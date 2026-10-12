/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Alpha emulation for QEMU - main translation routines.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/log.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/target_page.h"
#include "accel/tcg/cpu-loop.h"
#include "translate.h"
#include "cpu.h"
#include "ipregs.h"
#include "internals.h"
#include "instmap.h"
#include "syndrome.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

/* global register indexes */
static TCGv_i64 cpu_ir[32];
static TCGv_i64 cpu_ptir_ev5[32];
static TCGv_i64 cpu_ptir_ev6[32];
static TCGv_i64 cpu_fir[32];
static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_rc;
static TCGv_i64 cpu_llsc_addr;
static TCGv_i64 cpu_llsc_val;

/*
 * Include the generated decoders.
 */
#include "decode-insns.c.inc"

/* initialize TCG globals.  */
#define global_mem_new(field, name) \
    tcg_global_mem_new_i64(tcg_env, offsetof(CPUAlphaState, field), name)

static void alpha_translate_shadow_init(void)
{
    /*
     * EV5 and EV6 have distinctly different mappings for their PALshadow
     * registers. Handle that here.
     */
    cpu_ptir_ev5[IR_T7] = global_mem_new(pal_shadow[0], "pt7");
    cpu_ptir_ev5[IR_S0] = global_mem_new(pal_shadow[1], "ps0");
    cpu_ptir_ev5[IR_S1] = global_mem_new(pal_shadow[2], "ps1");
    cpu_ptir_ev5[IR_S2] = global_mem_new(pal_shadow[3], "ps2");
    cpu_ptir_ev5[IR_S3] = global_mem_new(pal_shadow[4], "ps3");
    cpu_ptir_ev5[IR_S4] = global_mem_new(pal_shadow[5], "ps4");
    cpu_ptir_ev5[IR_S5] = global_mem_new(pal_shadow[6], "ps5");
    cpu_ptir_ev5[IR_T11] = global_mem_new(pal_shadow[7], "pt1");

    cpu_ptir_ev6[IR_T3] = global_mem_new(pal_shadow[0], "pt3");
    cpu_ptir_ev6[IR_T4] = global_mem_new(pal_shadow[1], "pt4");
    cpu_ptir_ev6[IR_T5] = global_mem_new(pal_shadow[2], "pt5");
    cpu_ptir_ev6[IR_T6] = global_mem_new(pal_shadow[3], "pt6");
    cpu_ptir_ev6[IR_A4] = global_mem_new(pal_shadow[4], "pa4");
    cpu_ptir_ev6[IR_A5] = global_mem_new(pal_shadow[5], "pa5");
    cpu_ptir_ev6[IR_T8] = global_mem_new(pal_shadow[6], "pt8");
    cpu_ptir_ev6[IR_T9] = global_mem_new(pal_shadow[7], "pt9");
}

void alpha_translate_init(void)
{
    int i;

    for (i = 0; i < 32; i++) {
        TCGv_i64 reg = global_mem_new(gpregs[i], alpha_gregnames[i]);

        cpu_ir[i] = reg;
        cpu_ptir_ev5[i] = reg;
        cpu_ptir_ev6[i] = reg;
    }

    for (i = 0; i < 32; i++) {
        cpu_fir[i] = global_mem_new(fpregs[i], alpha_fregnames[i]);
    }

    cpu_pc = global_mem_new(pc, "pc");
    cpu_rc = global_mem_new(rc, "rc");
    cpu_llsc_addr = global_mem_new(llsc_addr, "llsc_addr");
    cpu_llsc_val = global_mem_new(llsc_val, "llsc_val");

    alpha_translate_shadow_init();
}
#undef global_mem_new

void alpha_translate_code(CPUState *cpu, TranslationBlock *tb,
                          int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc = {};
    const TranslatorOps *ops = &alpha_translator_ops;

    translator_loop(cpu, tb, max_insns, pc, host_pc, ops, &dc.base,
                    TCG_TYPE_VA);
}

static target_long jmp_diff(DisasContext *s, target_long diff)
{
    return (diff << 2) + curr_insn_len(s);
}

/*
 * Register access functions
 *
 * These functions are used for directly accessing a register in where
 * changes to the final register value are likely to be made. If you
 * need to use a register for temporary calculation (e.g. index type
 * operations) use the read_* form.
 */
static TCGv_i64 cpu_reg_shadow(DisasContext *s, int reg)
{
    switch (s->isa_implver) {
    case IMPLVER_21164:
        return cpu_ptir_ev5[reg];
    case IMPLVER_21264:
        return cpu_ptir_ev6[reg];
    default:
        g_assert_not_reached();
    }
}

static TCGv_i64 cpu_reg_pal_check(DisasContext *s, int reg, bool pal_check)
{
    if (likely(reg < 31)) {
        if (s->sde_enabled && pal_check) {
            return cpu_reg_shadow(s, reg);
        } else {
            return cpu_ir[reg];
        }
    } else {
        TCGv_i64 t = tcg_temp_new_i64();
        tcg_gen_movi_i64(t, 0);
        return t;
    }
}

static TCGv_i64 cpu_reg(DisasContext *s, int reg)
{
    return cpu_reg_pal_check(s, reg, s->pal_mode);
}

/* Access a GPR with banking and don't check PAL mode. */
static TCGv_i64 cpu_reg_forward(DisasContext *s, int reg)
{
    return cpu_reg_pal_check(s, reg, true);
}

static TCGv_i64 fpu_reg(DisasContext *s, int reg)
{
    if (likely(reg < 31)) {
        return cpu_fir[reg];
    } else {
        TCGv_i64 t = tcg_temp_new_i64();
        tcg_gen_movi_i64(t, 0);
        return t;
    }
}

static TCGv_i64 read_cpu_reg(DisasContext *s, int reg)
{
    TCGv_i64 v = tcg_temp_new_i64();

    if (likely(reg < 31)) {
        if (s->sde_enabled && s->pal_mode) {
            tcg_gen_mov_i64(v, cpu_reg_shadow(s, reg));
        } else {
            tcg_gen_mov_i64(v, cpu_ir[reg]);
        }
    } else {
        tcg_gen_movi_i64(v, 0);
    }
    return v;
}

static TCGv_i64 read_cpu_reg_lit(DisasContext *s, unsigned reg,
                                 uint8_t lit, bool islit)
{
    if (islit) {
        TCGv_i64 v = tcg_temp_new_i64();
        tcg_gen_movi_i64(v, lit);
        return v;
    } else {
        return read_cpu_reg(s, reg);
    }
}

static TCGv_i64 read_fpu_reg(DisasContext *s, int reg)
{
    TCGv_i64 v = tcg_temp_new_i64();

    if (likely(reg < 31)) {
        tcg_gen_mov_i64(v, cpu_fir[reg]);
    } else {
        tcg_gen_movi_i64(v, 0);
    }
    return v;
}

static void gen_pc_plus_diff(DisasContext *s, TCGv_i64 dest, target_long diff)
{
    assert(s->pc_save != -1);
    if (tb_cflags(s->base.tb) & CF_PCREL) {
        tcg_gen_addi_i64(dest, cpu_pc, (s->pc_curr - s->pc_save) + diff);
    } else {
        tcg_gen_movi_i64(dest, s->pc_curr + diff);
    }
}

static void gen_update_pc(DisasContext *s, target_long diff)
{
    gen_pc_plus_diff(s, cpu_pc, diff);
    s->pc_save = s->pc_curr + diff;
}

static void gen_set_pc(DisasContext *s, TCGv_i64 src)
{
    tcg_gen_mov_i64(cpu_pc, src);
    s->pc_save = -1;
}

static void gen_rebuild_hflags(DisasContext *s)
{
    gen_helper_rebuild_hflags(tcg_env);
}

static void gen_exception(int excp, uint32_t syndrome)
{
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(excp),
                               tcg_constant_i32(syndrome));
}

static void gen_exception_insn(DisasContext *s, target_long pc_diff,
                               int excp, uint32_t syndrome)
{
    gen_update_pc(s, pc_diff);
    gen_exception(excp, syndrome);
    s->base.is_jmp = DISAS_NORETURN;
}

static void gen_exception_internal(int excp, uint32_t syndrome)
{
    assert(excp_is_internal(excp));
    gen_helper_raise_exception_internal(tcg_env, tcg_constant_i32(excp),
                                        tcg_constant_i32(syndrome));
}

static void gen_exception_internal_insn(DisasContext *s, int excp,
                                        uint32_t syndrome)
{
    gen_update_pc(s, 0);
    gen_exception_internal(excp, syndrome);
    s->base.is_jmp = DISAS_NORETURN;
}

static void unallocated_encoding(DisasContext *s)
{
    gen_exception_insn(s, 0, EXCP_OPCDEC, syn_uncategorized());
}

static void gen_set_rounding_mode(DisasContext *s, int fn11)
{
    TCGv_i32 tmp;

    fn11 &= QUAL_RM_MASK;
    if (fn11 == s->tb_rm) {
        return;
    }

    tmp = tcg_temp_new_i32();
    s->tb_rm = fn11;
    switch (s->tb_rm) {
    case QUAL_RM_N:
        tcg_gen_movi_i32(tmp, float_round_nearest_even);
        break;
    case QUAL_RM_C:
        tcg_gen_movi_i32(tmp, float_round_to_zero);
        break;
    case QUAL_RM_M:
        tcg_gen_movi_i32(tmp, float_round_down);
        break;
    case QUAL_RM_D:
        tmp = load_cpu_field(fpcr_dyn_round);
        break;
    }

    gen_helper_set_rounding_mode(tcg_env, tmp);
}

/*
 * Fix: gen_set_flush_zero logic was completely inverted.
 * 
 * The /U qualifier on IEEE FP instructions enables underflow trapping.
 * When /U is SET (QUAL_U != 0):
 *   - Underflow traps ARE enabled
 *   - The instruction will raise an arithmetic trap on underflow
 *   - Results should NOT be flushed to zero silently; the trap
 *     handler decides what to do
 *   - Therefore flush_to_zero = 0
 *
 * When /U is CLEAR (QUAL_U == 0):
 *   - Underflow traps are disabled
 *   - Underflowed results may be silently flushed to zero per FPCR.UNDZ
 *   - Therefore flush_to_zero = fpcr_flush_to_zero (from FPCR.UNDZ)
 *
 * The original code had this backwards: it forced flush_to_zero=1 when
 * underflow traps were DISABLED (/U clear), and read FPCR when they were
 * ENABLED (/U set). This caused:
 *   - Instructions without /U to always flush to zero (ignoring FPCR.UNDZ)
 *   - Instructions with /U to sometimes flush when they should trap
 * Both behaviors are wrong and cause incorrect results in programs that
 * rely on proper underflow/denormal handling (Pinball physics, IE layout).
 */
static void gen_set_flush_zero(DisasContext *s, int fn11)
{
    TCGv_i32 tmp;

    fn11 &= QUAL_U;
    if (fn11 == s->tb_ftz) {
        return;
    }

    tmp = tcg_temp_new_i32();
    s->tb_ftz = fn11;
    if (s->tb_ftz) {
        /*
         * /U is set: underflow trapping is enabled.
         * Do not flush to zero - let the exception fire.
         */
        tcg_gen_movi_i32(tmp, 0);
    } else {
        /*
         * /U is clear: underflow trapping is disabled.
         * Use the FPCR.UNDZ setting to determine whether to flush.
         */
        tmp = load_cpu_field(fpcr_flush_to_zero);
    }

    gen_helper_set_flush_zero(tcg_env, tmp);
}

static void gen_load_fp(DisasContext *s, int ra, int rb, int32_t disp16,
                        LoadFpFn *func)
{
    TCGv_i64 addr;

    if (unlikely(ra == 31)) {
        return;
    }

    addr = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr, read_cpu_reg(s, rb), disp16);
    (*func)(s, fpu_reg(s, ra), addr);
}

static void gen_store_fp(DisasContext *s, int ra, int rb, int32_t disp16,
                         StoreFpFn *func)
{
    TCGv_i64 addr = tcg_temp_new_i64();

    tcg_gen_addi_i64(addr, read_cpu_reg(s, rb), disp16);
    (*func)(s, read_fpu_reg(s, ra), addr);
}

static inline uint64_t memop_mask(MemOp op)
{
    return ~((uint64_t)memop_size(op) - 1);
}

static void gen_load_locked(DisasContext *s, TCGv_i64 addr, TCGv_i64 dest,
                            int mmu_idx, MemOp memop)
{
    tcg_gen_qemu_ld_i64(dest, addr, mmu_idx, memop);
    tcg_gen_mov_i64(cpu_llsc_addr, addr);
    tcg_gen_mov_i64(cpu_llsc_val, dest);
#if 0
    tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
#endif
}

static void gen_load_int(DisasContext *s, int ra, int rb, int32_t disp16,
                         MemOp op, bool clear, bool locked)
{
    TCGv_i64 addr, dest;

    if (unlikely(ra == 31)) {
        return;
    }

    addr = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr, read_cpu_reg(s, rb), disp16);
    if (clear) {
        tcg_gen_andi_i64(addr, addr, memop_mask(op));
    }

    dest = cpu_reg(s, ra);
    if (locked) {
        gen_load_locked(s, addr, dest, get_mem_index(s), op);
    } else {
        tcg_gen_qemu_ld_i64(dest, addr, get_mem_index(s), op);
    }
}

static void gen_store_int(DisasContext *s, int ra, int rb, int32_t disp16,
                          MemOp op, bool clear)
{
    TCGv_i64 addr, src;

    addr = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr, read_cpu_reg(s, rb), disp16);
    if (clear) {
        tcg_gen_andi_i64(addr, addr, memop_mask(op));
    }

    src = read_cpu_reg(s, ra);
    tcg_gen_qemu_st_i64(src, addr, get_mem_index(s), op);
}

static void gen_store_conditional_common(DisasContext *s, TCGv_i64 val,
                                         TCGv_i64 addr, int mmu_idx,
                                         MemOp memop)
{
    TCGLabel *fail_label = gen_new_label();
    TCGLabel *done_label = gen_new_label();
    TCGv_i64 res, tmp;

    tcg_gen_brcond_i64(TCG_COND_NE, addr, cpu_llsc_addr, fail_label);

    /*
     * Zero extend the store comparison value based on the memory operation
     * size.
     */
    tmp = tcg_temp_new_i64();
    if (memop_size(memop) == 4) {
        tcg_gen_ext32u_i64(tmp, cpu_llsc_val);
    } else {
        tcg_gen_mov_i64(tmp, cpu_llsc_val);
    }

    /* Perform an atomic store. */
    res = tcg_temp_new_i64();
    tcg_gen_atomic_cmpxchg_i64(res, cpu_llsc_addr, tmp, val, mmu_idx, memop);
    tcg_gen_setcond_i64(TCG_COND_EQ, res, res, tmp);
    tcg_gen_mov_i64(val, res);
    tcg_gen_br(done_label);

    /* Failure case: address comparison failure */
    gen_set_label(fail_label);
#if 0
    tcg_gen_mb(TCG_BAR_STRL | TCG_MO_ALL);
#endif
    tcg_gen_movi_i64(val, 0);

    /* Clear the LLSC address. */
    gen_set_label(done_label);
    tcg_gen_movi_i64(cpu_llsc_addr, LLSC_ADDR_NONE);
}

static void gen_store_conditional(DisasContext *s, int ra, int rb,
                                  int32_t disp16, int mmu_idx, MemOp memop)
{
    TCGv_i64 addr, va, vb;

    va = cpu_reg(s, ra);
    vb = cpu_reg(s, rb);
    addr = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr, vb, disp16);
    gen_store_conditional_common(s, va, addr, mmu_idx, memop);
}

static void gen_lds(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr)
{
    TCGv_i32 tmp32 = tcg_temp_new_i32();

    tcg_gen_qemu_ld_i32(tmp32, addr, get_mem_index(s),
                        MO_LEUL | MO_ALIGN);
    gen_helper_ieee_lds(dest, tmp32);
}

static void gen_ldt(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr)
{
    tcg_gen_qemu_ld_i64(dest, addr, get_mem_index(s),
                        MO_LEUQ | MO_ALIGN);
}

static void gen_sts(DisasContext *s, TCGv_i64 src, TCGv_i64 addr)
{
    TCGv_i32 tmp32 = tcg_temp_new_i32();

    gen_helper_ieee_sts(tmp32, src);
    tcg_gen_qemu_st_i32(tmp32, addr, get_mem_index(s),
                        MO_LEUL | MO_ALIGN);
}

static void gen_stt(DisasContext *s, TCGv_i64 src, TCGv_i64 addr)
{
    tcg_gen_qemu_st_i64(src, addr, get_mem_index(s),
                        MO_LEUQ | MO_ALIGN);
}

/*
 * Fix: gen_ieee_unop was reading from the INTEGER register file
 * (read_cpu_reg) instead of the FLOATING POINT register file (read_fpu_reg).
 *
 * All unary IEEE FP operations (sqrts, sqrtt, cvtts, cvtst, cvttq,
 * cvtql, cvtqs, cvtqt) use FP register rb as their source operand.
 * Using read_cpu_reg reads from cpu_ir[] (the integer registers),
 * returning completely wrong data - whatever integer value happens to
 * be in register rb, not the floating point value.
 *
 * This is almost certainly the primary cause of crashes in Pinball
 * (sqrt operations on physics values) and Internet Explorer (float
 * conversions in layout/rendering code), as every single-operand
 * FP instruction was operating on garbage data.
 * Note that this was what was likely causing the FP crashes in Pinball
 * and IE prior to the patch
 */
static void gen_ieee_unop(DisasContext *s, IeeeUnaryFn *helper,
                          int rb, int rc, int fn11)
{
    TCGv_i64 vb, vc;
    TCGv_i32 qual;

    vb = read_fpu_reg(s, rb);          /* Fix #8: was read_cpu_reg */
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    gen_set_flush_zero(s, fn11);
    (*helper)(vc, tcg_env, vb, qual);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_ieee_binop(DisasContext *s, IeeeBinaryFn *helper,
                           int ra, int rb, int rc, int fn11)
{
    TCGv_i64 va, vb, vc;
    TCGv_i32 qual;

    va = read_fpu_reg(s, ra);
    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    gen_set_flush_zero(s, fn11);
    (*helper)(vc, tcg_env, va, vb, qual);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

/*
 * Fix: gen_ieee_compare was missing gen_set_flush_zero.
 *
 * Comparison instructions can receive denormal inputs. Without
 * configuring flush_to_zero to match the current FPCR/qualifier
 * state, the ieee_input() path in fpu_helper.c may behave
 * inconsistently when FPCR_DNZ is set - denormal inputs might
 * not be squashed to zero as expected, leading to incorrect
 * comparison results (e.g., NaN comparisons returning wrong
 * ordering, denormal values comparing unequal to zero when
 * they should compare equal after flushing).
 */
static void gen_ieee_compare(DisasContext *s, IeeeCompareFn *helper,
                             int ra, int rb, int rc, int fn11)
{
    TCGv_i64 va, vb, vc;
    TCGv_i32 qual;

    va = read_fpu_reg(s, ra);
    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    gen_set_flush_zero(s, fn11);        /* Fix #10: was missing */
    (*helper)(vc, tcg_env, va, vb, qual);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_ieee_convert(DisasContext *s, IeeeConvertFn *helper,
                             int rb, int rc, int fn11)
{
    TCGv_i64 vb, vc;
    TCGv_i32 qual;

    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    gen_set_flush_zero(s, fn11);
    (*helper)(vc, tcg_env, vb, qual);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

#define GEN_IEEE_UNOP(name)                                     \
static void glue(gen_ieee_, name)(DisasContext *s,              \
                                  int rb, int rc)               \
{                                                               \
    gen_ieee_unop(s, glue(gen_helper_ieee_, name),              \
                  rb, rc, get_fn11(s->insn));                   \
}
GEN_IEEE_UNOP(sqrts)
GEN_IEEE_UNOP(sqrtt)
GEN_IEEE_UNOP(cvtst)
GEN_IEEE_UNOP(cvtts)

#define GEN_IEEE_BINOP(name)                                    \
static void glue(gen_ieee_, name)(DisasContext *s,              \
                                  int ra, int rb, int rc)       \
{                                                               \
    gen_ieee_binop(s, glue(gen_helper_ieee_, name),             \
                   ra, rb, rc, get_fn11(s->insn));              \
}
GEN_IEEE_BINOP(adds)
GEN_IEEE_BINOP(subs)
GEN_IEEE_BINOP(muls)
GEN_IEEE_BINOP(divs)
GEN_IEEE_BINOP(addt)
GEN_IEEE_BINOP(subt)
GEN_IEEE_BINOP(mult)
GEN_IEEE_BINOP(divt)

#define GEN_IEEE_COMPARE(name)                                  \
static void glue(gen_ieee_, name)(DisasContext *s,              \
                                  int ra, int rb, int rc)       \
{                                                               \
    gen_ieee_compare(s, glue(gen_helper_ieee_, name),           \
                     ra, rb, rc, get_fn11(s->insn));            \
}
GEN_IEEE_COMPARE(cmptun)
GEN_IEEE_COMPARE(cmpteq)
GEN_IEEE_COMPARE(cmptlt)
GEN_IEEE_COMPARE(cmptle)

#define GEN_IEEE_CONVERT(name)                                  \
static void glue(gen_ieee_, name)(DisasContext *s,              \
                                  int rb, int rc)               \
{                                                               \
    gen_ieee_convert(s, glue(gen_helper_ieee_, name),           \
                     rb, rc, get_fn11(s->insn));                \
}
GEN_IEEE_CONVERT(cvtqs)
GEN_IEEE_CONVERT(cvtqt)

static void gen_ieee_cvtql(DisasContext *s, int rb, int rc, int qualifier)
{
    TCGv_i64 vb, vc;
    TCGv_i32 qual;

    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(qualifier);
    gen_helper_ieee_cvtql(vc, tcg_env, vb, qual);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_ieee_cvttq(DisasContext *s, int rb, int rc)
{
    TCGv_i64 vb, vc;
    TCGv_i32 qual;
    int fn11;

    /* No need to set flushzero, since we have an integer output. */
    fn11 = get_fn11(s->insn);
    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    gen_helper_ieee_cvttq(vc, tcg_env, vb, qual);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_ieee_cvtlq(TCGv_i64 vc, TCGv_i64 vb)
{
    TCGv_i64 tmp = tcg_temp_new_i64();

    tcg_gen_shri_i64(tmp, vb, 29);
    tcg_gen_sari_i64(vc, vb, 32);
    tcg_gen_deposit_i64(vc, vc, tmp, 0, 30);
}

static void gen_ieee_copymask(TCGv_i64 vc, TCGv_i64 va, TCGv_i64 vb, bool inv,
                              uint64_t mask)
{
    TCGv_i64 vmask = tcg_constant_i64(mask);
    TCGv_i64 tmp = tcg_temp_new_i64();

    if (inv) {
        tcg_gen_andc_i64(tmp, vmask, va);
    } else {
        tcg_gen_and_i64(tmp, va, vmask);
    }

    tcg_gen_andc_i64(vc, vb, vmask);
    tcg_gen_or_i64(vc, vc, tmp);
}

static void gen_ldf(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr)
{
    TCGv_i32 tmp32 = tcg_temp_new_i32();

    tcg_gen_qemu_ld_i32(tmp32, addr, get_mem_index(s),
                        MO_LEUL | MO_ALIGN);
    gen_helper_vax_ldf(dest, tmp32);
}

static void gen_ldg(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr)
{
    TCGv_i64 tmp64 = tcg_temp_new_i64();

    tcg_gen_qemu_ld_i64(tmp64, addr, get_mem_index(s),
                        MO_LEUQ | MO_ALIGN);
    gen_helper_vax_ldg(dest, tmp64);
}

static void gen_stf(DisasContext *s, TCGv_i64 src, TCGv_i64 addr)
{
    TCGv_i32 tmp32 = tcg_temp_new_i32();

    gen_helper_vax_stf(tmp32, src);
    tcg_gen_qemu_st_i32(tmp32, addr, get_mem_index(s),
                        MO_LEUL | MO_ALIGN);
}

static void gen_stg(DisasContext *s, TCGv_i64 src, TCGv_i64 addr)
{
    TCGv_i64 tmp64 = tcg_temp_new_i64();

    gen_helper_vax_stg(tmp64, src);
    tcg_gen_qemu_st_i64(tmp64, addr, get_mem_index(s),
                        MO_LEUQ | MO_ALIGN);
}

/*
 * Fix: gen_vax_unop had the same wrong register file bug as
 * gen_ieee_unop: it was using read_cpu_reg (integer registers)
 * instead of read_fpu_reg (FP registers) for the source operand.
 *
 * VAX FP unary operations (sqrtf, sqrtg) read their source from
 * an FP register. Using read_cpu_reg returned the integer register
 * value instead, producing completely wrong sqrt inputs.
 */
static void gen_vax_unop(DisasContext *s, VaxUnaryFn *helper,
                          int rb, int rc, int fn11)
{
    TCGv_i64 vb, vc;
    TCGv_i32 qual;

    vb = read_fpu_reg(s, rb);          /* Fix #9: was read_cpu_reg */
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    (*helper)(vc, tcg_env, vb);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_vax_binop(DisasContext *s, VaxBinaryFn *helper,
                           int ra, int rb, int rc, int fn11)
{
    TCGv_i64 va, vb, vc;
    TCGv_i32 qual;

    va = read_fpu_reg(s, ra);
    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    (*helper)(vc, tcg_env, va, vb);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_vax_compare(DisasContext *s, VaxCompareFn *helper,
                             int ra, int rb, int rc, int fn11)
{
    TCGv_i64 va, vb, vc;
    TCGv_i32 qual;

    va = read_fpu_reg(s, ra);
    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    (*helper)(vc, tcg_env, va, vb);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

static void gen_vax_convert(DisasContext *s, VaxConvertFn *helper,
                             int rb, int rc, int fn11)
{
    TCGv_i64 vb, vc;
    TCGv_i32 qual;

    vb = read_fpu_reg(s, rb);
    vc = fpu_reg(s, rc);
    qual = tcg_constant_i32(fn11 & QUAL_MASK);
    gen_set_rounding_mode(s, fn11);
    (*helper)(vc, tcg_env, vb);
    gen_helper_check_float_exceptions(tcg_env, qual);
}

#define GEN_VAX_UNOP(name)                                      \
static void glue(gen_vax_, name)(DisasContext *s,               \
                                 int rb, int rc)                \
{                                                               \
    gen_vax_unop(s, glue(gen_helper_vax_, name),                \
                 rb, rc, get_fn11(s->insn));                    \
}
GEN_VAX_UNOP(sqrtf)
GEN_VAX_UNOP(sqrtg)

#define GEN_VAX_BINOP(name)                                     \
static void glue(gen_vax_, name)(DisasContext *s,               \
                                 int ra, int rb, int rc)        \
{                                                               \
    gen_vax_binop(s, glue(gen_helper_vax_, name),               \
                  ra, rb, rc, get_fn11(s->insn));               \
}
GEN_VAX_BINOP(addf)
GEN_VAX_BINOP(subf)
GEN_VAX_BINOP(mulf)
GEN_VAX_BINOP(divf)
GEN_VAX_BINOP(addg)
GEN_VAX_BINOP(subg)
GEN_VAX_BINOP(mulg)
GEN_VAX_BINOP(divg)

#define GEN_VAX_COMPARE(name)                                   \
static void glue(gen_vax_, name)(DisasContext *s,               \
                                 int ra, int rb, int rc)        \
{                                                               \
    gen_vax_compare(s, glue(gen_helper_vax_, name),             \
                    ra, rb, rc, get_fn11(s->insn));             \
}
GEN_VAX_COMPARE(cmpgeq)
GEN_VAX_COMPARE(cmpglt)
GEN_VAX_COMPARE(cmpgle)

#define GEN_VAX_CONVERT(name)                                   \
static void glue(gen_vax_, name)(DisasContext *s,               \
                                 int rb, int rc)                \
{                                                               \
    gen_vax_convert(s, glue(gen_helper_vax_, name),             \
                    rb, rc, get_fn11(s->insn));                 \
}
GEN_VAX_CONVERT(cvtqf)
GEN_VAX_CONVERT(cvtgf)
GEN_VAX_CONVERT(cvtqg)
GEN_VAX_CONVERT(cvtgq)
GEN_VAX_CONVERT(cvtdg)
GEN_VAX_CONVERT(cvtgd)

typedef struct AlphaHWLDSTParams {
    AlphaMMUIdx mmu_idx;
    bool locked;
} AlphaHWLDSTParams;

/* EV4 PHY+ALT selects physical load-lock/store-conditional; RWC is ignored. */
static const AlphaHWLDSTParams ev4_hw_ld_mmu_idx[8] = {
    [0b000] = { AlphaMMUIdx_Privileged,         false },
    [0b001] = { AlphaMMUIdx_PrivilegedWChk,     false },
    [0b010] = { AlphaMMUIdx_AltMode,            false },
    [0b011] = { AlphaMMUIdx_AltModeWChk,        false },
    [0b100] = { AlphaMMUIdx_Physical,           false },
    [0b101] = { AlphaMMUIdx_Physical,           false },
    [0b110] = { AlphaMMUIdx_Physical,           true },
    [0b111] = { AlphaMMUIdx_Physical,           true },
};

static const AlphaHWLDSTParams ev4_hw_st_mmu_idx[8] = {
    [0b000] = { AlphaMMUIdx_Privileged,         false },
    [0b001] = { AlphaMMUIdx_Privileged,         false },
    [0b010] = { AlphaMMUIdx_AltMode,            false },
    [0b011] = { AlphaMMUIdx_AltMode,            false },
    [0b100] = { AlphaMMUIdx_Physical,           false },
    [0b101] = { AlphaMMUIdx_Physical,           false },
    [0b110] = { AlphaMMUIdx_Physical,           true },
    [0b111] = { AlphaMMUIdx_Physical,           true },
};

static const AlphaHWLDSTParams ev5_hw_ld_mmu_idx[32] = {
    [0b00000] = { AlphaMMUIdx_Privileged,       false },
    [0b00001] = { AlphaMMUIdx_Privileged,       true },
    [0b00010] = { AlphaMMUIdx_PrivilegedVPTE,   false },
    [0b00011] = { AlphaMMUIdx_PrivilegedVPTE,   true },
    [0b00100] = { AlphaMMUIdx_PrivilegedWChk,   false },
    [0b00101] = { AlphaMMUIdx_PrivilegedWChk,   true },
    [0b00110] = { AlphaMMUIdx_PrivilegedWChk,   false },
    [0b00111] = { AlphaMMUIdx_PrivilegedWChk,   true },
    [0b01000] = { AlphaMMUIdx_AltMode,          false },
    [0b01001] = { AlphaMMUIdx_AltMode,          true },
    [0b01010] = { AlphaMMUIdx_AltMode,          false },
    [0b01011] = { AlphaMMUIdx_AltMode,          true },
    [0b01100] = { AlphaMMUIdx_AltModeWChk,      false },
    [0b01101] = { AlphaMMUIdx_AltModeWChk,      true },
    [0b01110] = { AlphaMMUIdx_AltModeWChk,      false },
    [0b01111] = { AlphaMMUIdx_AltModeWChk,      true },
    [0b10000] = { AlphaMMUIdx_Physical,         false },
    [0b10001] = { AlphaMMUIdx_Physical,         true },
    [0b10010] = { AlphaMMUIdx_Physical,         false },
    [0b10011] = { AlphaMMUIdx_Physical,         true },
    [0b10100] = { AlphaMMUIdx_Physical,         false },
    [0b10101] = { AlphaMMUIdx_Physical,         true },
    [0b10110] = { AlphaMMUIdx_Physical,         false },
    [0b10111] = { AlphaMMUIdx_Physical,         true },
    [0b11000] = { AlphaMMUIdx_Physical,         false },
    [0b11001] = { AlphaMMUIdx_Physical,         true },
    [0b11010] = { AlphaMMUIdx_Physical,         false },
    [0b11011] = { AlphaMMUIdx_Physical,         true },
    [0b11100] = { AlphaMMUIdx_Physical,         false },
    [0b11101] = { AlphaMMUIdx_Physical,         true },
    [0b11110] = { AlphaMMUIdx_Physical,         false },
    [0b11111] = { AlphaMMUIdx_Physical,         true },
};


static const AlphaHWLDSTParams ev5_hw_st_mmu_idx[32] = {
    [0b00000] = { AlphaMMUIdx_Privileged,       false },
    [0b00001] = { AlphaMMUIdx_Privileged,       true },
    [0b00010] = { AlphaMMUIdx_Privileged,       false },
    [0b00011] = { AlphaMMUIdx_Privileged,       true },
    [0b00100] = { AlphaMMUIdx_Privileged,       false },
    [0b00101] = { AlphaMMUIdx_Privileged,       true },
    [0b00110] = { AlphaMMUIdx_Privileged,       false },
    [0b00111] = { AlphaMMUIdx_Privileged,       true },
    [0b01000] = { AlphaMMUIdx_AltMode,          false },
    [0b01001] = { AlphaMMUIdx_AltMode,          true },
    [0b01010] = { AlphaMMUIdx_AltMode,          false },
    [0b01011] = { AlphaMMUIdx_AltMode,          true },
    [0b01100] = { AlphaMMUIdx_AltMode,          false },
    [0b01101] = { AlphaMMUIdx_AltMode,          true },
    [0b01110] = { AlphaMMUIdx_AltMode,          false },
    [0b01111] = { AlphaMMUIdx_AltMode,          true },
    [0b10000] = { AlphaMMUIdx_Physical,         false },
    [0b10001] = { AlphaMMUIdx_Physical,         true },
    [0b10010] = { AlphaMMUIdx_Physical,         false },
    [0b10011] = { AlphaMMUIdx_Physical,         true },
    [0b10100] = { AlphaMMUIdx_Physical,         false },
    [0b10101] = { AlphaMMUIdx_Physical,         true },
    [0b10110] = { AlphaMMUIdx_Physical,         false },
    [0b10111] = { AlphaMMUIdx_Physical,         true },
    [0b11000] = { AlphaMMUIdx_Physical,         false },
    [0b11001] = { AlphaMMUIdx_Physical,         true },
    [0b11010] = { AlphaMMUIdx_Physical,         false },
    [0b11011] = { AlphaMMUIdx_Physical,         true },
    [0b11100] = { AlphaMMUIdx_Physical,         false },
    [0b11101] = { AlphaMMUIdx_Physical,         true },
    [0b11110] = { AlphaMMUIdx_Physical,         false },
    [0b11111] = { AlphaMMUIdx_Physical,         true },
};

/*
 * Data for expanding a zapnot mask.
 *
 *  for (i = 0; i < 256; ++i) {
 *      unsigned long m = 0;
 *      for (j = 0; j < 8; j++) {
 *          if ((i >> j) & 1) {
 *              m |= 0xfful << (j << 3);
 *          }
 *      }
 *      printf("0x%016lx,\n", m);
 *  }
 */
static const uint64_t zapnot_mask_data[256] = {
    0x0000000000000000, 0x00000000000000ff, 0x000000000000ff00,
    0x000000000000ffff, 0x0000000000ff0000, 0x0000000000ff00ff,
    0x0000000000ffff00, 0x0000000000ffffff, 0x00000000ff000000,
    0x00000000ff0000ff, 0x00000000ff00ff00, 0x00000000ff00ffff,
    0x00000000ffff0000, 0x00000000ffff00ff, 0x00000000ffffff00,
    0x00000000ffffffff, 0x000000ff00000000, 0x000000ff000000ff,
    0x000000ff0000ff00, 0x000000ff0000ffff, 0x000000ff00ff0000,
    0x000000ff00ff00ff, 0x000000ff00ffff00, 0x000000ff00ffffff,
    0x000000ffff000000, 0x000000ffff0000ff, 0x000000ffff00ff00,
    0x000000ffff00ffff, 0x000000ffffff0000, 0x000000ffffff00ff,
    0x000000ffffffff00, 0x000000ffffffffff, 0x0000ff0000000000,
    0x0000ff00000000ff, 0x0000ff000000ff00, 0x0000ff000000ffff,
    0x0000ff0000ff0000, 0x0000ff0000ff00ff, 0x0000ff0000ffff00,
    0x0000ff0000ffffff, 0x0000ff00ff000000, 0x0000ff00ff0000ff,
    0x0000ff00ff00ff00, 0x0000ff00ff00ffff, 0x0000ff00ffff0000,
    0x0000ff00ffff00ff, 0x0000ff00ffffff00, 0x0000ff00ffffffff,
    0x0000ffff00000000, 0x0000ffff000000ff, 0x0000ffff0000ff00,
    0x0000ffff0000ffff, 0x0000ffff00ff0000, 0x0000ffff00ff00ff,
    0x0000ffff00ffff00, 0x0000ffff00ffffff, 0x0000ffffff000000,
    0x0000ffffff0000ff, 0x0000ffffff00ff00, 0x0000ffffff00ffff,
    0x0000ffffffff0000, 0x0000ffffffff00ff, 0x0000ffffffffff00,
    0x0000ffffffffffff, 0x00ff000000000000, 0x00ff0000000000ff,
    0x00ff00000000ff00, 0x00ff00000000ffff, 0x00ff000000ff0000,
    0x00ff000000ff00ff, 0x00ff000000ffff00, 0x00ff000000ffffff,
    0x00ff0000ff000000, 0x00ff0000ff0000ff, 0x00ff0000ff00ff00,
    0x00ff0000ff00ffff, 0x00ff0000ffff0000, 0x00ff0000ffff00ff,
    0x00ff0000ffffff00, 0x00ff0000ffffffff, 0x00ff00ff00000000,
    0x00ff00ff000000ff, 0x00ff00ff0000ff00, 0x00ff00ff0000ffff,
    0x00ff00ff00ff0000, 0x00ff00ff00ff00ff, 0x00ff00ff00ffff00,
    0x00ff00ff00ffffff, 0x00ff00ffff000000, 0x00ff00ffff0000ff,
    0x00ff00ffff00ff00, 0x00ff00ffff00ffff, 0x00ff00ffffff0000,
    0x00ff00ffffff00ff, 0x00ff00ffffffff00, 0x00ff00ffffffffff,
    0x00ffff0000000000, 0x00ffff00000000ff, 0x00ffff000000ff00,
    0x00ffff000000ffff, 0x00ffff0000ff0000, 0x00ffff0000ff00ff,
    0x00ffff0000ffff00, 0x00ffff0000ffffff, 0x00ffff00ff000000,
    0x00ffff00ff0000ff, 0x00ffff00ff00ff00, 0x00ffff00ff00ffff,
    0x00ffff00ffff0000, 0x00ffff00ffff00ff, 0x00ffff00ffffff00,
    0x00ffff00ffffffff, 0x00ffffff00000000, 0x00ffffff000000ff,
    0x00ffffff0000ff00, 0x00ffffff0000ffff, 0x00ffffff00ff0000,
    0x00ffffff00ff00ff, 0x00ffffff00ffff00, 0x00ffffff00ffffff,
    0x00ffffffff000000, 0x00ffffffff0000ff, 0x00ffffffff00ff00,
    0x00ffffffff00ffff, 0x00ffffffffff0000, 0x00ffffffffff00ff,
    0x00ffffffffffff00, 0x00ffffffffffffff, 0xff00000000000000,
    0xff000000000000ff, 0xff0000000000ff00, 0xff0000000000ffff,
    0xff00000000ff0000, 0xff00000000ff00ff, 0xff00000000ffff00,
    0xff00000000ffffff, 0xff000000ff000000, 0xff000000ff0000ff,
    0xff000000ff00ff00, 0xff000000ff00ffff, 0xff000000ffff0000,
    0xff000000ffff00ff, 0xff000000ffffff00, 0xff000000ffffffff,
    0xff0000ff00000000, 0xff0000ff000000ff, 0xff0000ff0000ff00,
    0xff0000ff0000ffff, 0xff0000ff00ff0000, 0xff0000ff00ff00ff,
    0xff0000ff00ffff00, 0xff0000ff00ffffff, 0xff0000ffff000000,
    0xff0000ffff0000ff, 0xff0000ffff00ff00, 0xff0000ffff00ffff,
    0xff0000ffffff0000, 0xff0000ffffff00ff, 0xff0000ffffffff00,
    0xff0000ffffffffff, 0xff00ff0000000000, 0xff00ff00000000ff,
    0xff00ff000000ff00, 0xff00ff000000ffff, 0xff00ff0000ff0000,
    0xff00ff0000ff00ff, 0xff00ff0000ffff00, 0xff00ff0000ffffff,
    0xff00ff00ff000000, 0xff00ff00ff0000ff, 0xff00ff00ff00ff00,
    0xff00ff00ff00ffff, 0xff00ff00ffff0000, 0xff00ff00ffff00ff,
    0xff00ff00ffffff00, 0xff00ff00ffffffff, 0xff00ffff00000000,
    0xff00ffff000000ff, 0xff00ffff0000ff00, 0xff00ffff0000ffff,
    0xff00ffff00ff0000, 0xff00ffff00ff00ff, 0xff00ffff00ffff00,
    0xff00ffff00ffffff, 0xff00ffffff000000, 0xff00ffffff0000ff,
    0xff00ffffff00ff00, 0xff00ffffff00ffff, 0xff00ffffffff0000,
    0xff00ffffffff00ff, 0xff00ffffffffff00, 0xff00ffffffffffff,
    0xffff000000000000, 0xffff0000000000ff, 0xffff00000000ff00,
    0xffff00000000ffff, 0xffff000000ff0000, 0xffff000000ff00ff,
    0xffff000000ffff00, 0xffff000000ffffff, 0xffff0000ff000000,
    0xffff0000ff0000ff, 0xffff0000ff00ff00, 0xffff0000ff00ffff,
    0xffff0000ffff0000, 0xffff0000ffff00ff, 0xffff0000ffffff00,
    0xffff0000ffffffff, 0xffff00ff00000000, 0xffff00ff000000ff,
    0xffff00ff0000ff00, 0xffff00ff0000ffff, 0xffff00ff00ff0000,
    0xffff00ff00ff00ff, 0xffff00ff00ffff00, 0xffff00ff00ffffff,
    0xffff00ffff000000, 0xffff00ffff0000ff, 0xffff00ffff00ff00,
    0xffff00ffff00ffff, 0xffff00ffffff0000, 0xffff00ffffff00ff,
    0xffff00ffffffff00, 0xffff00ffffffffff, 0xffffff0000000000,
    0xffffff00000000ff, 0xffffff000000ff00, 0xffffff000000ffff,
    0xffffff0000ff0000, 0xffffff0000ff00ff, 0xffffff0000ffff00,
    0xffffff0000ffffff, 0xffffff00ff000000, 0xffffff00ff0000ff,
    0xffffff00ff00ff00, 0xffffff00ff00ffff, 0xffffff00ffff0000,
    0xffffff00ffff00ff, 0xffffff00ffffff00, 0xffffff00ffffffff,
    0xffffffff00000000, 0xffffffff000000ff, 0xffffffff0000ff00,
    0xffffffff0000ffff, 0xffffffff00ff0000, 0xffffffff00ff00ff,
    0xffffffff00ffff00, 0xffffffff00ffffff, 0xffffffffff000000,
    0xffffffffff0000ff, 0xffffffffff00ff00, 0xffffffffff00ffff,
    0xffffffffffff0000, 0xffffffffffff00ff, 0xffffffffffffff00,
    0xffffffffffffffff,
};

static inline uint64_t zapnot_mask(uint8_t lit)
{
    return zapnot_mask_data[lit];
}

/*
 * Implement zapnot with an immediate operand, which expands to some
 * form of immediate AND.  This is a basic building block in the
 * definition of many of the other byte manipulation instructions.
 */
static void gen_zapnoti(TCGv_i64 dest, TCGv_i64 src, uint8_t lit)
{
    switch (lit) {
    case 0x00:
        tcg_gen_movi_i64(dest, 0);
        break;
    case 0x01:
        tcg_gen_ext8u_i64(dest, src);
        break;
    case 0x03:
        tcg_gen_ext16u_i64(dest, src);
        break;
    case 0x0f:
        tcg_gen_ext32u_i64(dest, src);
        break;
    case 0xff:
        tcg_gen_mov_i64(dest, src);
        break;
    default:
        tcg_gen_andi_i64(dest, src, zapnot_mask(lit));
        break;
    }
}

/* EXTWH, EXTLH, EXTQH */
static void gen_ext_h(DisasContext *s, TCGv_i64 vc, TCGv_i64 va, int rb,
                      bool islit, uint8_t lit, uint8_t byte_mask)
{
    if (islit) {
        int pos = (64 - lit * 8) & 0x3f;
        int len = cto32(byte_mask) * 8;
        if (pos < len) {
            tcg_gen_deposit_z_i64(vc, va, pos, len - pos);
        } else {
            tcg_gen_movi_i64(vc, 0);
        }
    } else {
        TCGv_i64 tmp = tcg_temp_new_i64();
        tcg_gen_shli_i64(tmp, read_cpu_reg(s, rb), 3);
        tcg_gen_neg_i64(tmp, tmp);
        tcg_gen_andi_i64(tmp, tmp, 0x3f);
        tcg_gen_shl_i64(vc, va, tmp);
    }
    gen_zapnoti(vc, vc, byte_mask);
}

/* EXTBL, EXTWL, EXTLL, EXTQL */
static void gen_ext_l(DisasContext *s, TCGv_i64 vc, TCGv_i64 va, int rb,
                      bool islit, uint8_t lit, uint8_t byte_mask)
{
    if (islit) {
        int pos = (lit & 7) * 8;
        int len = cto32(byte_mask) * 8;
        if (pos + len >= 64) {
            len = 64 - pos;
        }
        tcg_gen_extract_i64(vc, va, pos, len);
    } else {
        TCGv_i64 tmp = tcg_temp_new_i64();
        tcg_gen_andi_i64(tmp, read_cpu_reg(s, rb), 7);
        tcg_gen_shli_i64(tmp, tmp, 3);
        tcg_gen_shr_i64(vc, va, tmp);
        gen_zapnoti(vc, vc, byte_mask);
    }
}

/* INSWH, INSLH, INSQH */
static void gen_ins_h(DisasContext *s, TCGv_i64 vc, TCGv_i64 va, int rb,
                      bool islit, uint8_t lit, uint8_t byte_mask)
{
    if (islit) {
        int pos = 64 - (lit & 7) * 8;
        int len = cto32(byte_mask) * 8;
        if (pos < len) {
            tcg_gen_extract_i64(vc, va, pos, len - pos);
        } else {
            tcg_gen_movi_i64(vc, 0);
        }
    } else {
        TCGv_i64 tmp = tcg_temp_new_i64();
        TCGv_i64 shift = tcg_temp_new_i64();

        /*
         * The instruction description has us left-shift the byte mask
         * and extract bits <15:8> and apply that zap at the end.  This
         * is equivalent to simply performing the zap first and shifting
         * afterward.
         */
        gen_zapnoti(tmp, va, byte_mask);

        /*
         * If (B & 7) == 0, we need to shift by 64 and leave a zero.  Do this
         * portably by splitting the shift into two parts: shift_count-1 and 1.
         * Arrange for the -1 by using ones-complement instead of
         * twos-complement in the negation: ~(B * 8) & 63.
         */

        tcg_gen_shli_i64(shift, read_cpu_reg(s, rb), 3);
        tcg_gen_not_i64(shift, shift);
        tcg_gen_andi_i64(shift, shift, 0x3f);

        tcg_gen_shr_i64(vc, tmp, shift);
        tcg_gen_shri_i64(vc, vc, 1);
    }
}

/* INSBL, INSWL, INSLL, INSQL */
static void gen_ins_l(DisasContext *s, TCGv_i64 vc, TCGv_i64 va, int rb,
                      bool islit, uint8_t lit, uint8_t byte_mask)
{
    if (islit) {
        int pos = (lit & 7) * 8;
        int len = cto32(byte_mask) * 8;
        if (pos + len > 64) {
            len = 64 - pos;
        }
        tcg_gen_deposit_z_i64(vc, va, pos, len);
    } else {
        TCGv_i64 tmp = tcg_temp_new_i64();
        TCGv_i64 shift = tcg_temp_new_i64();

        /*
         * The instruction description has us left-shift the byte mask
         * and extract bits <15:8> and apply that zap at the end.  This
         * is equivalent to simply performing the zap first and shifting
         * afterward.
         */
        gen_zapnoti(tmp, va, byte_mask);

        tcg_gen_andi_i64(shift, read_cpu_reg(s, rb), 7);
        tcg_gen_shli_i64(shift, shift, 3);
        tcg_gen_shl_i64(vc, tmp, shift);
    }
}

/* MSKWH, MSKLH, MSKQH */
static void gen_msk_h(DisasContext *s, TCGv_i64 vc, TCGv_i64 va, int rb,
                      bool islit, uint8_t lit, uint8_t byte_mask)
{
    if (islit) {
        gen_zapnoti(vc, va, ~((byte_mask << (lit & 7)) >> 8));
    } else {
        TCGv_i64 shift = tcg_temp_new_i64();
        TCGv_i64 mask = tcg_temp_new_i64();

        /*
         * The instruction description is as above, where the byte_mask
         * is shifted left, and then we extract bits <15:8>.  This can be
         * emulated with a right-shift on the expanded byte mask.  This
         * requires extra care because for an input <2:0> == 0 we need a
         * shift of 64 bits in order to generate a zero.  This is done by
         * splitting the shift into two parts, the variable shift - 1
         * followed by a constant 1 shift.  The code we expand below is
         * equivalent to ~(B * 8) & 63.
         */

        tcg_gen_shli_i64(shift, read_cpu_reg(s, rb), 3);
        tcg_gen_not_i64(shift, shift);
        tcg_gen_andi_i64(shift, shift, 0x3f);
        tcg_gen_movi_i64(mask, zapnot_mask(byte_mask));
        tcg_gen_shr_i64(mask, mask, shift);
        tcg_gen_shri_i64(mask, mask, 1);

        tcg_gen_andc_i64(vc, va, mask);
    }
}

/* MSKBL, MSKWL, MSKLL, MSKQL */
static void gen_msk_l(DisasContext *s, TCGv_i64 vc, TCGv_i64 va, int rb,
                      bool islit, uint8_t lit, uint8_t byte_mask)
{
    if (islit) {
        gen_zapnoti(vc, va, ~(byte_mask << (lit & 7)));
    } else {
        TCGv_i64 shift = tcg_temp_new_i64();
        TCGv_i64 mask = tcg_temp_new_i64();

        tcg_gen_andi_i64(shift, read_cpu_reg(s, rb), 7);
        tcg_gen_shli_i64(shift, shift, 3);
        tcg_gen_movi_i64(mask, zapnot_mask(byte_mask));
        tcg_gen_shl_i64(mask, mask, shift);

        tcg_gen_andc_i64(vc, va, mask);
    }
}

static inline bool use_goto_tb(DisasContext *s, uint64_t dest)
{
    return translator_use_goto_tb(&s->base, dest);
}

static void gen_goto_ptr(DisasContext *s)
{
    tcg_gen_lookup_and_goto_ptr();
}

static void gen_goto_tb(DisasContext *s, int idx, int64_t diff)
{
    if (use_goto_tb(s, s->pc_curr + diff)) {
        /*
         * For pcrel, the pc must always be up-to-date on entry to
         * the linked TB, so that it can use simple additions for all
         * further adjustments.  For !pcrel, the linked TB is compiled
         * to know its full virtual address, so we can delay the
         * update to pc to the unlinked path.  A long chain of links
         * can thus avoid many updates to the PC.
         */
        if (tb_cflags(s->base.tb) & CF_PCREL) {
            gen_update_pc(s, diff);
            tcg_gen_goto_tb(idx);
        } else {
            tcg_gen_goto_tb(idx);
            gen_update_pc(s, diff);
        }
        tcg_gen_exit_tb(s->base.tb, idx);
    } else {
        gen_update_pc(s, diff);
        gen_goto_ptr(s);
    }
    s->base.is_jmp = DISAS_NORETURN;
}

/* Force a TB lookup after an instruction that changes the CPU state. */
static void gen_lookup_tb(DisasContext *s)
{
    gen_goto_tb(s, 0, curr_insn_len(s));
}

static void gen_hw_rei(DisasContext *s, TCGv_i64 addr)
{
    DisasLabel skip_exc = gen_disas_label(s);
    TCGv_i64 tmp = tcg_temp_new_i64();

    tcg_gen_andi_i64(tmp, addr, 1);
    tcg_gen_brcondi_i64(TCG_COND_EQ, tmp, s->pal_mode, skip_exc.label);

    /* Failure case, mode mismatch: generate exception. */
    tcg_gen_st_i64(addr, tcg_env,
                   offsetof(CPUAlphaState, exception.vaddress));
    gen_exception_internal_insn(s, EXCP_HW_REI, syn_uncategorized());

    /*
     * Success case, mode unchanged: allow for TB linkage.
     *
     * This is most useful for when we have double fault cases inside the
     * Dstream TB miss handlers.
     */
    set_disas_label(s, skip_exc);
    tcg_gen_andi_i64(addr, addr, UINT64_C(~0x03));
    gen_set_pc(s, addr);
    gen_goto_ptr(s);
    s->base.is_jmp = DISAS_NORETURN;
}

static bool fp_access_check(DisasContext *s)
{
    if (unlikely(!s->fp_enabled)) {
        gen_exception_insn(s, 0, EXCP_FEN, syn_uncategorized());
        return false;
    }
    return true;
}

static bool palres_access_check(DisasContext *s)
{
    if ((s->normal_mmu_idx == AlphaMMUIdx_Kernel && s->hwe_enabled) ||
        (s->pal_mode)) {
        return true;
    }
    unallocated_encoding(s);
    return false;
}

static void gen_bdirect(DisasContext *s, int ra, int32_t disp)
{
    gen_pc_plus_diff(s, cpu_reg(s, ra), curr_insn_len(s));
    gen_goto_tb(s, 0, jmp_diff(s, disp));
}

static void gen_bcond_internal(DisasContext *s, TCGCond cond,
                               TCGv_i64 cmp, uint64_t imm, int32_t disp)
{
    DisasLabel match;

    match = gen_disas_label(s);
    tcg_gen_brcondi_i64(cond, cmp, imm, match.label);
    gen_goto_tb(s, 0, curr_insn_len(s));
    set_disas_label(s, match);
    gen_goto_tb(s, 1, jmp_diff(s, disp));
}

static void gen_bcond(DisasContext *s, TCGCond cond, int ra, int32_t disp)
{
    gen_bcond_internal(s, cond, cpu_reg(s, ra), is_tst_cond(cond), disp);
}

/* Fold -0.0 for comparison with COND.  */
static TCGv_i64 gen_fold_mzero(TCGCond *pcond, uint64_t *pimm, TCGv_i64 src)
{
    TCGv_i64 tmp;

    *pimm = 0;
    switch (*pcond) {
    case TCG_COND_LE:
    case TCG_COND_GT:
        /* For <= or >, the -0.0 value directly compares the way we want.  */
        return src;

    case TCG_COND_EQ:
    case TCG_COND_NE:
        /* For == or !=, we can compare without the sign bit. */
        *pcond = *pcond == TCG_COND_EQ ? TCG_COND_TSTEQ : TCG_COND_TSTNE;
        *pimm = INT64_MAX;
        return src;

    case TCG_COND_GE:
    case TCG_COND_LT:
        /* For >= or <, map -0.0 to +0.0. */
        tmp = tcg_temp_new_i64();
        tcg_gen_movcond_i64(TCG_COND_EQ, tmp,
                            src, tcg_constant_i64(INT64_MIN),
                            tcg_constant_i64(0), src);
        return tmp;

    default:
        g_assert_not_reached();
    }
}

static void gen_fbcond(DisasContext *s, TCGCond cond, int ra, int32_t disp)
{
    TCGv_i64 tmp;
    uint64_t imm;

    tmp = gen_fold_mzero(&cond, &imm, read_fpu_reg(s, ra));
    gen_bcond_internal(s, cond, tmp, imm, disp);
}

static void gen_fcmov(DisasContext *s, TCGCond cond, int ra, int rb, int rc)
{
    TCGv_i64 tmp;
    uint64_t imm;

    tmp = gen_fold_mzero(&cond, &imm, read_fpu_reg(s, ra));
    tcg_gen_movcond_i64(cond, fpu_reg(s, rc),
                        tmp, tcg_constant_i64(imm),
                        read_fpu_reg(s, rb),
                        read_fpu_reg(s, rc));
}

static void gen_hw_ld_ev4(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr,
                          int type, bool quad)
{
    AlphaMMUIdx mmu_idx = ev4_hw_ld_mmu_idx[type & 0b111].mmu_idx;
    bool locked = ev4_hw_ld_mmu_idx[type & 0b111].locked;
    MemOp memop = quad ? MO_LEUQ : MO_LESL;

    tcg_gen_andi_i64(addr, addr, memop_mask(memop));
    if (locked) {
        gen_load_locked(s, addr, dest, mmu_idx, memop);
    } else {
        tcg_gen_qemu_ld_i64(dest, addr, mmu_idx, memop);
    }
}

static void gen_hw_ld_ev5(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr,
                          int type, bool quad)
{
    AlphaMMUIdx mmu_idx = ev5_hw_ld_mmu_idx[type & 0b11111].mmu_idx;
    bool locked = ev5_hw_ld_mmu_idx[type & 0b11111].locked;
    MemOp memop = quad ? MO_LEUQ : MO_LESL;

    tcg_gen_andi_i64(addr, addr, memop_mask(memop));
    if (locked) {
        gen_load_locked(s, addr, dest, mmu_idx, memop);
    } else {
        tcg_gen_qemu_ld_i64(dest, addr, mmu_idx, memop);
    }
}

static void gen_hw_ld_ev6(DisasContext *s, TCGv_i64 dest, TCGv_i64 addr,
                          int type, bool quad)
{
    AlphaMMUIdx mmu_idx;
    MemOp memop;
    bool locked;

    memop = quad ? MO_LEUQ : MO_LESL;
    switch (type) {
    case EV6_HW_LD_PHYS:
        /* Physical access (hw_ld{l,q}/p) */
        mmu_idx = AlphaMMUIdx_Physical;
        locked = false;
        break;
    case EV6_HW_LD_PHYS_LOCK:
        /* Longword physical access with lock (hw_ld{l,q}_l/p) */
        mmu_idx = AlphaMMUIdx_Physical;
        locked = true;
        break;
    case EV6_HW_LD_VPTE:
        /* Virtual PTE fetch (hw_ld{l,q}/v) */
        mmu_idx = AlphaMMUIdx_PrivilegedVPTE;
        locked = false;
        break;
    case EV6_HW_LD_VIRT:
        /* Virtual access (hw_ld{l,q}) */
        mmu_idx = AlphaMMUIdx_Privileged;
        locked = false;
        break;
    case EV6_HW_LD_VIRT_WCHK:
        /* Virtual access with write check (hw_ld{l,q}/w) */
        mmu_idx = AlphaMMUIdx_PrivilegedWChk;
        locked = false;
        break;
    case EV6_HW_LD_VIRT_ALT:
        /* Virtual access with AltMode access (hw_ld{l,q}/a) */
        mmu_idx = AlphaMMUIdx_AltMode;
        locked = false;
        break;
    case EV6_HW_LD_VIRT_WALT:
        /* Virtual access with AltMode + write check (hw_ld{l,q}/wa) */
        mmu_idx = AlphaMMUIdx_AltModeWChk;
        locked = false;
        break;
    default:
        g_assert_not_reached();
    }

    tcg_gen_andi_i64(addr, addr, memop_mask(memop));
    if (locked) {
        gen_load_locked(s, addr, dest, mmu_idx, memop);
    } else {
        tcg_gen_qemu_ld_i64(dest, addr, mmu_idx, memop);
    }
}

static void gen_hw_st_ev4(DisasContext *s, TCGv_i64 addr, TCGv_i64 val,
                          int type, bool quad)
{
    AlphaMMUIdx mmu_idx = ev4_hw_st_mmu_idx[type & 0b111].mmu_idx;
    bool locked = ev4_hw_st_mmu_idx[type & 0b111].locked;
    MemOp memop = quad ? MO_LEUQ : MO_LEUL;

    tcg_gen_andi_i64(addr, addr, memop_mask(memop));
    if (locked) {
        gen_store_conditional_common(s, val, addr, mmu_idx, memop);
    } else {
        tcg_gen_qemu_st_i64(val, addr, mmu_idx, memop);
    }
}

static void gen_hw_st_ev5(DisasContext *s, TCGv_i64 addr, TCGv_i64 val,
                          int type, bool quad)
{
    AlphaMMUIdx mmu_idx = ev5_hw_st_mmu_idx[type & 0b11111].mmu_idx;
    bool locked = ev5_hw_st_mmu_idx[type & 0b11111].locked;
    MemOp memop = quad ? MO_LEUQ : MO_LEUL;

    tcg_gen_andi_i64(addr, addr, memop_mask(memop));
    if (locked) {
        gen_store_conditional_common(s, val, addr, mmu_idx, memop);
    } else {
        tcg_gen_qemu_st_i64(val, addr, mmu_idx, memop);
    }
}

static void gen_hw_st_ev6(DisasContext *s, TCGv_i64 addr, TCGv_i64 val,
                          int type, bool quad)
{
    AlphaMMUIdx mmu_idx;
    MemOp memop;
    bool locked;

    memop = quad ? MO_LEUQ : MO_LEUL;
    switch (type) {
    case EV6_HW_ST_PHYS:
        /* Physical access (hw_st{l,q}/p) */
        mmu_idx = AlphaMMUIdx_Physical;
        locked = false;
        break;
    case EV6_HW_ST_PHYS_COND:
        /* Physical access with store conditional (hw_st{l,q}_c/p) */
        mmu_idx = AlphaMMUIdx_Physical;
        locked = true;
        break;
    case EV6_HW_ST_VIRT:
        /* Virtual access (hw_st{l,q}) */
        mmu_idx = AlphaMMUIdx_Privileged;
        locked = false;
        break;
    case EV6_HW_ST_VIRT_ALT:
        /* Virtual access with AltMode access (hw_st{l,q}/a) */
        mmu_idx = AlphaMMUIdx_AltMode;
        locked = false;
        break;
    default:
        g_assert_not_reached();
    }

    tcg_gen_andi_i64(addr, addr, memop_mask(memop));
    if (locked) {
        gen_store_conditional_common(s, val, addr, mmu_idx, memop);
    } else {
        tcg_gen_qemu_st_i64(val, addr, mmu_idx, memop);
    }
}

static void gen_rx(DisasContext *s, int ra, bool set)
{
    if (ra != 31) {
        tcg_gen_mov_i64(cpu_reg(s, ra), cpu_rc);
    }
    tcg_gen_movi_i64(cpu_rc, set);
}

static TCGv_ptr gen_lookup_ip_reg(uint32_t key)
{
    TCGv_ptr ret = tcg_temp_new_ptr();
    gen_helper_lookup_ip_reg(ret, tcg_env, tcg_constant_i32(key));
    return ret;
}

static void gen_ipreg_insn(DisasContext *s, int rt, uint32_t iprn, bool isread)
{
    const AlphaIPRegInfo *ri = get_alpha_ipreg_info(s->ipregs, iprn);
    bool need_exit_tb = false;
    TCGv_i64 tcg_rt;

    if (!ri) {
        qemu_log_mask(LOG_UNIMP, "%s access to unsupported internal "
                      "processor register 0x%04x at 0x" TARGET_FMT_lx "\n",
                      isread ? "read" : "write", iprn, s->pc_curr);
        unallocated_encoding(s);
        return;
    }

    /* Check access permissions */
    if (!ipreg_access_ok(ri, isread)) {
        unallocated_encoding(s);
        return;
    }

    if (ri->type & ALPHA_IP_NOP) {
        return;
    }

    if (ri->type & ALPHA_IP_RAISES_EXC) {
        gen_update_pc(s, 0);
    }

    if (ri->type & ALPHA_IP_IO) {
        /* I/O operations must end the TB here (whether read or write) */
        need_exit_tb = translator_io_start(&s->base);
    }

    tcg_rt = cpu_reg(s, rt);
    if (isread) {
        /* Read */
        if (ri->type & ALPHA_IP_CONST) {
            tcg_gen_movi_i64(tcg_rt, ri->resetvalue);
        } else if (ri->readfn) {
            gen_helper_get_ip_reg(tcg_rt, tcg_env, gen_lookup_ip_reg(iprn));
        } else {
            tcg_gen_ld_i64(tcg_rt, tcg_env, ri->fieldoffset);
        }
    } else {
        if (ri->type & ALPHA_IP_CONST) {
            /* If not forbidden by access permissions, treat as WI */
            return;
        } else if (ri->writefn) {
            gen_helper_set_ip_reg(tcg_env, gen_lookup_ip_reg(iprn), tcg_rt);
        } else {
            tcg_gen_st_i64(tcg_rt, tcg_env, ri->fieldoffset);
        }
    }

    if (!isread && !(ri->type & ALPHA_IP_SUPPRESS_TB_END)) {
        /*
         * A write to any IPR that ends a TB must rebuild the hflags for
         * the next TB.
         */
        gen_rebuild_hflags(s);

        /*
         * We default to ending the TB on an IPR write, but allow this to
         * be suppressed by the register definition
         * (usually only necessary to work around guest bugs).
         */
        need_exit_tb = true;
    }

    if (need_exit_tb) {
        s->base.is_jmp = DISAS_EXIT_UPDATE;
    }
}

static bool trans_call_pal(DisasContext *s, arg_call_pal *a)
{
    TCGv_i64 tmp;

    /*
     * 6.8.1 CALL_PAL Entry Points
     *
     * The PALcode OPCDEC exception flow will be invoked if the
     * CALL_PAL function field satisfies any of the following requirements:
     *
     *  - Is in the range of 0x40 and 0x7f, inclusive.
     *  - Is greater than 0xbf.
     *  - Is between 0x00 and 0x3f, inclusive, and IER_CM[CM] is not equal to
     *    the kernel mode value (0).
     *
     * If none of the conditions above are met, the PALcode entry point PC
     * is as follows:
     *
     * - PC[63:15]  = PAL_BASE[63:15]
     * - PC[14]     = 0
     * - PC[13]     = 1
     * - PC[12]     = CALL_PAL function field [7]
     * - PC[11:6]   = CALL_PAL function field [5:0]
     * - PC[5:1]    = 0
     * - PC[0]      = 1 (PALmode)
     */
    if ((a->func < 0x40 && !alpha_is_privileged(get_mem_index(s))) ||
        ((a->func > 0x3f) && (a->func < 0x80)) ||
        (a->func > 0xbf)) {
        return false;
    }

    /*
     * 6.3 Required PALcode Function Codes
     *
     * Table 6–1 lists opcodes required for all Alpha implementations.
     * The notation used is oo.ffff, where oo is the hexadecimal 6-bit
     * opcode and ffff is the hexadecimal 26-bit function code.
     *
     *  Mnemonic    Type          Function Code
     *  ----------  ------------  ---------------
     *  DRAINA      Privileged    00.0002
     *  HALT        Privileged    00.0000
     *  IMB         Unprivileged  00.0086
     */
    switch (a->func) {
    case 0x0086:
        /* Treat IMB as a memory barrier. */
        tcg_gen_mb(TCG_BAR_SC | TCG_MO_ALL);
        gen_lookup_tb(s);
        return true;
    default:
        break;
    }

    /* We defer to the guest PALcode for all other operations. */
    tmp = tcg_temp_new_i64();
    gen_pc_plus_diff(s, tmp, curr_insn_len(s));
    switch (s->isa_implver) {
    case IMPLVER_2106x:
    case IMPLVER_21164:
        tcg_gen_st_i64(tmp, tcg_env, offsetof(CPUAlphaState, ipr.exc_addr));
        break;
    case IMPLVER_21264:
    case IMPLVER_21364:
        tcg_gen_mov_i64(cpu_reg_forward(s, 23), tmp);
        break;
    default:
        g_assert_not_reached();
    }
    gen_exception_internal_insn(s, EXCP_CALL_PAL, syn_call_pal(a->func));
    return true;
}

static bool trans_lda(DisasContext *s, arg_lda *a)
{
    tcg_gen_addi_i64(cpu_reg(s, a->ra), read_cpu_reg(s, a->rb), a->disp);
    return true;
}

static bool trans_ldah(DisasContext *s, arg_ldah *a)
{
    tcg_gen_addi_i64(cpu_reg(s, a->ra), read_cpu_reg(s, a->rb), a->disp);
    return true;
}

static bool trans_ldbu(DisasContext *s, arg_ldbu *a)
{
    if (!dc_amask_feature(bwx, s)) {
        return false;
    }
    gen_load_int(s, a->ra, a->rb, a->disp, MO_UB, false, false);
    return true;
}

static bool trans_ldq_u(DisasContext *s, arg_ldq_u *a)
{
    gen_load_int(s, a->ra, a->rb, a->disp, MO_LEUQ, true, false);
    return true;
}

static bool trans_ldwu(DisasContext *s, arg_ldwu *a)
{
    if (!dc_amask_feature(bwx, s)) {
        return false;
    }
    gen_load_int(s, a->ra, a->rb, a->disp, MO_LEUW, false, false);
    return true;
}

static bool trans_stw(DisasContext *s, arg_stw *a)
{
    if (!dc_amask_feature(bwx, s)) {
        return false;
    }
    gen_store_int(s, a->ra, a->rb, a->disp, MO_LEUW, false);
    return true;
}

static bool trans_stb(DisasContext *s, arg_stb *a)
{
    if (!dc_amask_feature(bwx, s)) {
        return false;
    }
    gen_store_int(s, a->ra, a->rb, a->disp, MO_UB, false);
    return true;
}

static bool trans_stq_u(DisasContext *s, arg_stq_u *a)
{
    gen_store_int(s, a->ra, a->rb, a->disp, MO_LEUQ | MO_ALIGN, true);
    return true;
}

static bool trans_addl(DisasContext *s, arg_addl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    /* Special case ADDL as SEXTL.  */
    if (a->ra == 31) {
        tcg_gen_ext32s_i64(vc, vb);
    } else {
        tcg_gen_add_i64(vc, va, vb);
        tcg_gen_ext32s_i64(vc, vc);
    }
    return true;
}

static bool trans_s4addl(DisasContext *s, arg_s4addl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 2);
    tcg_gen_add_i64(tmp, tmp, vb);
    tcg_gen_ext32s_i64(vc, tmp);
    return true;
}

static bool trans_subl(DisasContext *s, arg_subl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_sub_i64(vc, va, vb);
    tcg_gen_ext32s_i64(vc, vc);
    return true;
}

static bool trans_s4subl(DisasContext *s, arg_s4subl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 2);
    tcg_gen_sub_i64(tmp, tmp, vb);
    tcg_gen_ext32s_i64(vc, tmp);
    return true;
}

static bool trans_cmpbge(DisasContext *s, arg_cmpbge *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    if (a->ra == 31) {
        /* Special case 0 >= X as X == 0.  */
        gen_helper_cmpbe0(vc, vb);
    } else {
        gen_helper_cmpbge(vc, va, vb);
    }
    return true;
}

static bool trans_s8addl(DisasContext *s, arg_s8addl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 3);
    tcg_gen_add_i64(tmp, tmp, vb);
    tcg_gen_ext32s_i64(vc, tmp);
    return true;
}

static bool trans_s8subl(DisasContext *s, arg_s8subl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 3);
    tcg_gen_sub_i64(tmp, tmp, vb);
    tcg_gen_ext32s_i64(vc, tmp);
    return true;
}

static bool trans_cmpult(DisasContext *s, arg_cmpult *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_setcond_i64(TCG_COND_LTU, vc, va, vb);
    return true;
}

static bool trans_addq(DisasContext *s, arg_addq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_add_i64(vc, va, vb);
    return true;
}

static bool trans_s4addq(DisasContext *s, arg_s4addq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 2);
    tcg_gen_add_i64(vc, tmp, vb);
    return true;
}

static bool trans_subq(DisasContext *s, arg_subq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    /* Special case SUBQ as NEGQ.  */
    if (a->ra == 31) {
        tcg_gen_neg_i64(vc, vb);
    } else {
        tcg_gen_sub_i64(vc, va, vb);
    }
    return true;
}

static bool trans_s4subq(DisasContext *s, arg_s4subq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 2);
    tcg_gen_sub_i64(vc, tmp, vb);
    return true;
}

static bool trans_cmpeq(DisasContext *s, arg_cmpeq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_setcond_i64(TCG_COND_EQ, vc, va, vb);
    return true;
}

static bool trans_s8addq(DisasContext *s, arg_s8addq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 3);
    tcg_gen_add_i64(vc, tmp, vb);
    return true;
}

static bool trans_s8subq(DisasContext *s, arg_s8subq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_shli_i64(tmp, va, 3);
    tcg_gen_sub_i64(vc, tmp, vb);
    return true;
}

static bool trans_cmpule(DisasContext *s, arg_cmpule *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_setcond_i64(TCG_COND_LEU, vc, va, vb);
    return true;
}

static bool trans_addl_v(DisasContext *s, arg_addl_v *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(tmp, va);
    tcg_gen_ext32s_i64(vc, vb);
    tcg_gen_add_i64(tmp, tmp, vc);
    tcg_gen_ext32s_i64(vc, tmp);
    gen_helper_check_overflow(tcg_env, vc, tmp);
    return true;
}

static bool trans_subl_v(DisasContext *s, arg_subl_v *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(tmp, va);
    tcg_gen_ext32s_i64(vc, vb);
    tcg_gen_sub_i64(tmp, tmp, vc);
    tcg_gen_ext32s_i64(vc, tmp);
    gen_helper_check_overflow(tcg_env, vc, tmp);
    return true;
}

static bool trans_cmplt(DisasContext *s, arg_cmplt *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_setcond_i64(TCG_COND_LT, vc, va, vb);
    return true;
}

static bool trans_addq_v(DisasContext *s, arg_addq_v *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp, tmp2;

    tmp = tcg_temp_new_i64();
    tmp2 = tcg_temp_new_i64();
    tcg_gen_eqv_i64(tmp, va, vb);
    tcg_gen_mov_i64(tmp2, va);
    tcg_gen_add_i64(vc, va, vb);
    tcg_gen_xor_i64(tmp2, tmp2, vc);
    tcg_gen_and_i64(tmp, tmp, tmp2);
    tcg_gen_shri_i64(tmp, tmp, 63);
    tcg_gen_movi_i64(tmp2, 0);
    gen_helper_check_overflow(tcg_env, tmp, tmp2);
    return true;
}

static bool trans_subq_v(DisasContext *s, arg_subq_v *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp, tmp2;

    tmp = tcg_temp_new_i64();
    tmp2 = tcg_temp_new_i64();
    tcg_gen_xor_i64(tmp, va, vb);
    tcg_gen_mov_i64(tmp2, va);
    tcg_gen_sub_i64(vc, va, vb);
    tcg_gen_xor_i64(tmp2, tmp2, vc);
    tcg_gen_and_i64(tmp, tmp, tmp2);
    tcg_gen_shri_i64(tmp, tmp, 63);
    tcg_gen_movi_i64(tmp2, 0);
    gen_helper_check_overflow(tcg_env, tmp, tmp2);
    return true;
}

static bool trans_cmple(DisasContext *s, arg_cmple *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_setcond_i64(TCG_COND_LE, vc, va, vb);
    return true;
}

static bool trans_and(DisasContext *s, arg_and *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_and_i64(vc, va, vb);
    return true;
}

static bool trans_bic(DisasContext *s, arg_bic *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_andc_i64(vc, va, vb);
    return true;
}

static bool trans_cmovlbs(DisasContext *s, arg_cmovlbs *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_TSTNE, vc, va, tcg_constant_i64(1),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_cmovlbc(DisasContext *s, arg_cmovlbc *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_TSTEQ, vc, va, tcg_constant_i64(1),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_bis(DisasContext *s, arg_bis *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    /* Special case BIS as NOP.  */
    if (a->rc == 31) {
        return true;
    }

    /* Special case BIS as MOV.  */
    if (a->ra == 31) {
        tcg_gen_mov_i64(vc, vb);
        return true;
    }

    tcg_gen_or_i64(vc, va, vb);
    return true;
}

static bool trans_cmoveq(DisasContext *s, arg_cmoveq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_EQ, vc, va, tcg_constant_i64(0),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_cmovne(DisasContext *s, arg_cmovne *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_NE, vc, va, tcg_constant_i64(0),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_ornot(DisasContext *s, arg_ornot *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    /* Special case ORNOT as NOT.  */
    if (a->ra == 31) {
        tcg_gen_not_i64(vc, vb);
        return true;
    }

    tcg_gen_orc_i64(vc, va, vb);
    return true;
}

static bool trans_xor(DisasContext *s, arg_xor *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_xor_i64(vc, va, vb);
    return true;
}

static bool trans_cmovlt(DisasContext *s, arg_cmovlt *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_LT, vc, va, tcg_constant_i64(0),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_cmovge(DisasContext *s, arg_cmovge *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_GE, vc, va, tcg_constant_i64(0),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_eqv(DisasContext *s, arg_eqv *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_eqv_i64(vc, va, vb);
    return true;
}

static bool trans_cmovle(DisasContext *s, arg_cmovle *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_LE, vc, va, tcg_constant_i64(0),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_cmovgt(DisasContext *s, arg_cmovgt *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_movcond_i64(TCG_COND_GT, vc, va, tcg_constant_i64(0),
                        vb, read_cpu_reg(s, a->rc));
    return true;
}

static bool trans_amask(DisasContext *s, arg_amask *a)
{
    TCGv_i64 vb, vc;

    if (unlikely(a->ra != 31)) {
        return false;
    }

    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    vc = cpu_reg(s, a->rc);
    if (s->isa_procid > PROCID_EV45) {
        tcg_gen_andi_i64(vc, vb, ~s->isa_amask);
    }
    return true;
}

static bool trans_implver(DisasContext *s, arg_implver *a)
{
    TCGv_i64 vc;

    if (unlikely(a->ra != 31)) {
        return false;
    }
    if (unlikely(a->lit != 0x01)) {
        return false;
    }

    vc = cpu_reg(s, a->rc);
    tcg_gen_movi_i64(vc, s->isa_implver);
    return true;
}

static bool trans_mskbl(DisasContext *s, arg_extbl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_l(s, vc, va, a->rb, a->islit, a->lit, 0x01);
    return true;
}

static bool trans_extbl(DisasContext *s, arg_extbl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_l(s, vc, va, a->rb, a->islit, a->lit, 0x01);
    return true;
}

static bool trans_insbl(DisasContext *s, arg_insbl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_l(s, vc, va, a->rb, a->islit, a->lit, 0x01);
    return true;
}

static bool trans_mskwl(DisasContext *s, arg_mskwl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_l(s, vc, va, a->rb, a->islit, a->lit, 0x03);
    return true;
}

static bool trans_extwl(DisasContext *s, arg_extwl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_l(s, vc, va, a->rb, a->islit, a->lit, 0x03);
    return true;
}

static bool trans_inswl(DisasContext *s, arg_inswl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_l(s, vc, va, a->rb, a->islit, a->lit, 0x03);
    return true;
}

static bool trans_mskll(DisasContext *s, arg_mskll *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_l(s, vc, va, a->rb, a->islit, a->lit, 0x0f);
    return true;
}

static bool trans_extll(DisasContext *s, arg_extll *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_l(s, vc, va, a->rb, a->islit, a->lit, 0x0f);
    return true;
}

static bool trans_insll(DisasContext *s, arg_insll *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_l(s, vc, va, a->rb, a->islit, a->lit, 0x0f);
    return true;
}

static bool trans_zap(DisasContext *s, arg_zap *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    if (a->islit) {
        gen_zapnoti(vc, va, ~(a->lit));
    } else {
        gen_helper_zap(vc, va, read_cpu_reg(s, a->rb));
    }
    return true;
}

static bool trans_zapnot(DisasContext *s, arg_zapnot *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    if (a->islit) {
        gen_zapnoti(vc, va, a->lit);
    } else {
        gen_helper_zapnot(vc, va, read_cpu_reg(s, a->rb));
    }
    return true;
}

static bool trans_mskql(DisasContext *s, arg_mskql *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_l(s, vc, va, a->rb, a->islit, a->lit, 0xff);
    return true;
}

static bool trans_srl(DisasContext *s, arg_srl *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 vb, tmp;

    if (a->islit) {
        tcg_gen_shri_i64(vc, va, a->lit & 0x3f);
    } else {
        tmp = tcg_temp_new_i64();
        vb = read_cpu_reg(s, a->rb);
        tcg_gen_andi_i64(tmp, vb, 0x3f);
        tcg_gen_shr_i64(vc, va, tmp);
    }
    return true;
}

static bool trans_extql(DisasContext *s, arg_extql *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_l(s, vc, va, a->rb, a->islit, a->lit, 0xff);
    return true;
}

static bool trans_sll(DisasContext *s, arg_sll *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 vb, tmp;

    if (a->islit) {
        tcg_gen_shli_i64(vc, va, a->lit & 0x3f);
    } else {
        tmp = tcg_temp_new_i64();
        vb = read_cpu_reg(s, a->rb);
        tcg_gen_andi_i64(tmp, vb, 0x3f);
        tcg_gen_shl_i64(vc, va, tmp);
    }
    return true;
}

static bool trans_insql(DisasContext *s, arg_insql *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_l(s, vc, va, a->rb, a->islit, a->lit, 0xff);
    return true;
}

static bool trans_sra(DisasContext *s, arg_sra *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 vb, tmp;

    if (a->islit) {
        tcg_gen_sari_i64(vc, va, a->lit & 0x3f);
    } else {
        tmp = tcg_temp_new_i64();
        vb = read_cpu_reg(s, a->rb);
        tcg_gen_andi_i64(tmp, vb, 0x3f);
        tcg_gen_sar_i64(vc, va, tmp);
    }
    return true;
}

static bool trans_mskwh(DisasContext *s, arg_mskwh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_h(s, vc, va, a->rb, a->islit, a->lit, 0x03);
    return true;
}

static bool trans_inswh(DisasContext *s, arg_inswh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_h(s, vc, va, a->rb, a->islit, a->lit, 0x03);
    return true;
}

static bool trans_extwh(DisasContext *s, arg_extwh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_h(s, vc, va, a->rb, a->islit, a->lit, 0x03);
    return true;
}

static bool trans_msklh(DisasContext *s, arg_msklh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_h(s, vc, va, a->rb, a->islit, a->lit, 0x0f);
    return true;
}

static bool trans_inslh(DisasContext *s, arg_inslh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_h(s, vc, va, a->rb, a->islit, a->lit, 0x0f);
    return true;
}

static bool trans_extlh(DisasContext *s, arg_extlh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_h(s, vc, va, a->rb, a->islit, a->lit, 0x0f);
    return true;
}

static bool trans_mskqh(DisasContext *s, arg_mskqh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_msk_h(s, vc, va, a->rb, a->islit, a->lit, 0xff);
    return true;
}

static bool trans_insqh(DisasContext *s, arg_insqh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ins_h(s, vc, va, a->rb, a->islit, a->lit, 0xff);
    return true;
}

static bool trans_extqh(DisasContext *s, arg_extqh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    gen_ext_h(s, vc, va, a->rb, a->islit, a->lit, 0xff);
    return true;
}

static bool trans_mull(DisasContext *s, arg_mull *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_mul_i64(vc, va, vb);
    tcg_gen_ext32s_i64(vc, vc);
    return true;
}

static bool trans_mulq(DisasContext *s, arg_mulq *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);

    tcg_gen_mul_i64(vc, va, vb);
    return true;
}

static bool trans_umulh(DisasContext *s, arg_umulh *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_mulu2_i64(tmp, vc, va, vb);
    return true;
}

static bool trans_mull_v(DisasContext *s, arg_mull_v *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(tmp, va);
    tcg_gen_ext32s_i64(vc, vb);
    tcg_gen_mul_i64(tmp, tmp, vc);
    tcg_gen_ext32s_i64(vc, tmp);
    gen_helper_check_overflow(tcg_env, vc, tmp);
    return true;
}

static bool trans_mulq_v(DisasContext *s, arg_mulq_v *a)
{
    TCGv_i64 vc = cpu_reg(s, a->rc);
    TCGv_i64 vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    TCGv_i64 va = read_cpu_reg(s, a->ra);
    TCGv_i64 tmp, tmp2;

    tmp = tcg_temp_new_i64();
    tmp2 = tcg_temp_new_i64();
    tcg_gen_muls2_i64(vc, tmp, va, vb);
    tcg_gen_sari_i64(tmp2, vc, 63);
    gen_helper_check_overflow(tcg_env, tmp, tmp2);
    return true;
}

static bool trans_itofs(DisasContext *s, arg_itofs *a)
{
    TCGv_i64 va, vc;
    TCGv_i32 tmp;

    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->rb != 31)) {
        return false;
    }
    tmp = tcg_temp_new_i32();
    va = read_cpu_reg(s, a->ra);
    vc = fpu_reg(s, a->rc);
    tcg_gen_extrl_i64_i32(tmp, va);
    gen_helper_ieee_lds(vc, tmp);
    return true;
}

static bool trans_sqrtf(DisasContext *s, arg_sqrtf *a)
{
    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    gen_vax_sqrtf(s, a->rb, a->rc);
    return true;
}

static bool trans_sqrts(DisasContext *s, arg_sqrts *a)
{
    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    gen_ieee_sqrts(s, a->rb, a->rc);
    return true;
}

static bool trans_itoff(DisasContext *s, arg_itoff *a)
{
    TCGv_i64 va, vc;
    TCGv_i32 tmp;

    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->rb != 31)) {
        return false;
    }
    tmp = tcg_temp_new_i32();
    va = read_cpu_reg(s, a->ra);
    vc = fpu_reg(s, a->rc);
    tcg_gen_extrl_i64_i32(tmp, va);
    gen_helper_vax_ldf(vc, tmp);
    return true;
}

static bool trans_itoft(DisasContext *s, arg_itoft *a)
{
    TCGv_i64 va, vc;

    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->rb != 31)) {
        return false;
    }
    va = read_cpu_reg(s, a->ra);
    vc = fpu_reg(s, a->rc);
    tcg_gen_mov_i64(vc, va);
    return true;
}

static bool trans_sqrtg(DisasContext *s, arg_sqrtg *a)
{
    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    gen_vax_sqrtg(s, a->rb, a->rc);
    return true;
}

static bool trans_sqrtt(DisasContext *s, arg_sqrtt *a)
{
    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    gen_ieee_sqrtt(s, a->rb, a->rc);
    return true;
}

static bool trans_cvtql(DisasContext *s, arg_cvtql *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    gen_ieee_cvtql(s, a->rb, a->rc, get_fn11(s->insn) & QUAL_MASK);
    return true;
}

static bool trans_cvtql_nt(DisasContext *s, arg_cvtql_nt *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    /*
     * Windows NT 3.5 build 756 WINSRV uses function 0x330. Although the
     * Alpha manuals do not list this encoding, retain it for NT compatibility
     * as CVTQL/V (0x130). Ignore the extra QUAL_I bit: CVTQL does not support
     * inexact traps, but integer overflow must still raise an exception.
     */
    gen_ieee_cvtql(s, a->rb, a->rc, QUAL_V);
    return true;
}


#define GEN_IEEE_OP1(name)                              \
static bool glue(trans_, name)(DisasContext *s,         \
                               glue(arg_, name) *a)     \
{                                                       \
    if (!fp_access_check(s)) {                          \
        return false;                                   \
    }                                                   \
    if (unlikely(a->ra != 31)) {                        \
        return false;                                   \
    }                                                   \
    glue(gen_ieee_, name)(s, a->rb, a->rc);             \
    return true;                                        \
}

#define GEN_IEEE_OP2(name)                              \
static bool glue(trans_, name)(DisasContext *s,         \
                               glue(arg_, name) *a)     \
{                                                       \
    if (!fp_access_check(s)) {                          \
        return false;                                   \
    }                                                   \
    glue(gen_ieee_, name)(s, a->ra, a->rb, a->rc);      \
    return true;                                        \
}

GEN_IEEE_OP2(adds)
GEN_IEEE_OP2(subs)
GEN_IEEE_OP2(muls)
GEN_IEEE_OP2(divs)
GEN_IEEE_OP2(addt)
GEN_IEEE_OP2(subt)
GEN_IEEE_OP2(mult)
GEN_IEEE_OP2(divt)
GEN_IEEE_OP2(cmptun)
GEN_IEEE_OP2(cmpteq)
GEN_IEEE_OP2(cmptlt)
GEN_IEEE_OP2(cmptle)
GEN_IEEE_OP1(cvttq)
GEN_IEEE_OP1(cvtqs)
GEN_IEEE_OP1(cvtqt)
GEN_IEEE_OP1(cvtst)
GEN_IEEE_OP1(cvtts)

#define GEN_VAX_OP1(name)                               \
static bool glue(trans_, name)(DisasContext *s,         \
                               glue(arg_, name) *a)     \
{                                                       \
    if (!fp_access_check(s)) {                          \
        return false;                                   \
    }                                                   \
    if (unlikely(a->ra != 31)) {                        \
        return false;                                   \
    }                                                   \
    glue(gen_vax_, name)(s, a->rb, a->rc);              \
    return true;                                        \
}

#define GEN_VAX_OP2(name)                               \
static bool glue(trans_, name)(DisasContext *s,         \
                               glue(arg_, name) *a)     \
{                                                       \
    if (!fp_access_check(s)) {                          \
        return false;                                   \
    }                                                   \
    glue(gen_vax_, name)(s, a->ra, a->rb, a->rc);       \
    return true;                                        \
}

GEN_VAX_OP2(addf)
GEN_VAX_OP2(subf)
GEN_VAX_OP2(mulf)
GEN_VAX_OP2(divf)
GEN_VAX_OP2(addg)
GEN_VAX_OP2(subg)
GEN_VAX_OP2(mulg)
GEN_VAX_OP2(divg)
GEN_VAX_OP2(cmpgeq)
GEN_VAX_OP2(cmpglt)
GEN_VAX_OP2(cmpgle)
GEN_VAX_OP1(cvtqf)
GEN_VAX_OP1(cvtgf)
GEN_VAX_OP1(cvtqg)
GEN_VAX_OP1(cvtgq)
GEN_VAX_OP1(cvtdg)
GEN_VAX_OP1(cvtgd)

static bool trans_cvtlq(DisasContext *s, arg_cvtlq *a)
{
    TCGv_i64 vb, vc;

    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->ra != 31)) {
        return false;
    }
    vc = fpu_reg(s, a->rc);
    vb = read_fpu_reg(s, a->rb);
    gen_ieee_cvtlq(vc, vb);
    return true;
}

static bool trans_cpys(DisasContext *s, arg_cpys *a)
{
    TCGv_i64 va, vb, vc;

    if (!fp_access_check(s)) {
        return false;
    }

    /* Special case CPYS as FNOP.  */
    if (a->rc == 31) {
        return true;
    }

    /* Special case CPYS as FMOV.  */
    vc = fpu_reg(s, a->rc);
    va = read_fpu_reg(s, a->ra);
    if (a->ra == a->rb) {
        tcg_gen_mov_i64(vc, va);
    } else {
        vb = read_fpu_reg(s, a->rb);
        gen_ieee_copymask(vc, va, vb, 0, UINT64_C(0x8000000000000000));
    }
    return true;
}

static bool trans_cpysn(DisasContext *s, arg_cpysn *a)
{
    TCGv_i64 va, vb, vc;

    if (!fp_access_check(s)) {
        return false;
    }
    vc = fpu_reg(s, a->rc);
    vb = read_fpu_reg(s, a->rb);
    va = read_fpu_reg(s, a->ra);
    gen_ieee_copymask(vc, va, vb, 1, UINT64_C(0x8000000000000000));
    return true;
}

static bool trans_cpyse(DisasContext *s, arg_cpyse *a)
{
    TCGv_i64 va, vb, vc;

    if (!fp_access_check(s)) {
        return false;
    }
    vc = fpu_reg(s, a->rc);
    vb = read_fpu_reg(s, a->rb);
    va = read_fpu_reg(s, a->ra);
    gen_ieee_copymask(vc, va, vb, 0, UINT64_C(0xFFF0000000000000));
    return true;
}

static bool trans_mt_fpcr(DisasContext *s, arg_mt_fpcr *a)
{
    TCGv_i64 va;

    if (!fp_access_check(s)) {
        return false;
    }
    va = read_fpu_reg(s, a->ra);
    gen_helper_set_fpcr(tcg_env, va);
    if (s->tb_rm == QUAL_RM_D) {
        s->tb_rm = -1;
    }
    /* XXX: Should we dispatch a MT_FPCR exception here? */
    return true;
}

static bool trans_mf_fpcr(DisasContext *s, arg_mf_fpcr *a)
{
    TCGv_i64 va;

    if (!fp_access_check(s)) {
        return false;
    }
    va = fpu_reg(s, a->ra);
    gen_helper_get_fpcr(va, tcg_env);
    return true;
}

static bool trans_fcmoveq(DisasContext *s, arg_fcmoveq *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fcmov(s, TCG_COND_EQ, a->ra, a->rb, a->rc);
    return true;
}

static bool trans_fcmovne(DisasContext *s, arg_fcmovne *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fcmov(s, TCG_COND_NE, a->ra, a->rb, a->rc);
    return true;
}

static bool trans_fcmovlt(DisasContext *s, arg_fcmovlt *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fcmov(s, TCG_COND_LT, a->ra, a->rb, a->rc);
    return true;
}

#define GEN_IEEE_FCMOV_OP(name, cond)                   \
static bool glue(trans_, name)(DisasContext *s,         \
                               glue(arg_, name) *a)     \
{                                                       \
    if (!fp_access_check(s)) {                          \
        return false;                                   \
    }                                                   \
    gen_fcmov(s, cond, a->ra, a->rb, a->rc);            \
    return true;                                        \
}

GEN_IEEE_FCMOV_OP(fcmovge, TCG_COND_GE)
GEN_IEEE_FCMOV_OP(fcmovle, TCG_COND_LE)
GEN_IEEE_FCMOV_OP(fcmovgt, TCG_COND_GT)

static bool trans_trapb(DisasContext *s, arg_trapb *a)
{
    /*
     * We need to break the TB after this insn to execute
     * self-modifying code correctly and also to take
     * any pending interrupts immediately.
     */
    gen_lookup_tb(s);
    return true;
}

static bool trans_excb(DisasContext *s, arg_excb *a)
{
    /*
     * TODO: There is no speculation barrier opcode for TCG;
     * MB and end the TB instead.
     */
    tcg_gen_mb(TCG_BAR_SC | TCG_MO_ALL);
    gen_lookup_tb(s);
    return true;
}

static bool trans_mb(DisasContext *s, arg_mb *a)
{
    tcg_gen_mb(TCG_BAR_SC | TCG_MO_ALL);
    return true;
}

static bool trans_wmb(DisasContext *s, arg_wmb *a)
{
    tcg_gen_mb(TCG_BAR_SC | TCG_MO_ST_ST);
    return true;
}

static bool trans_fetch(DisasContext *s, arg_fetch *a)
{
    /* Prefetch is a nop within QEMU. */
    return true;
}

static bool trans_fetch_m(DisasContext *s, arg_fetch_m *a)
{
    /* Prefetch is a nop within QEMU. */
    return true;
}

static bool trans_rpcc(DisasContext *s, arg_rpcc *a)
{
    TCGv_i64 va = cpu_reg(s, a->ra);

    translator_io_start(&s->base);
    gen_helper_rpcc(va, tcg_env);

    /* Exit back to the main loop to handle timer interrupts. */
    s->base.is_jmp = DISAS_EXIT_UPDATE;
    return true;
}

static bool trans_rc(DisasContext *s, arg_rc *a)
{
    gen_rx(s, a->ra, false);
    return true;
}

static bool trans_ecb(DisasContext *s, arg_ecb *a)
{
    /* Evict cache block is a nop within QEMU. */
    return true;
}

static bool trans_rs(DisasContext *s, arg_rs *a)
{
    gen_rx(s, a->ra, true);
    return true;
}

static bool trans_wh64(DisasContext *s, arg_wh64 *a)
{
    /* Prefetch is a nop within QEMU. */
    return true;
}

static bool trans_wh64en(DisasContext *s, arg_wh64en *a)
{
    /*
     * 4.11.11 Write Hint:
     *
     * WH64EN is implemented as a NOP on processors previous to the 21264/EV6x
     * and implemented as WH64 on 21264/EV6x processors.
     */
    return true;
}

static bool trans_hw_mfpr(DisasContext *s, arg_hw_mfpr *a)
{
    int iprn;

    if (!palres_access_check(s)) {
        return false;
    }
    switch (s->isa_implver) {
    case IMPLVER_2106x:
        if (unlikely(a->ra != a->rb)) {
            return false;
        }
        break;
    default:
        if (unlikely(a->rb != 31)) {
            return false;
        }
        break;
    }
    switch (s->isa_implver) {
    case IMPLVER_21264:
    case IMPLVER_21364:
        iprn = lshr_8(s, a->iprn);
        break;
    default:
        iprn = a->iprn;
        break;
    }
    gen_ipreg_insn(s, a->ra, iprn, true);
    return true;
}

static bool trans_jmp(DisasContext *s, arg_jmp *a)
{
    TCGv_i64 tmp;

    tmp = tcg_temp_new_i64();
    tcg_gen_andi_i64(tmp, read_cpu_reg(s, a->rb), UINT64_C(~0x03));
    if (a->ra != 31) {
        gen_pc_plus_diff(s, cpu_reg(s, a->ra), curr_insn_len(s));
    }
    gen_set_pc(s, tmp);
    s->base.is_jmp = DISAS_CHAIN;
    return true;
}

static bool trans_hw_ld_ev4(DisasContext *s, arg_hw_ld_ev4 *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }
    if (s->isa_implver != IMPLVER_2106x) {
        return false;
    }
    tmp = tcg_temp_new_i64();
    tcg_gen_addi_i64(tmp, read_cpu_reg(s, a->rb), a->disp);
    gen_hw_ld_ev4(s, cpu_reg(s, a->ra), tmp, a->type, a->len & 0b01);
    return true;
}

static bool trans_hw_ld_ev5(DisasContext *s, arg_hw_ld_ev5 *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }
    if (s->isa_implver != IMPLVER_21164) {
        return false;
    }
    tmp = tcg_temp_new_i64();
    tcg_gen_addi_i64(tmp, read_cpu_reg(s, a->rb), a->disp);
    gen_hw_ld_ev5(s, cpu_reg(s, a->ra), tmp, a->type, a->len & 0b01);
    return true;
}

static bool trans_hw_ld_ev6(DisasContext *s, arg_hw_ld_ev6 *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }

    switch (s->isa_implver) {
    case IMPLVER_21264:
    case IMPLVER_21364:
        break;
    default:
        return false;
    }

    tmp = tcg_temp_new_i64();
    tcg_gen_addi_i64(tmp, read_cpu_reg(s, a->rb), a->disp);
    switch (a->type) {
    case EV6_HW_LD_PHYS:
    case EV6_HW_LD_PHYS_LOCK:
    case EV6_HW_LD_VPTE:
    case EV6_HW_LD_VIRT:
    case EV6_HW_LD_VIRT_WCHK:
    case EV6_HW_LD_VIRT_ALT:
    case EV6_HW_LD_VIRT_WALT:
        gen_hw_ld_ev6(s, cpu_reg(s, a->ra), tmp, a->type, a->len & 0b01);
        return true;
    default:
        return false;
    }
}

static bool trans_sextb(DisasContext *s, arg_sextb *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(bwx, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    tcg_gen_ext8s_i64(vc, vb);
    return true;
}

static bool trans_sextw(DisasContext *s, arg_sextw *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(bwx, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    tcg_gen_ext16s_i64(vc, vb);
    return true;
}

static bool trans_ctpop(DisasContext *s, arg_ctpop *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(cix, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    tcg_gen_ctpop_i64(vc, vb);
    return true;
}

static bool trans_perr(DisasContext *s, arg_perr *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_perr(vc, va, vb);
    return true;
}

static bool trans_ctlz(DisasContext *s, arg_ctlz *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(cix, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    tcg_gen_clzi_i64(vc, vb, 64);
    return true;
}

static bool trans_cttz(DisasContext *s, arg_cttz *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(cix, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    tcg_gen_ctzi_i64(vc, vb, 64);
    return true;
}

static bool trans_unpkbw(DisasContext *s, arg_unpkbw *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_unpkbw(vc, vb);
    return true;
}

static bool trans_unpkbl(DisasContext *s, arg_unpkbl *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_unpkbl(vc, vb);
    return true;
}

static bool trans_pkwb(DisasContext *s, arg_pkwb *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_pkwb(vc, vb);
    return true;
}

static bool trans_pklb(DisasContext *s, arg_pklb *a)
{
    TCGv_i64 vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_pklb(vc, vb);
    return true;
}

static bool trans_minsb8(DisasContext *s, arg_minsb8 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_minsb8(vc, va, vb);
    return true;
}

static bool trans_minsw4(DisasContext *s, arg_minsw4 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_minsw4(vc, va, vb);
    return true;
}

static bool trans_minub8(DisasContext *s, arg_minub8 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_minub8(vc, va, vb);
    return true;
}

static bool trans_minuw4(DisasContext *s, arg_minuw4 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_minuw4(vc, va, vb);
    return true;
}

static bool trans_maxub8(DisasContext *s, arg_maxub8 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_maxub8(vc, va, vb);
    return true;
}

static bool trans_maxuw4(DisasContext *s, arg_maxuw4 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_maxuw4(vc, va, vb);
    return true;
}

static bool trans_maxsb8(DisasContext *s, arg_maxsb8 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_maxsb8(vc, va, vb);
    return true;
}

static bool trans_maxsw4(DisasContext *s, arg_maxsw4 *a)
{
    TCGv_i64 va, vb, vc;

    if (!dc_amask_feature(mvi, s)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_cpu_reg(s, a->ra);
    vb = read_cpu_reg_lit(s, a->rb, a->lit, a->islit);
    gen_helper_maxsw4(vc, va, vb);
    return true;
}

static bool trans_ftoit(DisasContext *s, arg_ftoit *a)
{
    TCGv_i64 va, vc;

    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->rb != 31)) {
        return false;
    }
    vc = cpu_reg(s, a->rc);
    va = read_fpu_reg(s, a->ra);
    tcg_gen_mov_i64(vc, va);
    return true;
}

static bool trans_ftois(DisasContext *s, arg_ftois *a)
{
    TCGv_i64 va, vc;
    TCGv_i32 tmp;

    if (!dc_amask_feature(fix, s)) {
        return false;
    }
    if (!fp_access_check(s)) {
        return false;
    }
    if (unlikely(a->rb != 31)) {
        return false;
    }
    tmp = tcg_temp_new_i32();
    vc = cpu_reg(s, a->rc);
    va = read_fpu_reg(s, a->ra);
    gen_helper_ieee_sts(tmp, va);
    tcg_gen_ext_i32_i64(vc, tmp);
    return true;
}

static bool trans_hw_mtpr(DisasContext *s, arg_hw_mtpr *a)
{
    int iprn;

    if (!palres_access_check(s)) {
        return false;
    }
    switch (s->isa_implver) {
    case IMPLVER_2106x:
        if (unlikely(a->ra != a->rb)) {
            return false;
        }
        break;
    default:
        if (unlikely(a->ra != 31)) {
            return false;
        }
        break;
    }
    switch (s->isa_implver) {
    case IMPLVER_21264:
    case IMPLVER_21364:
        iprn = lshr_8(s, a->iprn);
        break;
    default:
        iprn = a->iprn;
        break;
    }
    gen_ipreg_insn(s, a->rb, iprn, false);
    return true;
}

static bool trans_hw_rei(DisasContext *s, arg_hw_rei *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }
    if (unlikely(a->disp)) {
        return false;
    }
    switch (s->isa_implver) {
    case IMPLVER_2106x:
    case IMPLVER_21164:
        tmp = tcg_temp_new_i64();
        tcg_gen_ld_i64(tmp, tcg_env, offsetof(CPUAlphaState, ipr.exc_addr));
        gen_hw_rei(s, tmp);
        return true;
    default:
        return false;
    }
}

static bool trans_hw_ret(DisasContext *s, arg_hw_ret *a)
{
    if (!palres_access_check(s)) {
        return false;
    }
    if (unlikely(a->disp)) {
        return false;
    }
    switch (s->isa_implver) {
    case IMPLVER_21264:
    case IMPLVER_21364:
        gen_hw_rei(s, read_cpu_reg(s, a->rb));
        return true;
    default:
        return false;
    }
}

static bool trans_hw_st_ev4(DisasContext *s, arg_hw_st_ev4 *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }
    if (s->isa_implver != IMPLVER_2106x) {
        return false;
    }
    tmp = tcg_temp_new_i64();
    tcg_gen_addi_i64(tmp, read_cpu_reg(s, a->rb), a->disp);
    gen_hw_st_ev4(s, tmp, cpu_reg(s, a->ra), a->type, a->len & 0b01);
    return true;
}

static bool trans_hw_st_ev5(DisasContext *s, arg_hw_st_ev5 *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }
    if (s->isa_implver != IMPLVER_21164) {
        return false;
    }

    tmp = tcg_temp_new_i64();
    tcg_gen_addi_i64(tmp, read_cpu_reg(s, a->rb), a->disp);
    gen_hw_st_ev5(s, tmp, cpu_reg(s, a->ra), a->type, a->len & 0b01);
    return true;
}

static bool trans_hw_st_ev6(DisasContext *s, arg_hw_st_ev6 *a)
{
    TCGv_i64 tmp;

    if (!palres_access_check(s)) {
        return false;
    }

    switch (s->isa_implver) {
    case IMPLVER_21264:
    case IMPLVER_21364:
        break;
    default:
        return false;
    }

    tmp = tcg_temp_new_i64();
    tcg_gen_addi_i64(tmp, read_cpu_reg(s, a->rb), a->disp);
    switch (a->type) {
    case EV6_HW_ST_PHYS:
    case EV6_HW_ST_PHYS_COND:
    case EV6_HW_ST_VIRT:
    case EV6_HW_ST_VIRT_ALT:
        gen_hw_st_ev6(s, tmp, cpu_reg(s, a->ra), a->type, a->len & 0b01);
        return true;
    default:
        return false;
    }
}

static bool trans_ldf(DisasContext *s, arg_ldf *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_load_fp(s, a->ra, a->rb, a->disp, gen_ldf);
    return true;
}

static bool trans_ldg(DisasContext *s, arg_ldg *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_load_fp(s, a->ra, a->rb, a->disp, gen_ldg);
    return true;
}

static bool trans_lds(DisasContext *s, arg_lds *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_load_fp(s, a->ra, a->rb, a->disp, gen_lds);
    return true;
}

static bool trans_ldt(DisasContext *s, arg_ldt *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_load_fp(s, a->ra, a->rb, a->disp, gen_ldt);
    return true;
}

static bool trans_stf(DisasContext *s, arg_stf *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_store_fp(s, a->ra, a->rb, a->disp, gen_stf);
    return true;
}

static bool trans_stg(DisasContext *s, arg_stg *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_store_fp(s, a->ra, a->rb, a->disp, gen_stg);
    return true;
}

static bool trans_sts(DisasContext *s, arg_sts *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_store_fp(s, a->ra, a->rb, a->disp, gen_sts);
    return true;
}

static bool trans_stt(DisasContext *s, arg_stt *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_store_fp(s, a->ra, a->rb, a->disp, gen_stt);
    return true;
}

static bool trans_ldl(DisasContext *s, arg_ldl *a)
{
    gen_load_int(s, a->ra, a->rb, a->disp, MO_LESL | MO_ALIGN, false, false);
    return true;
}

static bool trans_ldq(DisasContext *s, arg_ldq *a)
{
    gen_load_int(s, a->ra, a->rb, a->disp, MO_LEUQ | MO_ALIGN, false, false);
    return true;
}

static bool trans_ldl_l(DisasContext *s, arg_ldl_l *a)
{
    gen_load_int(s, a->ra, a->rb, a->disp, MO_LESL | MO_ALIGN, false, true);
    return true;
}

static bool trans_ldq_l(DisasContext *s, arg_ldq_l *a)
{
    gen_load_int(s, a->ra, a->rb, a->disp, MO_LEUQ | MO_ALIGN, false, true);
    return true;
}

static bool trans_stl(DisasContext *s, arg_stl *a)
{
    gen_store_int(s, a->ra, a->rb, a->disp, MO_LEUL | MO_ALIGN, false);
    return true;
}

static bool trans_stq(DisasContext *s, arg_stq *a)
{
    gen_store_int(s, a->ra, a->rb, a->disp, MO_LEUQ | MO_ALIGN, false);
    return true;
}

static bool trans_stl_c(DisasContext *s, arg_stl_c *a)
{
    gen_store_conditional(s, a->ra, a->rb, a->disp,
                          get_mem_index(s), MO_LEUL | MO_ALIGN);
    return true;
}

static bool trans_stq_c(DisasContext *s, arg_stq_c *a)
{
    gen_store_conditional(s, a->ra, a->rb, a->disp,
                          get_mem_index(s), MO_LEUQ | MO_ALIGN);
    return true;
}

static bool trans_br(DisasContext *s, arg_br *a)
{
    gen_bdirect(s, a->ra, a->disp);
    return true;
}

static bool trans_fbeq(DisasContext *s, arg_fbeq *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fbcond(s, TCG_COND_EQ, a->ra, a->disp);
    return true;
}

static bool trans_fblt(DisasContext *s, arg_fblt *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fbcond(s, TCG_COND_LT, a->ra, a->disp);
    return true;
}

static bool trans_fble(DisasContext *s, arg_fble *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fbcond(s, TCG_COND_LE, a->ra, a->disp);
    return true;
}

static bool trans_bsr(DisasContext *s, arg_bsr *a)
{
    gen_bdirect(s, a->ra, a->disp);
    return true;
}

static bool trans_fbne(DisasContext *s, arg_fbne *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fbcond(s, TCG_COND_NE, a->ra, a->disp);
    return true;
}

static bool trans_fbge(DisasContext *s, arg_fbge *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fbcond(s, TCG_COND_GE, a->ra, a->disp);
    return true;
}

static bool trans_fbgt(DisasContext *s, arg_fbgt *a)
{
    if (!fp_access_check(s)) {
        return false;
    }
    gen_fbcond(s, TCG_COND_GT, a->ra, a->disp);
    return true;
}

static bool trans_blbc(DisasContext *s, arg_blbc *a)
{
    gen_bcond(s, TCG_COND_TSTEQ, a->ra, a->disp);
    return true;
}

static bool trans_beq(DisasContext *s, arg_beq *a)
{
    gen_bcond(s, TCG_COND_EQ, a->ra, a->disp);
    return true;
}

static bool trans_blt(DisasContext *s, arg_blt *a)
{
    gen_bcond(s, TCG_COND_LT, a->ra, a->disp);
    return true;
}

static bool trans_ble(DisasContext *s, arg_ble *a)
{
    gen_bcond(s, TCG_COND_LE, a->ra, a->disp);
    return true;
}

static bool trans_blbs(DisasContext *s, arg_blbs *a)
{
    gen_bcond(s, TCG_COND_TSTNE, a->ra, a->disp);
    return true;
}

static bool trans_bne(DisasContext *s, arg_bne *a)
{
    gen_bcond(s, TCG_COND_NE, a->ra, a->disp);
    return true;
}

static bool trans_bge(DisasContext *s, arg_bge *a)
{
    gen_bcond(s, TCG_COND_GE, a->ra, a->disp);
    return true;
}

static bool trans_bgt(DisasContext *s, arg_bgt *a)
{
    gen_bcond(s, TCG_COND_GT, a->ra, a->disp);
    return true;
}

static void alpha_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    CPUAlphaState *env = cpu_env(cs);
    AlphaCPU *cpu = env_archcpu(env);
    uint32_t tb_flags = alpha_tbflags_from_tb(dc->base.tb);
    int bound;

    dc->cs = cs;
    dc->normal_mmu_idx = alpha_mmu_index(env, false);
    dc->altmode_mmu_idx = alpha_mmu_index_altmode(env);
    dc->pc_save = dc->base.pc_first;
    dc->ipregs = cpu->ipregs;
    dc->isa_procid = cpu->proc_id;
    dc->isa_amask = env->amask;
    dc->isa_implver = env->implver;
    dc->tb_rm = -1;
    dc->tb_ftz = -1;
    dc->pal_mode = EX_TBFLAG(tb_flags, PAL_MODE);
    dc->hwe_enabled = EX_TBFLAG(tb_flags, HWE);
    dc->sde_enabled = EX_TBFLAG(tb_flags, SDE);
    dc->fp_enabled = EX_TBFLAG(tb_flags, FEN);

    /* Bound the number of insns to execute to those left on the page.  */
    bound = -(dc->base.pc_first | TARGET_PAGE_MASK) / 4;
    dc->base.max_insns = MIN(dc->base.max_insns, bound);
}

static void alpha_tr_tb_start(DisasContextBase *dc, CPUState *cpu)
{
}

static void alpha_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    target_ulong pc_arg = dc->base.pc_next;

    if (tb_cflags(dcbase->tb) & CF_PCREL) {
        pc_arg &= ~TARGET_PAGE_MASK;
    }
    tcg_gen_insn_start(pc_arg, 0, 0);
    dc->insn_start_updated = false;
}

static void alpha_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *s = container_of(dcbase, DisasContext, base);
    CPUAlphaState *env = cpu_env(cs);
    uint64_t pc = s->base.pc_next;
    uint32_t insn;

    /* We can never have an unaligned PC. */
    assert((s->base.pc_next & 3) == 0);

    s->pc_curr = pc;
    s->insn = insn = translator_ldl_end(env, dcbase, pc, MO_LE);
    s->base.pc_next = pc + 4;

    if (unlikely(!decode(s, insn))) {
        unallocated_encoding(s);
    }

    /*
     * Alpha is a fixed-length ISA.  We performed the cross-page check
     * in init_disas_context by adjusting max_insns.
     */
}

static void alpha_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    DisasJumpType is_jmp = dc->base.is_jmp;

    switch (is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        gen_goto_tb(dc, 0, curr_insn_len(dc));
        break;
    case DISAS_CHAIN_UPDATE:
        gen_update_pc(dc, curr_insn_len(dc));
        QEMU_FALLTHROUGH;
    case DISAS_CHAIN:
        gen_goto_ptr(dc);
        break;

    case DISAS_EXIT_UPDATE:
        gen_update_pc(dc, curr_insn_len(dc));
        QEMU_FALLTHROUGH;
    case DISAS_EXIT:
        tcg_gen_exit_tb(NULL, 0);
        break;

    case DISAS_NORETURN:
        break;

    default:
        g_assert_not_reached();
    }
}

const TranslatorOps alpha_translator_ops = {
    .init_disas_context = alpha_tr_init_disas_context,
    .tb_start           = alpha_tr_tb_start,
    .insn_start         = alpha_tr_insn_start,
    .translate_insn     = alpha_tr_translate_insn,
    .tb_stop            = alpha_tr_tb_stop,
};
