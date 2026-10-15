/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Alpha emulation helpers for QEMU.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/qemu-print.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/memopidx.h"
#include "exec/memop.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/log.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ops.h"
#include "system/hw_accel.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "hw/core/clock.h"
#include "fpu/softfloat.h"
#include "cpu.h"
#include "ipregs.h"
#include "instmap.h"
#include "internals.h"
#include "syndrome.h"
#include "trace.h"

static uint64_t raw_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    assert(ri->fieldoffset);
    return IPREG_FIELD(env, ri);
}

static void raw_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                      uint64_t value)
{
    assert(ri->fieldoffset);
    IPREG_FIELD(env, ri) = value;
}

uint64_t read_raw_ipreg(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    if (ri->type & ALPHA_IP_CONST) {
        return ri->resetvalue;
    } else if (ri->raw_readfn) {
        return (*ri->raw_readfn)(env, ri);
    } else if (ri->readfn) {
        return (*ri->readfn)(env, ri);
    } else {
        return raw_read(env, ri);
    }
}

void write_raw_ipreg(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                     uint64_t value)
{
    if (ri->type & ALPHA_IP_CONST) {
        return;
    } else if (ri->raw_writefn) {
        (*ri->raw_writefn)(env, ri, value);
    } else if (ri->writefn) {
        (*ri->writefn)(env, ri, value);
    } else {
        raw_write(env, ri, value);
    }
}

static bool raw_accessors_invalid(const AlphaIPRegInfo *ri)
{
    if ((ri->type & ALPHA_IP_CONST) ||
        (ri->fieldoffset) ||
        ((ri->raw_writefn || ri->writefn) &&
         (ri->raw_readfn || ri->readfn))) {
        return false;
    }
    return true;
}

bool write_cpustate_to_list(AlphaCPU *cpu)
{
    int i;
    bool ok = true;

    for (i = 0; i < cpu->ipreg_array_len; i++) {
        const AlphaIPRegInfo *ri;
        uint32_t regidx = cpu->ipreg_indexes[i];
        uint64_t newval;

        ri = get_alpha_ipreg_info(cpu->ipregs, regidx);
        if (!ri) {
            ok = false;
            continue;
        }

        if (ri->type & ALPHA_IP_NO_RAW) {
            continue;
        }

        newval = read_raw_ipreg(&cpu->env, ri);
        cpu->ipreg_values[i] = newval;
    }

    return ok;
}

bool write_list_to_cpustate(AlphaCPU *cpu)
{
    int i;
    bool ok = true;

    for (i = 0; i < cpu->ipreg_array_len; i++) {
        const AlphaIPRegInfo *ri;
        uint32_t regidx = cpu->ipreg_indexes[i];
        uint64_t val = cpu->ipreg_values[i];

        ri = get_alpha_ipreg_info(cpu->ipregs, regidx);
        if (!ri) {
            ok = false;
            continue;
        }

        if (ri->type & ALPHA_IP_NO_RAW) {
            continue;
        }

        /*
         * Write value and confirm it reads back as written
         * (to catch read-only registers and partially read-only
         * registers where the incoming migration value doesn't match)
         */
        write_raw_ipreg(&cpu->env, ri, val);
        if (read_raw_ipreg(&cpu->env, ri) != val) {
            ok = false;
        }
    }
    return ok;
}

static void add_ipreg_to_list(gpointer key, gpointer opaque)
{
    AlphaCPU *cpu = opaque;
    uint32_t regidx = (uintptr_t)key;
    const AlphaIPRegInfo *ri = get_alpha_ipreg_info(cpu->ipregs, regidx);

    if (!(ri->type & (ALPHA_IP_NO_RAW | ALPHA_IP_ALIAS))) {
        cpu->ipreg_indexes[cpu->ipreg_array_len] = regidx;

        /* The value array need not be initialized at this point */
        cpu->ipreg_array_len++;
    }
}

static void count_ipreg(gpointer key, gpointer opaque)
{
    AlphaCPU *cpu = opaque;
    const AlphaIPRegInfo *ri;

    ri = g_hash_table_lookup(cpu->ipregs, key);
    if (!(ri->type & (ALPHA_IP_NO_RAW | ALPHA_IP_ALIAS))) {
        cpu->ipreg_array_len++;
    }
}

static gint ipreg_key_compare(gconstpointer a, gconstpointer b)
{
    uint64_t aidx = (uintptr_t)a;
    uint64_t bidx = (uintptr_t)b;

    if (aidx > bidx) {
        return 1;
    }
    if (aidx < bidx) {
        return -1;
    }
    return 0;
}

void alpha_cpu_init_ipreg_list(AlphaCPU *cpu)
{
    GList *keys;
    int arraylen;

    /*
     * Initialize the ipreg_tuples[] array based on the ipregs hash.
     *
     * Note that we require ipreg_tuples[] to be sorted by key ID.
     */
    keys = g_hash_table_get_keys(cpu->ipregs);
    keys = g_list_sort_with_data(keys, (GCompareDataFunc)ipreg_key_compare, NULL);
    cpu->ipreg_array_len = 0;

    g_list_foreach(keys, count_ipreg, cpu);
    arraylen = cpu->ipreg_array_len;
    cpu->ipreg_indexes = g_new(uint64_t, arraylen);
    cpu->ipreg_values = g_new(uint64_t, arraylen);
    cpu->ipreg_vmstate_indexes = g_new(uint64_t, arraylen);
    cpu->ipreg_vmstate_values = g_new(uint64_t, arraylen);
    cpu->ipreg_vmstate_array_len = cpu->ipreg_array_len;
    cpu->ipreg_array_len = 0;

    g_list_foreach(keys, add_ipreg_to_list, cpu);
    assert(cpu->ipreg_array_len == arraylen);

    g_list_free(keys);
}

const AlphaIPRegInfo *get_alpha_ipreg_info(GHashTable *cpregs, uint16_t iprn)
{
    return g_hash_table_lookup(cpregs, (gpointer)(uintptr_t)iprn);
}

void alpha_ip_write_ignore(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
}

uint64_t alpha_ip_read_zero(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return 0;
}

void alpha_ip_reset_ignore(CPUAlphaState *env, const AlphaIPRegInfo *opaque)
{
}

static void add_ipreg_to_hashtable(AlphaCPU *cpu, const AlphaIPRegInfo *ri,
                                   void *opaque, uint32_t iprn,
                                   const char *name)
{
    AlphaIPRegInfo *ri2;
    size_t name_len;

    g_assert(get_alpha_ipreg_info(cpu->ipregs, iprn) == NULL);

    /* Combine the register info and name into one allocation. */
    name_len = strlen(name) + 1;
    ri2 = (void *)g_new0(char, sizeof(*ri2) + name_len);
    *ri2 = *ri;
    ri2->name = memcpy(ri2 + 1, name, name_len);

    /*
     * Update fields to match the instantiation, overwiting wildcards.
     */
    ri2->ipr = iprn;
    if (opaque) {
        ri2->opaque = opaque;
    }

    /*
     * Special registers are never migratable and not even raw-accessible.
     */
    if (ri2->type & ALPHA_IP_NOP) {
        ri2->type |= ALPHA_IP_NO_RAW;
    }

    if (HOST_BIG_ENDIAN && ri2->fieldoffset) {
        ri2->fieldoffset += sizeof(uint32_t);
    }

    /*
     * Check that raw accesses are either forbidden or handled.
     */
    if (!(ri2->type & ALPHA_IP_NO_RAW)) {
        assert(!raw_accessors_invalid(ri2));
    }
    g_hash_table_insert(cpu->ipregs, (gpointer)(uintptr_t)iprn, ri2);
}

void define_one_alpha_ipreg_with_opaque(AlphaCPU *cpu,
                                        const AlphaIPRegInfo *ri,
                                        void *opaque)
{
    add_ipreg_to_hashtable(cpu, ri, opaque, ri->ipr, ri->name);
}

/* Define a whole list of registers */
void define_alpha_ipregs_with_opaque_len(AlphaCPU *cpu,
                                         const AlphaIPRegInfo *regs,
                                         void *opaque, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        define_one_alpha_ipreg_with_opaque(cpu, regs + i, opaque);
    }
}

static void ipreg_reset(gpointer key, gpointer value, gpointer opaque)
{
    AlphaIPRegInfo *ri = value;
    AlphaCPU *cpu = opaque;

    if (ri->type & ALPHA_IP_ALIAS) {
        return;
    }

    if (ri->resetfn) {
        (*ri->resetfn)(&cpu->env, ri);
        return;
    }

    /* A zero offset is never possible as it would be regs[0]
     * so we use it to indicate that reset is being handled elsewhere.
     */
    if (!ri->fieldoffset) {
        return;
    }

    IPREG_FIELD(&cpu->env, ri) = ri->resetvalue;
}

static void ipreg_check_reset(gpointer key, gpointer value,  gpointer opaque)
{
    AlphaIPRegInfo *ri = value;
    AlphaCPU *cpu = opaque;
    uint64_t oldvalue, newvalue;

    if (ri->type & (ALPHA_IP_ALIAS | ALPHA_IP_NO_RAW)) {
        return;
    }

    /*
     * Purely an assertion check: we've already done reset once,
     * so now check that running the reset for the cpreg doesn't
     * change its value. This traps bugs where two different cpregs
     * both try to reset the same state field but to different values.
     */
    oldvalue = read_raw_ipreg(&cpu->env, ri);
    ipreg_reset(key, value, opaque);
    newvalue = read_raw_ipreg(&cpu->env, ri);
    assert(oldvalue == newvalue);
}

void alpha_cpu_reset_ipregs(AlphaCPU *cpu)
{
    g_hash_table_foreach(cpu->ipregs, ipreg_reset, cpu);
    g_hash_table_foreach(cpu->ipregs, ipreg_check_reset, cpu);
}

static uint32_t alpha_rebuild_hflags_value(CPUAlphaState *env)
{
    uint32_t hflags = 0;

    /*
     * 6.2 PALmode Environment
     *
     * Istream memory mapping is disabled. Because the PALcode is used to
     * implement translation buffer fill routines, Istream mapping clearly
     * cannot be enabled. Dstream mapping is still enabled.
     */
    DP_TBFLAG(hflags, DMMU_IDX, alpha_mmu_index(env, false));
    DP_TBFLAG(hflags, IMMU_IDX, alpha_mmu_index(env, true));
    DP_TBFLAG(hflags, PAL_MODE, alpha_is_pal(env));

    /* Handle IPR flags. */
    DP_TBFLAG(hflags, FEN, alpha_fpu_enabled(env));

    /*
     * 6.6 PALshadow Registers
     *
     * The 21264/EV6 contains eight extra virtual integer registers, called
     * shadow registers, which are available to PALcode for use as scratch
     * space and storage for commonly used values. These registers are made
     * available under the control of the SDE[1] field of the I_CTL IPR.
     * These shadow registers overlay R4 through R7 and R20 through R23,
     * when the CPU is in PALmode and SDE[1] is set.
     */
    DP_TBFLAG(hflags, SDE, alpha_palshadow_enabled(env));

    /*
     * Table 5–10 Ibox Control Register Fields Description (Continued)
     *
     * If set, allow PALRES intructions to be executed in kernel mode. Note
     * that modification of the ITB while in kernel mode/native mode may
     * cause UNPREDICTABLE behavior.
     */
    DP_TBFLAG(hflags, HWE, alpha_palres_enabled(env));

    return hflags;
}

void alpha_rebuild_hflags(CPUAlphaState *env)
{
    env->hflags = alpha_rebuild_hflags_value(env);
}

void assert_hflags_rebuild_correctly(CPUAlphaState *env)
{
    uint32_t hflags_current = env->hflags;
    uint32_t hflags_rebuilt = alpha_rebuild_hflags_value(env);

    if (unlikely(hflags_current != hflags_rebuilt)) {
        cpu_abort(env_cpu(env),
                  "TCG hflags mismatch (current:0x%08x rebuilt:0x%08x)\n",
                  hflags_current, hflags_rebuilt);
    }
}

/*
 * Return the underlying cycle count for the PMU cycle counters. If we're in
 * usermode, simply return 0.
 */
static uint64_t ns_to_ticks(CPUAlphaState *env, uint64_t value)
{
    AlphaCPU *cpu = env_archcpu(env);

    return clock_ns_to_ticks(cpu->sysclk, value);
}

static bool cycle_counter_enabled(CPUAlphaState *env)
{
    return IPR_EX64(cc_ctl, CC_CTL, CC_ENA);
}

static uint64_t cycle_counter_offset(CPUAlphaState *env)
{
    return IPR_EX64(cc_ctl, CC_CTL, COUNTER);
}

static uint64_t cycle_counter_for_timestamp(CPUAlphaState *env, uint64_t now)
{
    uint64_t ticks;

    if (!cycle_counter_enabled(env)) {
        /* Counter is disabled and does not increment */
        return env->cc_ticks_then;
    }

    ticks = ns_to_ticks(env, now - env->cc_ns_then);
    return env->cc_ticks_then + ticks;
}

static uint64_t cycle_counter_value(CPUAlphaState *env)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks = cycle_counter_for_timestamp(env, now);

    /* Every read takes a cycle, however coarse the host clock is. */
    if (cycle_counter_enabled(env) && ticks <= env->cc_ticks_read) {
        ticks = env->cc_ticks_read + 1;
    }
    env->cc_ticks_read = ticks;
    return ticks;
}

uint64_t alpha_read_cyclecounter(CPUAlphaState *env)
{
    uint64_t cc = 0;

    if (env->implver == IMPLVER_2106x) {
        SET_FIELD(cc, CC, COUNTER_FULL, cycle_counter_value(env));
    } else {
        SET_FIELD(cc, CC, COUNTER, cycle_counter_value(env));
    }
    SET_FIELD(cc, CC, OFFSET, cycle_counter_offset(env));
    return cc;
}

void cpu_get_tb_cpu_state(CPUAlphaState *env, vaddr *pc,
                          uint64_t *cs_base, uint32_t *flags)
{
    assert_hflags_rebuild_correctly(env);

    *pc = env->pc;
    *cs_base = 0;
    *flags = env->hflags;
}

static inline bool alpha_cpu_sirq_pending(CPUAlphaState *env)
{
    return alpha_get_sir(env) & alpha_get_sien(env);
}

static inline bool alpha_cpu_astirq_pending(CPUAlphaState *env)
{
    return alpha_pending_ast(env);
}

static inline bool alpha_cpu_eirq_pending(CPUAlphaState *env)
{
    return alpha_get_eir(env) & alpha_get_eien(env);
}

static void alpha_cpu_update_astirq(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);
    bool new_state = alpha_cpu_astirq_pending(env);

    if (new_state != ((cs->interrupt_request & CPU_INTERRUPT_ASTIRQ) != 0)) {
        if (new_state) {
            cpu_interrupt(cs, CPU_INTERRUPT_ASTIRQ);
        } else {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_ASTIRQ);
        }
    }
}

static void alpha_cpu_update_sirq(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);
    bool new_state = alpha_cpu_sirq_pending(env);

    if (new_state != ((cs->interrupt_request & CPU_INTERRUPT_SIRQ) != 0)) {
        if (new_state) {
            cpu_interrupt(cs, CPU_INTERRUPT_SIRQ);
        } else {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_SIRQ);
        }
    }
}

static void alpha_cpu_update_irq(CPUState *cs)
{
    g_assert(bql_locked());
    alpha_cpu_update_sirq(cs);
    alpha_cpu_update_astirq(cs);
}

void alpha_cpu_do_system_reset(CPUState *cs)
{
    cs->exception_index = EXCP_RESET;
    alpha_cpu_do_interrupt(cs);
}

typedef struct UpdateExceptionFlags {
    bool load_insn;
    bool update_exc_sum;
    bool update_mm_stat;
    bool update_va;
    bool data_fault;
} UpdateExceptionFlags;

#define COPY_FIELD(dst, r1, f1, src, r2, f2)                            \
    SET_FIELD(dst, r1, f1, FIELD_EX64(src, r2, f2))

static inline uint8_t convert_opcode(uint8_t opc)
{
    /*
     * Table 5–17 Memory Management Status Register Fields Description:
     *
     * HW_LD is displayed as 3 and HW_ST is displayed as 7.
     */
    return (opc == OPC_HW_LD || opc == OPC_HW_ST) ? opc - 0x18 : opc;
}

/*
 * The 21064 latches the unmasked Rb + disp of LDQ_U/STQ_U in VA. NT 3.1's
 * loader (LprpUnprotectThunk) matches it against the 4-byte IAT slot.
 */
static uint64_t ev4_unmasked_va(CPUAlphaState *env, uint32_t insn,
                                uint64_t va)
{
    uint32_t opc = get_opc(insn), rb = get_rb(insn);
    uint64_t ea;

    if (opc != OPC_LDQ_U && opc != OPC_STQ_U) {
        return va;
    }
    ea = (rb == 31 ? 0 : alpha_cpu_get_reg_value(env, rb)) + (int16_t)insn;
    return (ea & ~7ULL) == (va & ~7ULL) ? ea : va;
}

static void alpha_update_exception_state_ev4(CPUAlphaState *env,
                                             UpdateExceptionFlags flags,
                                             target_long diff)
{
    uint64_t exc_addr = env->pc + diff;
    uint64_t exc_sum = 0;
    uint64_t mm_stat = 0;
    uint32_t syndrome, iss;

    /* Handle flag specific updates. */
    syndrome = env->exception.syndrome;
    iss = syn_get_iss(syndrome);
    switch (syn_get_ec(syndrome)) {
    case EC_MMUABORT:
        COPY_FIELD(mm_stat, EV4_MMCSR, FOW, iss, MMU_ISS, FOW);
        COPY_FIELD(mm_stat, EV4_MMCSR, FOR, iss, MMU_ISS, FOR);
        COPY_FIELD(mm_stat, EV4_MMCSR, ACV, iss, MMU_ISS, ACV);
        COPY_FIELD(mm_stat, EV4_MMCSR, WR, iss, MMU_ISS, WR);
        break;
    case EC_ARITH:
        COPY_FIELD(exc_sum, EV4_EXC_SUM, SWC, iss, ARITH_ISS, SWC);
        COPY_FIELD(exc_sum, EV4_EXC_SUM, INV, iss, ARITH_ISS, INV);
        COPY_FIELD(exc_sum, EV4_EXC_SUM, DZE, iss, ARITH_ISS, DZE);
        COPY_FIELD(exc_sum, EV4_EXC_SUM, FOV, iss, ARITH_ISS, FOV);
        COPY_FIELD(exc_sum, EV4_EXC_SUM, UNF, iss, ARITH_ISS, UNF);
        COPY_FIELD(exc_sum, EV4_EXC_SUM, INE, iss, ARITH_ISS, INE);
        COPY_FIELD(exc_sum, EV4_EXC_SUM, IOV, iss, ARITH_ISS, IOV);
        break;
    default:
        break;
    }

    if (flags.load_insn) {
        MemOpIdx oi = make_memop_idx(MO_LEUL, cpu_mmu_index(env_cpu(env), true));
        uint32_t insn = cpu_ldl_code_mmu(env, env->pc + diff, oi, 0);

        SET_FIELD(mm_stat, EV4_MMCSR, RA, get_ra(insn));
        SET_FIELD(mm_stat, EV4_MMCSR, OPCODE, get_opc(insn));
        if (flags.data_fault && !alpha_is_pal(env)) {
            env->exception.vaddress =
                ev4_unmasked_va(env, insn, env->exception.vaddress);
        }
    }

    env->ipr.exc_addr = exc_addr | alpha_is_pal(env);
    if (flags.update_exc_sum) {
        env->ipr.exc_sum = exc_sum;
    }
    if (flags.update_mm_stat) {
        env->ipr.mm_stat = mm_stat;
    }
    if (flags.update_va) {
        env->ipr.va = env->exception.vaddress;
    }
}

static void alpha_update_exception_state_ev5(CPUAlphaState *env,
                                             UpdateExceptionFlags flags,
                                             target_long diff)
{
    uint64_t exc_addr = env->pc + diff;
    uint64_t exc_sum = 0;
    uint64_t mm_stat = 0;
    uint32_t syndrome, iss;

    /* Handle flag specific updates. */
    syndrome = env->exception.syndrome;
    iss = syn_get_iss(syndrome);
    switch (syn_get_ec(syndrome)) {
    case EC_MMUABORT:
        COPY_FIELD(mm_stat, EV5_MM_STAT, FOW, iss, MMU_ISS, FOW);
        COPY_FIELD(mm_stat, EV5_MM_STAT, FOR, iss, MMU_ISS, FOR);
        COPY_FIELD(mm_stat, EV5_MM_STAT, ACV, iss, MMU_ISS, ACV);
        COPY_FIELD(mm_stat, EV5_MM_STAT, WR, iss, MMU_ISS, WR);
        break;
    case EC_ARITH:
        COPY_FIELD(exc_sum, EV5_EXC_SUM, SWC, iss, ARITH_ISS, SWC);
        COPY_FIELD(exc_sum, EV5_EXC_SUM, INV, iss, ARITH_ISS, INV);
        COPY_FIELD(exc_sum, EV5_EXC_SUM, DZE, iss, ARITH_ISS, DZE);
        COPY_FIELD(exc_sum, EV5_EXC_SUM, FOV, iss, ARITH_ISS, FOV);
        COPY_FIELD(exc_sum, EV5_EXC_SUM, UNF, iss, ARITH_ISS, UNF);
        COPY_FIELD(exc_sum, EV5_EXC_SUM, INE, iss, ARITH_ISS, INE);
        COPY_FIELD(exc_sum, EV5_EXC_SUM, IOV, iss, ARITH_ISS, IOV);
        break;
    default:
        break;
    }

    if (flags.load_insn) {
        MemOpIdx oi = make_memop_idx(MO_LEUL, cpu_mmu_index(env_cpu(env), true));
        uint32_t insn = cpu_ldl_code_mmu(env, env->pc + diff, oi, 0);
        int opc = get_opc(insn);

        switch (opc) {
        case OPC_ITFP:
        case OPC_FLTV:
        case OPC_FLTI:
        case OPC_FLTL:
            env->ipr.exc_mask |= BIT((uint64_t)get_ra(insn) << 32);
            break;
        default:
            env->ipr.exc_mask |= BIT(get_ra(insn));
            break;
        }

        SET_FIELD(exc_sum, EV5_MM_STAT, RA, get_ra(insn));
        SET_FIELD(mm_stat, EV5_MM_STAT, OPCODE, get_opc(insn));
    }

    env->ipr.exc_addr = exc_addr | alpha_is_pal(env);
    if (flags.update_exc_sum) {
        env->ipr.exc_sum = exc_sum;
    }
    if (flags.update_mm_stat) {
        env->ipr.mm_stat = mm_stat;
    }
    if (flags.update_va) {
        env->ipr.va = env->exception.vaddress;
    }
}

static void alpha_update_exception_state_ev6(CPUAlphaState *env,
                                             UpdateExceptionFlags flags,
                                             target_long diff)
{
    uint64_t exc_addr = env->pc + diff;
    uint64_t exc_sum = 0;
    uint64_t mm_stat = 0;
    uint32_t syndrome, iss;

    /* Handle flag specific updates. */
    syndrome = env->exception.syndrome;
    iss = syn_get_iss(syndrome);
    switch (syn_get_ec(syndrome)) {
    case EC_MMUABORT:
        COPY_FIELD(mm_stat, EV6_MM_STAT, FOW, iss, MMU_ISS, FOW);
        COPY_FIELD(mm_stat, EV6_MM_STAT, FOR, iss, MMU_ISS, FOR);
        COPY_FIELD(mm_stat, EV6_MM_STAT, ACV, iss, MMU_ISS, ACV);
        COPY_FIELD(mm_stat, EV6_MM_STAT, WR, iss, MMU_ISS, WR);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, PC_OVFL, iss, MMU_ISS, OVFL);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, BAD_IVA, iss, MMU_ISS, IVA);
        break;
    case EC_ARITH:
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SWC, iss, ARITH_ISS, SWC);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, INV, iss, ARITH_ISS, INV);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, DZE, iss, ARITH_ISS, DZE);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, FOV, iss, ARITH_ISS, FOV);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, UNF, iss, ARITH_ISS, UNF);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, INE, iss, ARITH_ISS, INE);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, IOV, iss, ARITH_ISS, IOV);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, INT, iss, ARITH_ISS, INT);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SET_INV, iss, ARITH_ISS, SET_INV);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SET_DZE, iss, ARITH_ISS, SET_DZE);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SET_FOV, iss, ARITH_ISS, SET_FOV);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SET_UNF, iss, ARITH_ISS, SET_UNF);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SET_INE, iss, ARITH_ISS, SET_INE);
        COPY_FIELD(exc_sum, EV6_EXC_SUM, SET_IOV, iss, ARITH_ISS, SET_IOV);
        break;
    default:
        break;
    }

    if (flags.load_insn) {
        MemOpIdx oi = make_memop_idx(MO_LEUL, cpu_mmu_index(env_cpu(env), true));
        uint32_t insn = cpu_ldl_code_mmu(env, env->pc + diff, oi, 0);
        int opc = get_opc(insn);

        switch (opc) {
        case OPC_INTA:
        case OPC_INTL:
        case OPC_INTS:
        case OPC_INTM:
        case OPC_ITFP:
        case OPC_FLTV:
        case OPC_FLTI:
        case OPC_FLTL:
            SET_FIELD(exc_sum, EV6_EXC_SUM, REG, get_rc(insn));
            break;
        default:
            SET_FIELD(exc_sum, EV6_EXC_SUM, REG, get_ra(insn));
            break;
        }
        SET_FIELD(mm_stat, EV6_MM_STAT, OPCODE, convert_opcode(opc));
    }

    env->ipr.exc_addr = exc_addr | alpha_is_pal(env);
    if (flags.update_exc_sum) {
        env->ipr.exc_sum = sextract64(exc_sum, 0, R_EV6_EXC_SUM_SEXT0_SHIFT);
    }
    if (flags.update_mm_stat) {
        env->ipr.mm_stat = mm_stat;
    }
    if (flags.update_va) {
        env->ipr.va = env->exception.vaddress;
    }
}

static void alpha_update_exception_state(CPUAlphaState *env,
                                         UpdateExceptionFlags flags,
                                         target_long diff)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        alpha_update_exception_state_ev4(env, flags, diff);
        break;
    case IMPLVER_21164:
        alpha_update_exception_state_ev5(env, flags, diff);
        break;
    case IMPLVER_21264:
        alpha_update_exception_state_ev6(env, flags, diff);
        break;
    default:
        g_assert_not_reached();
    }
}

static void alpha_log_exception(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);
    int idx = cs->exception_index;

    qemu_log_mask(CPU_LOG_INT, "Taking exception %d [%s(0x%08x)] on CPU %d "
                  "at 0x" TARGET_FMT_lx "\n",
                  idx, alpha_cpu_exception_name(idx), env->exception.syndrome,
                  cs->cpu_index, env->pc);
}

static void do_hw_rei(CPUAlphaState *env)
{
    uint64_t new_pc = env->exception.vaddress;
    bool to_pal = new_pc & R_PC_PAL_MODE_MASK;

    /*
     * We can enter this routine by either a hw_rei being executed in PALmode
     * to normal mode, or from kernel mode to PAL mode with PALRES enabled
     * (though the latter is unlikely because it makes little sense to
     * actually do).
     */
    qemu_log_mask(CPU_LOG_INT, "Exception return: resuming execution in "
                  "%s mode at 0x" TARGET_FMT_lx "\n",
                  to_pal ? "PAL" : "normal", new_pc & ~R_PC_PAL_MODE_MASK);

    /* Update AST and SIR state. */
    alpha_cpu_update_irq(env_cpu(env));

    /* We have a successful exception exit. */
    alpha_env_set_pc(env, new_pc);
}

static void do_call_pal(CPUAlphaState *env)
{
    uint64_t new_pc;

    /*
     * As per 6.8.1 CALL_PAL Entry Points, the CALL_PC program counter
     * value is as follows:
     *
     * - PC[63:15]  = PAL_BASE[63:15]
     * - PC[14]     = 0
     * - PC[13]     = 1
     * - PC[12]     = CALL_PAL function field [7]
     * - PC[11:6]   = CALL_PAL function field [5:0]
     * - PC[5:1]    = 0
     * - PC[0]      = 1 (PALmode)
     */
    new_pc  = alpha_get_pal_base(env);
    new_pc |= BIT(13);
    new_pc |= (extract32(env->exception.syndrome, 7, 1) << 12);
    new_pc |= (extract32(env->exception.syndrome, 0, 6) << 6);
    new_pc |= R_PC_PAL_MODE_MASK;

    /* Set the new PC. */
    alpha_env_set_pc(env, new_pc);
}

static inline int map_exception_type(int excp)
{
    switch (excp) {
    case EXCP_SIRQ:
    case EXCP_ASTIRQ:
        return EXCP_IRQ;
    default:
        return excp;
    }
}

void alpha_cpu_do_interrupt(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);
    UpdateExceptionFlags flags = {};
    int excp = map_exception_type(cs->exception_index);
    uint64_t new_pc, vector;

    alpha_log_exception(cs);
    alpha_clear_llsc_addr(env);

    switch (excp) {
    case EXCP_DTBM_DOUBLE_3:
    case EXCP_DTBM_DOUBLE_4:
    case EXCP_FEN:
    case EXCP_UNALIGNED:
    case EXCP_DTBM_SINGLE:
    case EXCP_DFAULT:
    case EXCP_OPCDEC:
    case EXCP_IACV:
    case EXCP_MCHK:
    case EXCP_ITB_MISS:
    case EXCP_ARITH:
    case EXCP_IRQ:
    case EXCP_MT_FPCR:
    case EXCP_RESET:
        vector = env->excp_vectors[excp];
        if (unlikely(vector == (target_ulong)(-1ULL))) {
            cpu_abort(env_cpu(env),
                      "raised exception %d without defined vector offset\n",
                      excp);
        }

        /*
         * Use the default exception offsets.
         */
        new_pc  = alpha_get_pal_base(env);
        new_pc |= vector;
        new_pc |= R_PC_PAL_MODE_MASK;
        break;

    case EXCP_CALL_PAL:
        do_call_pal(env);
        goto out;

    case EXCP_HW_REI:
        do_hw_rei(env);
        goto out;

    default:
        cpu_abort(cs, "Unhandled exception 0x%x", cs->exception_index);
    }

    /*
     * 5.2.6 Exception Address Register – EXC_ADDR
     *
     * The exception address register (EXC_ADDR) is a read-only register
     * that is updated by hardware when it encounters an exception or
     * interrupt. EXC_ADDR[0] is set if the associated exception occurred
     * in PAL mode.
     *
     * The exception actions are listed here:
     *  - If the exception was a fault or a synchronous trap, EXC_ADDR
     *    contains the PC of the instruction that triggered the fault or trap.
     *  - If the exception was an interrupt, EXC_ADDR contains the PC of the
     *    next instruction that would have executed if the interrupt had not
     *    occurred.
     *
     * References for implicitly updated registers(?):
     *
     *  Exception Name  Updated Registers
     *  --------------  -----------------------------------------------------
     *  DTBM_DOUBLE_3   EXC_ADDR
     *  DTBM_DOUBLE_4   EXC_ADDR
     *  FEN             EXC_ADDR
     *  UNALIGNED       EXC_ADDR    EXC_SUM     MM_STAT     VA      VA_FORM
     *  DTBM_SINGLE     EXC_ADDR    EXC_SUM     MM_STAT     VA      VA_FORM
     *  DFAULT          EXC_ADDR    EXC_SUM     MM_STAT     VA      VA_FORM
     *  OPCDEC          EXC_ADDR
     *  IACV            EXC_ADDR    EXC_SUM                 VA      VA_FORM
     *  MCHK            EXC_ADDR
     *  ITB_MISS        EXC_ADDR                                    IVA_FORM
     *  ARITH           EXC_ADDR    EXC_SUM
     *  IRQ             EXC_ADDR    ISUM
     *  MT_FPCR         EXC_ADDR
     *  RESET
     */

    switch (excp) {
    case EXCP_DTBM_DOUBLE_3:
    case EXCP_DTBM_DOUBLE_4:
        if (env->implver == IMPLVER_2106x) {
            flags.load_insn = true;
            flags.update_exc_sum = true;
            flags.update_mm_stat = true;
            flags.update_va = true;
            alpha_update_exception_state(env, flags, 0);
            qemu_log_mask(CPU_LOG_INT,
                          "...with EXC_SUM 0x%" PRIx64 "\n"
                          "...with MM_STAT 0x%" PRIx64 "\n"
                          "...with VA 0x%" PRIx64 "\n",
                          env->ipr.exc_sum,
                          env->ipr.mm_stat,
                          env->ipr.va);
        } else {
            alpha_update_exception_state(env, flags, 0);
        }
        break;

    case EXCP_FEN:
    case EXCP_OPCDEC:
    case EXCP_MCHK:
    case EXCP_MT_FPCR:
        alpha_update_exception_state(env, flags, 0);
        break;

    case EXCP_UNALIGNED:
    case EXCP_DTBM_SINGLE:
    case EXCP_DFAULT:
        flags.load_insn = true;
        flags.update_exc_sum = true;
        flags.update_mm_stat = true;
        flags.update_va = true;
        flags.data_fault = true;
        alpha_update_exception_state(env, flags, 0);
        qemu_log_mask(CPU_LOG_INT,
                      "...with EXC_SUM 0x%" PRIx64 "\n"
                      "...with MM_STAT 0x%" PRIx64 "\n"
                      "...with VA 0x%" PRIx64 "\n",
                      env->ipr.exc_sum,
                      env->ipr.mm_stat,
                      env->ipr.va);
        break;

    case EXCP_IACV:
        flags.update_exc_sum = true;
        alpha_update_exception_state(env, flags, 0);
        qemu_log_mask(CPU_LOG_INT,
                      "...with EXC_SUM 0x%" PRIx64 "\n",
                      env->ipr.exc_sum);
        break;

    case EXCP_ITB_MISS:
        alpha_update_exception_state(env, flags, 0);
        break;

    case EXCP_ARITH:
        flags.load_insn = true;
        flags.update_exc_sum = true;
        alpha_update_exception_state(env, flags, 0);
        qemu_log_mask(CPU_LOG_INT,
                      "...with EXC_SUM 0x%" PRIx64 "\n",
                      env->ipr.exc_sum);
        break;

    case EXCP_IRQ:
        assert(!alpha_is_pal(env));
        alpha_update_exception_state(env, flags, 0);
        break;

    case EXCP_RESET:
        break;

    default:
        g_assert_not_reached();
    }

    if (!excp_is_internal(excp)) {
        qemu_log_mask(CPU_LOG_INT,
                      "...with EXC_ADDR 0x%" PRIx64 "\n"
                      "...with PC 0x%" PRIx64 "\n"
                      "...with CM 0x%" PRId32 "\n",
                      env->ipr.exc_addr,
                      env->pc,
                      alpha_cur_mode(env, true));
        env->excp_stats[excp]++;
    }

    alpha_env_set_pc(env, new_pc);
    cs->exception_index = EXCP_NONE;

out:
    alpha_rebuild_hflags(env);
    cpu_interrupt_exittb(cs);
}

/*
 * Cycle counter helper functions.
 */
static uint64_t cc_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return alpha_read_cyclecounter(env);
}

static void cc_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                     uint64_t value)
{
    uint64_t offset;

    /*
     * 5.1.1 Cycle Counter Register
     *
     * A HW_MTPR instruction to the CC writes the upper half of the register
     * and leaves the lower half unchanged.
     */
    offset = FIELD_EX64(value, CC, OFFSET);
    SET_FIELD(env->ipr.cc_ctl, CC_CTL, COUNTER, offset);
}

static void cc_ctl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                         uint64_t value)
{
    if ((value ^ env->ipr.cc_ctl) & R_CC_CTL_CC_ENA_MASK) {
        /*
         * Whether the counter is being enabled or disabled, the
         * required action is the same: sync the (ns_then, ticks_then)
         * tuple.
         */
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        env->cc_ticks_then = MAX(cycle_counter_for_timestamp(env, now),
                                 env->cc_ticks_read);
        env->cc_ns_then = now;
    }
    env->ipr.cc_ctl = value & (R_CC_CTL_CC_ENA_MASK | R_CC_CTL_COUNTER_MASK);
}

static void pal_base_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
    env->ipr.pal_base = value & alpha_get_pal_base_mask(env->implver);
}

static inline uint64_t ev4_xtb_pte_temp_init(uint64_t pte, bool itb)
{
    uint64_t ret = 0;

    if (!itb) {
        ret = deposit64(ret, 3, 1, FIELD_EX64(pte, PTE, FOR));
        ret = deposit64(ret, 4, 1, FIELD_EX64(pte, PTE, FOW));
        ret = deposit64(ret, 5, 1, FIELD_EX64(pte, PTE, KWE));
        ret = deposit64(ret, 6, 1, FIELD_EX64(pte, PTE, EWE));
        ret = deposit64(ret, 7, 1, FIELD_EX64(pte, PTE, SWE));
        ret = deposit64(ret, 8, 1, FIELD_EX64(pte, PTE, UWE));
    }
    ret = deposit64(ret, 9, 1, FIELD_EX64(pte, PTE, KRE));
    ret = deposit64(ret, 10, 1, FIELD_EX64(pte, PTE, ERE));
    ret = deposit64(ret, 11, 1, FIELD_EX64(pte, PTE, SRE));
    ret = deposit64(ret, 12, 1, FIELD_EX64(pte, PTE, URE));
    ret = deposit64(ret, 34, 1, FIELD_EX64(pte, PTE, ASM));
    ret = deposit64(ret, 13, 21,
                    itb ? FIELD_EX64(pte, EV4_ITB_PTE, PFN)
                        : FIELD_EX64(pte, EV4_DTB_PTE, PFN));
    return ret;
}

static void ev4_tb_tag_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    raw_write(env, ri, value & R_EV4_TB_TAG_VA_MASK);
}

static void ev4_itb_pte_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Instruction;
    int asn = alpha_cur_asn(env, type, 0);
    uint64_t mask = R_EV4_ITB_PTE_ASM_MASK |
                    R_EV4_ITB_PTE_KRE_MASK | R_EV4_ITB_PTE_ERE_MASK |
                    R_EV4_ITB_PTE_SRE_MASK | R_EV4_ITB_PTE_URE_MASK |
                    R_EV4_ITB_PTE_PFN_MASK;

    value &= mask;
    value |= env->ipr.tb_ctl;
    alpha_tlb_fill(env, type, env->ipr.tb_tag, value, asn);
    env->ipr.itb_pte = value;
    env->ipr.itb_pte_temp = ev4_xtb_pte_temp_init(value, true);
}

static uint64_t ev4_itb_pte_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return env->ipr.itb_pte_temp;
}

static void ev4_iccsr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    int asn = FIELD_EX64(value, EV4_ICCSR, ASN);
    uint64_t wmask = R_EV4_ICCSR_ASN_MASK | R_EV4_ICCSR_PME1_MASK |
                     R_EV4_ICCSR_PME0_MASK | R_EV4_ICCSR_FPE_MASK |
                     R_EV4_ICCSR_MAP_MASK | R_EV4_ICCSR_HWE_MASK |
                     R_EV4_ICCSR_DI_MASK | R_EV4_ICCSR_BHE_MASK |
                     R_EV4_ICCSR_JSE_MASK | R_EV4_ICCSR_BPE_MASK |
                     R_EV4_ICCSR_PIPE_MASK | R_EV4_ICCSR_MUX1_MASK |
                     R_EV4_ICCSR_MUX0_MASK | R_EV4_ICCSR_PC0_MASK |
                     R_EV4_ICCSR_PC1_MASK;

    /* Only flush TLBs if we're switching ASIDs. */
    if (alpha_itb_asn(env) != asn) {
        tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    }
    MERGE_FIELD(env->ipr.iccsr, value, wmask);
    alpha_rebuild_hflags(env);
}

static uint64_t ev4_iccsr_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    uint64_t ret = 0;

    ret = deposit64(ret, 1, 1, IPR_EX64(iccsr, EV4_ICCSR, PC0));
    ret = deposit64(ret, 2, 1, IPR_EX64(iccsr, EV4_ICCSR, PC1));
    ret = deposit64(ret, 9, 3, IPR_EX64(iccsr, EV4_ICCSR, MUX0));
    ret = deposit64(ret, 13, 3, IPR_EX64(iccsr, EV4_ICCSR, MUX1));
    ret = deposit64(ret, 16, 1, IPR_EX64(iccsr, EV4_ICCSR, PIPE));
    ret = deposit64(ret, 17, 1, IPR_EX64(iccsr, EV4_ICCSR, BPE));
    ret = deposit64(ret, 18, 1, IPR_EX64(iccsr, EV4_ICCSR, JSE));
    ret = deposit64(ret, 19, 1, IPR_EX64(iccsr, EV4_ICCSR, BHE));
    ret = deposit64(ret, 20, 1, IPR_EX64(iccsr, EV4_ICCSR, DI));
    ret = deposit64(ret, 21, 1, IPR_EX64(iccsr, EV4_ICCSR, HWE));
    ret = deposit64(ret, 22, 1, IPR_EX64(iccsr, EV4_ICCSR, MAP));
    ret = deposit64(ret, 23, 1, IPR_EX64(iccsr, EV4_ICCSR, FPE));
    ret = deposit64(ret, 28, 6, IPR_EX64(iccsr, EV4_ICCSR, ASN));
    ret = deposit64(ret, 44, 1, IPR_EX64(iccsr, EV4_ICCSR, PME0));
    ret = deposit64(ret, 45, 1, IPR_EX64(iccsr, EV4_ICCSR, PME1));
    return ret;
}

/* TB invalidation does not clear the EV4 PTE readback holding registers. */
static void ev4_itbzap_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    alpha_tlb_flush_all(env, AlphaTLBType_Instruction);
}

static void ev4_itbasm_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    alpha_tlb_flush_process(env, AlphaTLBType_Instruction);
}

static void ev4_itbis_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                         uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Instruction;

    alpha_tlb_flush_single(env, type, value & R_EV4_ITBIS_VA_MASK,
                           alpha_cur_asn(env, type, 0));
}

static void ev4_ps_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                         uint64_t value)
{
    /* We may be performing a mode switch, so flush the TLBs. */
    MERGE_FIELD(env->ipr.ps, value, R_EV4_PS_CM_MASK);
    tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    alpha_rebuild_hflags(env);
}

static uint64_t ev4_irr_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    uint64_t ret = 0;
    uint32_t eir = alpha_get_eir(env) & alpha_get_eien(env);
    uint32_t sir = alpha_get_sir(env) & alpha_get_sien(env);
    int ast = alpha_pending_ast(env);
    bool pc0 = env->irq_line_state & BIT(ALPHA_CPU_INPUT_PC0);
    bool pc1 = env->irq_line_state & BIT(ALPHA_CPU_INPUT_PC1);

    SET_FIELD(ret, EV4_HIRR, HWR, (eir != 0));
    SET_FIELD(ret, EV4_HIRR, SWR, (sir != 0));
    SET_FIELD(ret, EV4_HIRR, ATR, alpha_cpu_astirq_pending(env));
    SET_FIELD(ret, EV4_HIRR, CRR, alpha_get_crr(env) & alpha_get_cren(env));
    SET_FIELD(ret, EV4_HIRR, IRQ3, extract64(eir, 3, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ4, extract64(eir, 4, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ5, extract64(eir, 5, 1));
    SET_FIELD(ret, EV4_HIRR, PC1, pc1);
    SET_FIELD(ret, EV4_HIRR, PC0, pc0);
    SET_FIELD(ret, EV4_HIRR, IRQ0, extract64(eir, 0, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ1, extract64(eir, 1, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ2, extract64(eir, 2, 1));
    SET_FIELD(ret, EV4_HIRR, SLR, alpha_get_slr(env) & alpha_get_slen(env));
    SET_FIELD(ret, EV4_HIRR, SIR, sir);
    SET_FIELD(ret, EV4_HIRR, ASTK, (ast & 0b0001) != 0);
    SET_FIELD(ret, EV4_HIRR, ASTE, (ast & 0b0010) != 0);
    SET_FIELD(ret, EV4_HIRR, ASTS, (ast & 0b0100) != 0);
    SET_FIELD(ret, EV4_HIRR, ASTU, (ast & 0b1000) != 0);
    return ret;
}

static uint64_t ev4_ier_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    uint64_t ret = 0;
    uint32_t aster = IPR_EX64(aster, EV4_ASTER, ASTER);
    uint32_t sier = IPR_EX64(sier, EV4_SIER, SIER);
    uint32_t hier = IPR_EX64(hier, EV4_HIER, HIER);

    SET_FIELD(ret, EV4_HIRR, CRR, IPR_EX64(hier, EV4_HIER, CRE));
    SET_FIELD(ret, EV4_HIRR, IRQ3, extract32(hier, 3, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ4, extract32(hier, 4, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ5, extract32(hier, 5, 1));
    SET_FIELD(ret, EV4_HIRR, PC1, IPR_EX64(hier, EV4_HIER, PC1));
    SET_FIELD(ret, EV4_HIRR, PC0, IPR_EX64(hier, EV4_HIER, PC0));
    SET_FIELD(ret, EV4_HIRR, IRQ0, extract32(hier, 0, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ1, extract32(hier, 1, 1));
    SET_FIELD(ret, EV4_HIRR, IRQ2, extract32(hier, 2, 1));
    SET_FIELD(ret, EV4_HIRR, SLR, IPR_EX64(hier, EV4_HIER, SLE));
    SET_FIELD(ret, EV4_HIRR, SIR, sier);
    SET_FIELD(ret, EV4_HIRR, ASTK, (aster & 0b0001) != 0);
    SET_FIELD(ret, EV4_HIRR, ASTE, (aster & 0b0010) != 0);
    SET_FIELD(ret, EV4_HIRR, ASTS, (aster & 0b0100) != 0);
    SET_FIELD(ret, EV4_HIRR, ASTU, (aster & 0b1000) != 0);
    return ret;
}

static void ev4_sirr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.sirr = value & R_EV4_SIRR_SIR_MASK;
}

static void ev4_aster_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.aster = value & R_EV4_ASTER_ASTER_MASK;
}

static void ev4_hier_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    uint64_t wmask = R_EV4_HIER_CRE_MASK | R_EV4_HIER_PC0_MASK |
                     R_EV4_HIER_HIER_MASK | R_EV4_HIER_PC1_MASK |
                     R_EV4_HIER_SLE_MASK;

    MERGE_FIELD(env->ipr.hier, value, wmask);
}

static void ev4_sier_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.sier = value & R_EV4_SIER_SIER_MASK;
}

static void ev4_astrr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.astrr = value & R_EV4_ASTER_ASTER_MASK;
}

static const AlphaIPRegInfo ev4_ibox_ip_reginfo[] = {
    { .name = "STALL", .ipr = EV4_IPR_STALL,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "TB_TAG", .ipr = EV4_IPR_TB_TAG,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.tb_tag),
      .writefn = ev4_tb_tag_write, .raw_writefn = raw_write },
    { .name = "ITB_PTE", .ipr = EV4_IPR_ITB_PTE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_pte),
      .writefn = ev4_itb_pte_write, .raw_writefn = raw_write,
      .readfn = ev4_itb_pte_read, .raw_readfn = raw_read },
    { .name = "ICCSR", .ipr = EV4_IPR_ICCSR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.iccsr),
      .writefn = ev4_iccsr_write, .raw_writefn = raw_write,
      .readfn = ev4_iccsr_read, .raw_readfn = raw_read },
    { .name = "ITB_PTE_TEMP", .ipr = EV4_IPR_ITB_PTE_TEMP,
      .access = IPR_R,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_pte_temp) },
    { .name = "EXC_ADDR", .ipr = EV4_IPR_EXC_ADDR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_addr) },
    /* FIXME: Serial interrupt implementation */
    { .name = "SL_RCV", .ipr = EV4_IPR_SL_RCV,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "ITBZAP", .ipr = EV4_IPR_ITBZAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev4_itbzap_write },
    { .name = "ITBASM", .ipr = EV4_IPR_ITBASM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev4_itbasm_write },
    { .name = "ITBIS", .ipr = EV4_IPR_ITBIS,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev4_itbis_write },
    { .name = "PS", .ipr = EV4_IPR_PS,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.ps),
      .writefn = ev4_ps_write, .raw_writefn = raw_write },
    { .name = "EXC_SUM", .ipr = EV4_IPR_EXC_SUM,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_sum) },
    { .name = "PAL_BASE", .ipr = EV4_IPR_PAL_BASE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pal_base),
      .writefn = pal_base_write, .raw_writefn = raw_write },
    { .name = "HIRR", .ipr = EV4_IPR_HIRR,
      .type = ALPHA_IP_NO_RAW, .access = IPR_RW,
      /* Read-only hardware state; Jensen's NT bootstrap writes zero here. */
      .readfn = ev4_irr_read, .writefn = alpha_ip_write_ignore },
    { .name = "SIRR", .ipr = EV4_IPR_SIRR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.sirr),
      .writefn = ev4_sirr_write, .raw_writefn = raw_write,
      .readfn = ev4_irr_read, .raw_readfn = raw_read },
    { .name = "ASTRR", .ipr = EV4_IPR_ASTRR,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.aster),
      .writefn = ev4_astrr_write, .raw_writefn = raw_write,
      .readfn = ev4_irr_read, .raw_readfn = raw_read },
    { .name = "HIER", .ipr = EV4_IPR_HIER,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.hier),
      .writefn = ev4_hier_write, .raw_writefn = raw_write,
      .readfn = ev4_ier_read, .raw_readfn = raw_read },
    { .name = "SIER", .ipr = EV4_IPR_SIER,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.sier),
      .writefn = ev4_sier_write, .raw_writefn = raw_write,
      .readfn = ev4_ier_read, .raw_readfn = raw_read },
    { .name = "ASTER", .ipr = EV4_IPR_ASTER,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.aster),
      .writefn = ev4_aster_write, .raw_writefn = raw_write,
      .readfn = ev4_ier_read, .raw_readfn = raw_read },
    /* FIXME: Serial interrupt implementation */
    { .name = "SL_CLR", .ipr = EV4_IPR_SL_CLR,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "SL_XMIT", .ipr = EV4_IPR_SL_XMIT,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "ISSUE_CHK", .ipr = EV4_IPR_ISSUE_CHK,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0},
    { .name = "SINGLE_ISSUE", .ipr = EV4_IPR_SINGLE_ISSUE,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "DUAL_ISSUE", .ipr = EV4_IPR_DUAL_ISSUE,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
};

static void ev4_tb_ctl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    env->ipr.tb_ctl = value & R_EV4_TB_CTL_GH_MASK;
}

static void ev4_dtb_pte_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Data;
    int asn = alpha_cur_asn(env, type, 0);
    uint64_t mask = R_EV4_DTB_PTE_FOR_MASK | R_EV4_DTB_PTE_FOW_MASK |
                    R_EV4_DTB_PTE_ASM_MASK |
                    R_EV4_DTB_PTE_KRE_MASK | R_EV4_DTB_PTE_ERE_MASK |
                    R_EV4_DTB_PTE_SRE_MASK | R_EV4_DTB_PTE_URE_MASK |
                    R_EV4_DTB_PTE_KWE_MASK | R_EV4_DTB_PTE_EWE_MASK |
                    R_EV4_DTB_PTE_SWE_MASK | R_EV4_DTB_PTE_UWE_MASK |
                    R_EV4_DTB_PTE_PFN_MASK;

    value &= mask;
    value |= env->ipr.tb_ctl;
    alpha_tlb_fill(env, type, env->ipr.tb_tag, value, asn);
    env->ipr.dtb_pte = value;
    env->ipr.dtb_pte_temp = ev4_xtb_pte_temp_init(value, false);
}

static void ev4_dtbzap_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    alpha_tlb_flush_all(env, AlphaTLBType_Data);
}

static void ev4_dtbasm_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    alpha_tlb_flush_process(env, AlphaTLBType_Data);
}

static void ev4_dtbis_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Data;

    alpha_tlb_flush_single(env, type, value & R_EV4_DTBIS_VA_MASK,
                           alpha_cur_asn(env, type, 0));
}

static void ev4_aboxctl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    uint64_t wmask = R_EV4_ABOX_CTL_WB_DIS_MASK |
                     R_EV4_ABOX_CTL_MCHK_EN_MASK |
                     R_EV4_ABOX_CTL_CRD_EN_MASK |
                     R_EV4_ABOX_CTL_IC_SBUF_EN_MASK |
                     R_EV4_ABOX_CTL_SPE_MASK |
                     R_EV4_ABOX_CTL_EMD_EN_MASK |
                     R_EV4_ABOX_CTL_DC_ENA_MASK |
                     R_EV4_ABOX_CTL_DC_FHIT_MASK |
                     R_EV4_ABOX_CTL_DC_16K_MASK |
                     R_EV4_ABOX_CTL_F_TAG_ERR_MASK |
                     R_EV4_ABOX_CTL_NOCHK_PAR_MASK;

    MERGE_FIELD(env->ipr.abox_ctl, value, wmask);
}

static void ev4_altmode_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    CPUState *cs = env_cpu(env);

    env->ipr.altmode = value & R_EV4_ALT_MODE_AM_MASK;
    tlb_flush_by_mmuidx(cs, altmode_tlbmask());
}

static uint64_t ev4_lock_flag_read(CPUAlphaState *env,
                                   const AlphaIPRegInfo *ri)
{
    return env->llsc_addr != LLSC_ADDR_NONE;
}

static const AlphaIPRegInfo ev4_abox_ip_reginfo[] = {
    { .name = "TB_CTL", .ipr = EV4_IPR_TB_CTL,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.tb_ctl),
      .writefn = ev4_tb_ctl_write, .raw_writefn = raw_write },
    { .name = "DTB_PTE", .ipr = EV4_IPR_DTB_PTE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_pte),
      .writefn = ev4_dtb_pte_write, .raw_writefn = raw_write },
    { .name = "DTB_PTE_TEMP", .ipr = EV4_IPR_DTB_PTE_TEMP,
      .access = IPR_R,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_pte_temp) },
    { .name = "MMCSR", .ipr = EV4_IPR_MMCSR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.mmcsr),
      .writefn = alpha_ip_write_ignore, .raw_writefn = raw_write },
    { .name = "VA", .ipr = EV4_IPR_VA,
      .access = IPR_RW, .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.va) },
    { .name = "DTBZAP", .ipr = EV4_IPR_DTBZAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev4_dtbzap_write },
    { .name = "DTBASM", .ipr = EV4_IPR_DTBASM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev4_dtbasm_write },
    { .name = "DTBIS", .ipr = EV4_IPR_DTBIS,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev4_dtbis_write },
    { .name = "BIU_ADDR", .ipr = EV4_IPR_BIU_ADDR,
      .access = IPR_R,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.biu_addr) },
    { .name = "BIU_STAT", .ipr = EV4_IPR_BIU_STAT,
      .access = IPR_R,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.biu_stat) },
    { .name = "DC_ADDR", .ipr = EV4_IPR_DC_ADDR,
      .type = ALPHA_IP_CONST, .access = IPR_R,
      .resetvalue = 0 },
    { .name = "DC_STAT", .ipr = EV4_IPR_DC_STAT,
      .type = ALPHA_IP_CONST, .access = IPR_R,
      .resetvalue = 0b111 },
    { .name = "FILL_ADDR", .ipr = EV4_IPR_FILL_ADDR,
      .type = ALPHA_IP_CONST, .access = IPR_R,
      .resetvalue = 0 },
    { .name = "ABOX_CTL", .ipr = EV4_IPR_ABOX_CTL,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.abox_ctl),
      .writefn = ev4_aboxctl_write, .raw_writefn = raw_write },
    { .name = "ALT_MODE", .ipr = EV4_IPR_ALT_MODE,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_altmode),
      .writefn = ev4_altmode_write, .raw_writefn = raw_write },
    { .name = "CC", .ipr = EV4_IPR_CC,
      .type = ALPHA_IP_NO_RAW | ALPHA_IP_IO, .access = IPR_RW,
      .readfn = cc_read, .writefn = cc_write },
    { .name = "CC_CTL", .ipr = EV4_IPR_CC_CTL,
      .type = ALPHA_IP_IO, .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.cc_ctl),
      .writefn = cc_ctl_write, .raw_writefn = raw_write },
    { .name = "BIU_CTL", .ipr = EV4_IPR_BIU_CTL,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "FILL_SYNDROME", .ipr = EV4_IPR_FILL_SYNDROME,
      .type = ALPHA_IP_CONST, .access = IPR_R,
      .resetvalue = 0 },
    { .name = "BC_TAG", .ipr = EV4_IPR_BC_TAG,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "FLUSH_IC", .ipr = EV4_IPR_FLUSH_IC,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "LOCK_FLAG", .ipr = EV4_IPR_LOCK_FLAG,
      .type = ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev4_lock_flag_read },
    { .name = "FLUSH_IC_ASM", .ipr = EV4_IPR_FLUSH_IC_ASM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "INTR_FLAG", .ipr = EV4_IPR_INTR_FLAG,
      .type = ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev4_lock_flag_read },
};

static void ev4_redirect_write_ipreg(CPUAlphaState *env,
                                     const AlphaIPRegInfo *ri, uint64_t value)
{
    if (ri->type & ALPHA_IP_CONST) {
        return;
    } else if (ri->writefn) {
        (*ri->writefn)(env, ri, value);
    } else if (ri->raw_writefn) {
        (*ri->raw_writefn)(env, ri, value);
    } else {
        raw_write(env, ri, value);
    }
}

static void ev4_redirect_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                               uint64_t value)
{
    AlphaCPU *cpu = env_archcpu(env);
    int index = extract32(ri->ipr, 0, 5);
    const AlphaIPRegInfo *sub;

    /*
     * It is possible for an "hw_mtpr" instruction to write multiple IPRs
     * in parallel if both have the same index.
     */

    /* Ibox */
    if (ri->ipr & BIT(5)) {
        sub = get_alpha_ipreg_info(cpu->ipregs, index | BIT(5));
        if (sub) {
            ev4_redirect_write_ipreg(env, sub, value);
        }
    }

    /* Abox */
    if (ri->ipr & BIT(6)) {
        sub = get_alpha_ipreg_info(cpu->ipregs, index | BIT(6));
        if (sub) {
            ev4_redirect_write_ipreg(env, sub, value);
        }
    }

    /* PALtemp */
    if (ri->ipr & BIT(7)) {
        sub = get_alpha_ipreg_info(cpu->ipregs, index | BIT(7));
        if (sub) {
            ev4_redirect_write_ipreg(env, sub, value);
        }
    }
}

static void register_ev4_ipregs(AlphaCPU *cpu)
{
    int i;

    define_alpha_ipregs(cpu, ev4_ibox_ip_reginfo);
    define_alpha_ipregs(cpu, ev4_abox_ip_reginfo);

    for (i = 0; i < 32; i++) {
        char *temp_name = g_strdup_printf("PALTEMP%d", i);
        AlphaIPRegInfo temp_reg[] = {
            { .name = temp_name, .ipr = EV4_IPR_PAL_R0 + i,
              .access = IPR_RW,
              .fieldoffset = offsetof(CPUAlphaState, ipr.temp[i]) },
        };
        define_one_alpha_ipreg(cpu, temp_reg);
        g_free(temp_name);
    }

    for (i = 0; i < 32; i++) {
        char *redirect0_name = g_strdup_printf("REDIRECT_ALIAS_AI%d", i);
        char *redirect1_name = g_strdup_printf("REDIRECT_ALIAS_PI%d", i);
        char *redirect2_name = g_strdup_printf("REDIRECT_ALIAS_PAI%d", i);
        AlphaIPRegInfo redirect_regs[] = {
            { .name = redirect0_name, .ipr = 0b01100000 + i,
              .type = ALPHA_IP_NO_RAW | ALPHA_IP_ALIAS, .access = IPR_W,
              .writefn = ev4_redirect_write },
            { .name = redirect1_name, .ipr = 0b10100000 + i,
              .type = ALPHA_IP_NO_RAW | ALPHA_IP_ALIAS, .access = IPR_W,
              .writefn = ev4_redirect_write },
            { .name = redirect2_name, .ipr = 0b11100000 + i,
              .type = ALPHA_IP_NO_RAW | ALPHA_IP_ALIAS, .access = IPR_W,
              .writefn = ev4_redirect_write },
        };
        define_alpha_ipregs(cpu, redirect_regs);
        g_free(redirect0_name);
        g_free(redirect1_name);
        g_free(redirect2_name);
    }
}

/* These assumptions should not be violated. */
#pragma GCC diagnostic ignored "-Wenum-compare"
QEMU_BUILD_BUG_ON(R_EV5_IFAULT_VA_FORM_VA_MASK != R_EV5_VA_FORM_VA_MASK);
QEMU_BUILD_BUG_ON(R_EV5_IFAULT_VA_FORM_VPTB_MASK != R_EV5_VA_FORM_VPTB_MASK);
QEMU_BUILD_BUG_ON(R_EV5_IFAULT_VA_FORM_NT_VA_MASK != R_EV5_VA_FORM_NT_VA_MASK);
QEMU_BUILD_BUG_ON(R_EV5_IFAULT_VA_FORM_NT_VPTB_MASK !=
                  R_EV5_VA_FORM_NT_VPTB_MASK);
#pragma GCC diagnostic pop

static uint64_t ev5_va_form(CPUAlphaState *env, bool ifetch)
{
    uint64_t vaddr = alpha_cur_fault_addr(env, ifetch);
    uint64_t vptb = alpha_cur_vptb(env, ifetch);
    uint64_t ret = 0;
    int va32 = alpha_va32_enabled(env, ifetch);

    switch (va32) {
    case 0b01: /* NT_MODE = 1 */
        vaddr = extract64(vaddr, 0, 32) & TARGET_PAGE_MASK;
        ret = vptb & R_EV5_VA_FORM_NT_VPTB_MASK;
        ret |= ((vaddr >> 13) << 3) & R_EV5_VA_FORM_NT_VA_MASK;
        break;
    case 0b00: /* NT_MODE = 0 */
        vaddr = extract64(vaddr, 0, 43) & TARGET_PAGE_MASK;
        ret = vptb & R_EV5_VA_FORM_VPTB_MASK;
        ret |= ((vaddr >> 13) << 3) & R_EV5_VA_FORM_VA_MASK;
        break;
    default:
        g_assert_not_reached();
    }
    return ret;
}

static uint64_t ev5_va_form_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return ev5_va_form(env, false);
}

static void ev5_itb_asn_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    int asn = FIELD_EX64(value, EV5_ITB_ASN, ASN);

    /* Only flush TLBs if we're switching ASIDs. */
    if (alpha_itb_asn(env) != asn) {
        tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    }
    raw_write(env, ri, value & R_EV5_ITB_ASN_ASN_MASK);
}

static void ev5_itb_tag_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    raw_write(env, ri, value & R_EV5_ITB_TAG_VA_MASK);
}

static inline uint64_t ev5_itb_pte_temp_init(CPUAlphaState *env)
{
    uint64_t ret = 0;
    switch (FIELD_EX64(env->ipr.itb_pte, PTE, GH)) {
    case 0b11:
        SET_FIELD(ret, EV5_ITB_PTE_TEMP, GH_2, 1);
        QEMU_FALLTHROUGH;
    case 0b10:
        SET_FIELD(ret, EV5_ITB_PTE_TEMP, GH_1, 1);
        QEMU_FALLTHROUGH;
    case 0b01:
        SET_FIELD(ret, EV5_ITB_PTE_TEMP, GH_0, 1);
        QEMU_FALLTHROUGH;
    default:
        break;
    }
    return ret;
}

static void ev5_itb_pte_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Instruction;
    int asn = alpha_cur_asn(env, type, 0);
    uint64_t mask = R_EV5_ITB_PTE_ASM_MASK | R_EV5_ITB_PTE_GH_MASK |
                    R_EV5_ITB_PTE_KRE_MASK | R_EV5_ITB_PTE_ERE_MASK |
                    R_EV5_ITB_PTE_SRE_MASK | R_EV5_ITB_PTE_URE_MASK |
                    R_EV5_ITB_PTE_PFN_MASK;

    value &= mask;
    alpha_tlb_fill(env, type, env->ipr.itb_tag, value, asn);
    env->ipr.itb_pte = value;
    env->ipr.itb_pte_temp = ev5_itb_pte_temp_init(env);
}

static void ev5_itb_iap_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    env->ipr.itb_pte = 0;
    env->ipr.itb_pte_temp = 0;
    alpha_tlb_flush_process(env, AlphaTLBType_Instruction);
}

static void ev5_itb_ia_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    env->ipr.itb_pte = 0;
    env->ipr.itb_pte_temp = 0;
    alpha_tlb_flush_all(env, AlphaTLBType_Instruction);
}

static void ev5_itb_is_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                         uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Instruction;

    env->ipr.itb_pte = 0;
    env->ipr.itb_pte_temp = 0;
    alpha_tlb_flush_single(env, type, value & R_EV5_ITB_IS_VA_MASK,
                           alpha_cur_asn(env, type, 0));
}

static uint64_t ev5_iva_form_read_uncached(CPUAlphaState *env)
{
    return ev5_va_form(env, true);
}

static uint64_t ev5_iva_form_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return ev5_iva_form_read_uncached(env);
}

static void ev5_ivptbr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    env->ipr.ivptbr = value & R_EV5_IVPTBR_VPTB_MASK;
}

static void ev5_icm_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                          uint64_t value)
{
    /* We may be performing a mode switch, so flush the TLBs. */
    MERGE_FIELD(env->ipr.icm, value, R_EV5_ICM_CM_MASK);
    tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    alpha_rebuild_hflags(env);
}

static void ev5_icsr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
    uint64_t wmask = R_EV5_ICSR_PMA_MASK | R_EV5_ICSR_PMP_MASK |
                     R_EV5_ICSR_BYT_MASK | R_EV5_ICSR_FMP_MASK |
                     R_EV5_ICSR_MVE_MASK | R_EV5_ICSR_IMSK_MASK |
                     R_EV5_ICSR_TMM_MASK | R_EV5_ICSR_TMD_MASK |
                     R_EV5_ICSR_FPE_MASK | R_EV5_ICSR_HWE_MASK |
                     R_EV5_ICSR_SPE_MASK | R_EV5_ICSR_SDE_MASK |
                     R_EV5_ICSR_CRDE_MASK | R_EV5_ICSR_SLE_MASK |
                     R_EV5_ICSR_FMS_MASK | R_EV5_ICSR_FBT_MASK |
                     R_EV5_ICSR_FBD_MASK | R_EV5_ICSR_DBS_MASK |
                     R_EV5_ICSR_TST_MASK;

    MERGE_FIELD(env->ipr.icsr, value, wmask);
    alpha_rebuild_hflags(env);
}

static void ev5_ipl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                          uint64_t value)
{
    env->ipr.ipl = value & R_EV5_IPL_IPL_MASK;
}

static uint64_t ev5_intid_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    uint64_t sirr = FIELD_EX64(env->ipr.sirr, EV5_SIRR, SIR);
    uint32_t req = 0;
    int i;

    if (env->irq_line_state & BIT(ALPHA_CPU_INPUT_MCHK) ||
        env->irq_line_state & BIT(ALPHA_CPU_INPUT_CRR)) {
        req = IPL_HIGH;
    }
    if (env->irq_line_state & BIT(ALPHA_CPU_INPUT_PWRFAIL)) {
        req = IPL_PWRFAIL;
    }
    if (env->irq_line_state & BIT(ALPHA_CPU_INPUT_IRQ3)) {
        req = IPL_EIRQ_MIN + 3;
    }
    if (env->irq_line_state & BIT(ALPHA_CPU_INPUT_IRQ2)) {
        req = IPL_EIRQ_MIN + 2;
    }
    if (env->irq_line_state & BIT(ALPHA_CPU_INPUT_IRQ1)) {
        req = IPL_EIRQ_MIN + 1;
    }
    if (env->irq_line_state & BIT(ALPHA_CPU_INPUT_IRQ0)) {
        req = IPL_EIRQ_MIN + 0;
    }
    if (sirr) {
        for (i = IPL_SIRQ_MAX; i > 0; i--) {
            if ((sirr >> (i - 1)) & 1) {
                req = i;
                break;
            }
        }
    }
    if ((req < IPL_AST) && alpha_cpu_astirq_pending(env)) {
        req = IPL_AST;
    }
    return req;
}

static void ev5_aster_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.aster = value & R_EV5_ASTER_ASTER_MASK;
}

static void ev5_astrr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.astrr = value & R_EV5_ASTER_ASTER_MASK;
}

static void ev5_sirr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    env->ipr.sirr = value & R_EV5_SIRR_SIR_MASK;
}

static void ev5_hw_int_clr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                                 uint64_t value)
{
    uint32_t cur_irq = env->irq_line_state;

    if (FIELD_EX64(value, EV5_HW_INT_CLR, CRDC)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_CRR, 1, 0);
    }
    if (FIELD_EX64(value, EV5_HW_INT_CLR, SLC)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_SLR, 1, 0);
    }
    if (FIELD_EX64(value, EV5_HW_INT_CLR, PC0C)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_PC0, 1, 0);
    }
    if (FIELD_EX64(value, EV5_HW_INT_CLR, PC1C)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_PC1, 1, 0);
    }
    if (FIELD_EX64(value, EV5_HW_INT_CLR, PC2C)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_PC2, 1, 0);
    }
    env->irq_line_state = cur_irq;
}

static uint64_t ev5_isr_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    uint64_t ret = 0;
    uint32_t eir = alpha_get_eir(env) & alpha_get_eien(env);
    int ast = alpha_pending_ast(env);
    bool powerfail = env->irq_line_state & BIT(ALPHA_CPU_INPUT_PWRFAIL);
    bool mchk = env->irq_line_state & BIT(ALPHA_CPU_INPUT_MCHK);
    bool halt = env->irq_line_state & BIT(ALPHA_CPU_INPUT_HLT);

    SET_FIELD(ret, EV5_ISR, HLT, halt);
    SET_FIELD(ret, EV5_ISR, CRD, alpha_get_crr(env) & alpha_get_cren(env));
    SET_FIELD(ret, EV5_ISR, SLI, alpha_get_slr(env) & alpha_get_slen(env));
    SET_FIELD(ret, EV5_ISR, MCK, mchk);
    SET_FIELD(ret, EV5_ISR, PFL, powerfail);
    SET_FIELD(ret, EV5_ISR, PC, alpha_get_pcr(env) & alpha_get_pcen(env));
    SET_FIELD(ret, EV5_ISR, IRQ, (eir & 0b1111));
    SET_FIELD(ret, EV5_ISR, ATR, (ast != 0));
    SET_FIELD(ret, EV5_ISR, SISR, alpha_get_sir(env) & alpha_get_sien(env));
    SET_FIELD(ret, EV5_ISR, ASTK, (ast & 0b0001) != 0);
    SET_FIELD(ret, EV5_ISR, ASTE, (ast & 0b0010) != 0);
    SET_FIELD(ret, EV5_ISR, ASTS, (ast & 0b0100) != 0);
    SET_FIELD(ret, EV5_ISR, ASTU, (ast & 0b1000) != 0);
    return ret;
}

static void ev5_pmctr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                       uint64_t value)
{
    uint64_t wmask = R_EV5_PMCTR_SEL2_MASK | R_EV5_PMCTR_SEL1_MASK |
                     R_EV5_PMCTR_KILLK_MASK | R_EV5_PMCTR_KILLP_MASK |
                     R_EV5_PMCTR_CTL2_MASK | R_EV5_PMCTR_CTL1_MASK |
                     R_EV5_PMCTR_CTL0_MASK | R_EV5_PMCTR_CTR2_MASK |
                     R_EV5_PMCTR_KILLU_MASK | R_EV5_PMCTR_SEL0_MASK |
                     R_EV5_PMCTR_CTR1_MASK | R_EV5_PMCTR_CTR0_MASK;

    MERGE_FIELD(env->ipr.pctr_ctl, value, wmask);
}

static const AlphaIPRegInfo ev5_ibox_ip_reginfo[] = {
    { .name = "ITB_TAG", .ipr = EV5_IPR_ITB_TAG,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_tag),
      .writefn = ev5_itb_tag_write, .raw_writefn = raw_write },
    { .name = "ITB_PTE", .ipr = EV5_IPR_ITB_PTE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_pte),
      .writefn = ev5_itb_pte_write, .raw_writefn = raw_write },
    { .name = "ITB_PTE_TEMP", .ipr = EV5_IPR_ITB_PTE_TEMP,
      .access = IPR_R,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_pte_temp) },
    { .name = "ITB_ASN", .ipr = EV5_IPR_ITB_ASN,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_asn),
      .writefn = ev5_itb_asn_write, .raw_writefn = raw_write },
    { .name = "ITB_IAP", .ipr = EV5_IPR_ITB_IAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_itb_iap_write },
    { .name = "ITB_IA", .ipr = EV5_IPR_ITB_IA,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_itb_ia_write },
    { .name = "ITB_IS", .ipr = EV5_IPR_ITB_IS,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_itb_is_write },
    { .name = "IFAULT_VA_FORM", .ipr = EV5_IPR_IFAULT_VA_FORM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev5_iva_form_read },
    { .name = "IVPTBR", .ipr = EV5_IPR_IVPTBR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.ivptbr),
      .writefn = ev5_ivptbr_write, .raw_writefn = raw_write },
    { .name = "IC_PERR", .ipr = EV5_IPR_IC_PERR,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "IC_FLUSH", .ipr = EV5_IPR_IC_FLUSH,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "EXC_ADDR", .ipr = EV5_IPR_EXC_ADDR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_addr) },
    { .name = "EXC_SUM", .ipr = EV5_IPR_EXC_SUM,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_sum) },
    { .name = "EXC_MASK", .ipr = EV5_IPR_EXC_MASK,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_mask) },
    { .name = "PAL_BASE", .ipr = EV5_IPR_PAL_BASE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pal_base),
      .writefn = pal_base_write, .raw_writefn = raw_write },
    { .name = "ICM", .ipr = EV5_IPR_ICM,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.icm),
      .writefn = ev5_icm_write, .raw_writefn = raw_write },
    { .name = "ICSR", .ipr = EV5_IPR_ICSR,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.icsr),
      .writefn = ev5_icsr_write, .raw_writefn = raw_write },
    { .name = "IPL", .ipr = EV5_IPR_IPL,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.ipl),
      .writefn = ev5_ipl_write, .raw_writefn = raw_write },
    { .name = "INTID", .ipr = EV5_IPR_INTID,
      .type = ALPHA_IP_IO | ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev5_intid_read },
    { .name = "ASTER", .ipr = EV5_IPR_ASTER,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.aster),
      .writefn = ev5_aster_write, .raw_writefn = raw_write },
    { .name = "ASTRR", .ipr = EV5_IPR_ASTRR,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.aster),
      .writefn = ev5_astrr_write, .raw_writefn = raw_write },
    { .name = "SIRR", .ipr = EV5_IPR_SIRR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.sirr),
      .writefn = ev5_sirr_write, .raw_writefn = raw_write },
    { .name = "HW_INT_CLR", .ipr = EV5_IPR_HW_INT_CLR,
      .type = ALPHA_IP_IO | ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_hw_int_clr_write },
    { .name = "ISR", .ipr = EV5_IPR_ISR,
      .type = ALPHA_IP_IO | ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev5_isr_read },
    /* FIXME: Serial interrupt implementation */
    { .name = "SL_RCV", .ipr = EV5_IPR_SL_RCV,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "SL_XMIT", .ipr = EV5_IPR_SL_XMIT,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "PMCTR", .ipr = EV5_IPR_PMCTR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pmctr),
      .writefn = ev5_pmctr_write, .raw_writefn = raw_write },
};

static void ev5_dtb_asn_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    int asn = FIELD_EX64(value, EV5_DTB_ASN, ASN);

    /* Only flush TLBs if we're switching ASIDs. */
    if (alpha_itb_asn(env) != asn) {
        tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    }
    raw_write(env, ri, value & R_EV5_DTB_ASN_ASN_MASK);
}

static void ev5_dtb_cm_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    /* We may be performing a mode switch, so flush the TLBs. */
    MERGE_FIELD(env->ipr.dtb_cm, value, R_EV5_DTB_CM_CM_MASK);
    tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    alpha_rebuild_hflags(env);
}

static void ev5_dtb_tag_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                          uint64_t value)
{
    raw_write(env, ri, value & R_EV5_DTB_TAG_VA_MASK);
}

static void ev5_dtb_pte_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Data;
    int asn = alpha_cur_asn(env, type, 0);
    uint64_t mask = R_EV5_DTB_PTE_FOR_MASK | R_EV5_DTB_PTE_FOW_MASK |
                    R_EV5_DTB_PTE_ASM_MASK | R_EV5_DTB_PTE_GH_MASK |
                    R_EV5_DTB_PTE_KRE_MASK | R_EV5_DTB_PTE_ERE_MASK |
                    R_EV5_DTB_PTE_SRE_MASK | R_EV5_DTB_PTE_URE_MASK |
                    R_EV5_DTB_PTE_KWE_MASK | R_EV5_DTB_PTE_EWE_MASK |
                    R_EV5_DTB_PTE_SWE_MASK | R_EV5_DTB_PTE_UWE_MASK |
                    R_EV5_DTB_PTE_PFN_MASK;

    value &= mask;
    alpha_tlb_fill(env, type, env->ipr.dtb_tag[0], value, asn);
    env->ipr.dtb_pte = value;
    env->ipr.dtb_pte_temp = env->ipr.dtb_pte;
}

static void ev5_mvptbr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    env->ipr.mvptbr = value & R_EV5_MVPTBR_VPTB_MASK;
}

static void ev5_dtb_iap_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    env->ipr.dtb_pte = 0;
    env->ipr.dtb_pte_temp = 0;
    alpha_tlb_flush_process(env, AlphaTLBType_Data);
}

static void ev5_dtb_ia_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    env->ipr.dtb_pte = 0;
    env->ipr.dtb_pte_temp = 0;
    alpha_tlb_flush_all(env, AlphaTLBType_Data);
}

static void ev5_dtb_is_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Data;

    env->ipr.dtb_pte = 0;
    env->ipr.dtb_pte_temp = 0;
    alpha_tlb_flush_single(env, type, value & R_EV6_DTB_IS_VA_MASK,
                           alpha_cur_asn(env, type, 0));
}

static void ev5_altmode_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    CPUState *cs = env_cpu(env);

    env->ipr.altmode = value & R_EV5_ALT_MODE_AM_MASK;
    tlb_flush_by_mmuidx(cs, altmode_tlbmask());
}

static void ev5_mcsr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                                  uint64_t value)
{
    uint64_t wmask = R_EV5_MCSR_M_BIG_ENDIAN_MASK | R_EV5_MCSR_SPE_MASK |
                     R_EV5_MCSR_E_BIG_ENDIAN_MASK;

    MERGE_FIELD(env->ipr.mcsr, value, wmask);
}

static void ev5_dcmode_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    uint64_t mask = R_EV5_DC_MODE_DC_ENA_MASK |
                    R_EV5_DC_MODE_DC_FHIT_MASK |
                    R_EV5_DC_MODE_DC_BAD_PARITY_MASK |
                    R_EV5_DC_MODE_DC_PERR_DIS_MASK |
                    R_EV5_DC_MODE_DC_DOA_MASK;

    raw_write(env, ri, (value & mask) | R_EV5_DC_MODE_DC_ENA_MASK);
}

static void ev5_dcmode_reset(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    SET_FIELD(env->ipr.dc_ctl, EV5_DC_MODE, DC_ENA, 1);
}

static const AlphaIPRegInfo ev5_mbox_ip_reginfo[] = {
    { .name = "DTB_ASN", .ipr = EV5_IPR_DTB_ASN,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_asn),
      .writefn = ev5_dtb_asn_write, .raw_writefn = raw_write },
    { .name = "DTB_CM", .ipr = EV5_IPR_DTB_CM,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_cm),
      .writefn = ev5_dtb_cm_write, .raw_writefn = raw_write },
    { .name = "DTB_TAG", .ipr = EV5_IPR_DTB_TAG,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_tag[0]),
      .writefn = ev5_dtb_tag_write, .raw_writefn = raw_write },
    { .name = "DTB_PTE", .ipr = EV5_IPR_DTB_PTE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_pte),
      .writefn = ev5_dtb_pte_write, .raw_writefn = raw_write },
    { .name = "DTB_PTE_TEMP", .ipr = EV5_IPR_DTB_PTE_TEMP,
      .access = IPR_R,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_pte_temp) },
    { .name = "MM_STAT", .ipr = EV5_IPR_MM_STAT,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.mm_stat),
      .writefn = alpha_ip_write_ignore, .raw_writefn = raw_write },
    { .name = "VA", .ipr = EV5_IPR_VA,
      .access = IPR_RW, .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.va) },
    { .name = "VA_FORM", .ipr = EV5_IPR_VA_FORM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev5_va_form_read },
    { .name = "MVTPBR", .ipr = EV5_IPR_MVPTBR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.mvptbr),
      .writefn = ev5_mvptbr_write, .raw_writefn = raw_write },
    { .name = "DTB_IAP", .ipr = EV5_IPR_DTB_IAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_dtb_iap_write },
    { .name = "DTB_IA", .ipr = EV5_IPR_DTB_IA,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_dtb_ia_write },
    { .name = "DTB_IS", .ipr = EV5_IPR_DTB_IS,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev5_dtb_is_write },
    { .name = "ALT_MODE", .ipr = EV5_IPR_ALT_MODE,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.altmode),
      .writefn = ev5_altmode_write, .raw_writefn = raw_write },
    { .name = "CC", .ipr = EV5_IPR_CC,
      .type = ALPHA_IP_NO_RAW | ALPHA_IP_IO, .access = IPR_RW,
      .readfn = cc_read, .writefn = cc_write },
    { .name = "CC_CTL", .ipr = EV5_IPR_CC_CTL,
      .type = ALPHA_IP_IO, .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.cc_ctl),
      .writefn = cc_ctl_write, .raw_writefn = raw_write },
    { .name = "MCSR", .ipr = EV5_IPR_MCSR,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.mcsr),
      .writefn = ev5_mcsr_write, .raw_writefn = raw_write },
    { .name = "DC_FLUSH", .ipr = EV5_IPR_DC_FLUSH,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "DC_PERR", .ipr = EV5_IPR_DC_PERR,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "DC_TEST_CTL", .ipr = EV5_IPR_DC_TEST_CTL,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "DC_TEST_TAG", .ipr = EV5_IPR_DC_TEST_TAG,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "DC_MODE", .ipr = EV5_IPR_DC_MODE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dc_mode),
      .writefn = ev5_dcmode_write, .raw_writefn = raw_write,
      .resetfn = ev5_dcmode_reset },
    { .name = "MAF_MODE", .ipr = EV5_IPR_MAF_MODE,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
};

static void register_ev5_ipregs(AlphaCPU *cpu)
{
    int i;

    define_alpha_ipregs(cpu, ev5_ibox_ip_reginfo);
    define_alpha_ipregs(cpu, ev5_mbox_ip_reginfo);

    for (i = 0; i < 24; i++) {
        char *temp_name = g_strdup_printf("PALTEMP%d", i);
        AlphaIPRegInfo temp_reg[] = {
            { .name = temp_name, .ipr = EV5_IPR_PALTEMP_0 + i,
              .access = IPR_RW,
              .fieldoffset = offsetof(CPUAlphaState, ipr.temp[i]) },
        };
        define_one_alpha_ipreg(cpu, temp_reg);
        g_free(temp_name);
    }
}

/* These assumptions should not be violated. */
#pragma GCC diagnostic ignored "-Wenum-compare"
QEMU_BUILD_BUG_ON(R_EV6_IVA_FORM_VA_MASK != R_EV6_VA_FORM_VA_MASK);
QEMU_BUILD_BUG_ON(R_EV6_IVA_FORM_VPTB_MASK != R_EV6_VA_FORM_VPTB_MASK);
QEMU_BUILD_BUG_ON(R_EV6_IVA_FORM_48_VA_MASK != R_EV6_VA_FORM_48_VA_MASK);
QEMU_BUILD_BUG_ON(R_EV6_IVA_FORM_48_VPTB_MASK != R_EV6_VA_FORM_48_VPTB_MASK);
QEMU_BUILD_BUG_ON(R_EV6_IVA_FORM_32_VA_MASK != R_EV6_VA_FORM_32_VA_MASK);
QEMU_BUILD_BUG_ON(R_EV6_IVA_FORM_32_VPTB_MASK != R_EV6_VA_FORM_32_VPTB_MASK);
#pragma GCC diagnostic pop

static uint64_t ev6_va_form(CPUAlphaState *env, bool ifetch)
{
    uint64_t vaddr = alpha_cur_fault_addr(env, ifetch);
    uint64_t vptb = alpha_cur_vptb(env, ifetch);
    uint64_t ret = 0;
    int form = 0;

    form = deposit32(form, 0, 1, alpha_va48_enabled(env, ifetch));
    form = deposit32(form, 1, 1, alpha_va32_enabled(env, ifetch));
    switch (form) {
    case 0b00: /* VA_48 = 0, VA_FORM_32 = 0 */
        vaddr = extract64(vaddr, 0, 43) & TARGET_PAGE_MASK;
        ret  = vptb & R_EV6_VA_FORM_VPTB_MASK;
        ret |= ((vaddr >> 13) << 3) & R_EV6_VA_FORM_VA_MASK;
        break;
    case 0b01: /* VA_48 = 1, VA_FORM_32 = 0 */
        vaddr = sextract64(vaddr, 0, 48) & TARGET_PAGE_MASK;
        ret = vptb & R_EV6_VA_FORM_48_VPTB_MASK;
        ret |= ((vaddr >> 13) << 3) &
               (R_EV6_VA_FORM_48_VA_MASK |
                R_EV6_VA_FORM_48_VA_SEXT0_MASK |
                R_EV6_VA_FORM_48_VA_SEXT1_MASK |
                R_EV6_VA_FORM_48_VA_SEXT2_MASK |
                R_EV6_VA_FORM_48_VA_SEXT3_MASK |
                R_EV6_VA_FORM_48_VA_SEXT4_MASK);
        break;
    case 0b10: /* VA_48 = 0, VA_FORM_32 = 1 */
        vaddr = extract64(vaddr, 0, 32) & TARGET_PAGE_MASK;
        ret = vptb & R_EV6_VA_FORM_32_VPTB_MASK;
        ret |= ((vaddr >> 13) << 3) & R_EV6_VA_FORM_32_VA_MASK;
        break;
    default:
        g_assert_not_reached();
    }
    return ret;
}

static uint64_t ev6_va_form_read_uncached(CPUAlphaState *env)
{
    return ev6_va_form(env, false);
}

static uint64_t ev6_va_form_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return ev6_va_form_read_uncached(env);
}

static void ev6_va_ctl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    uint64_t mask = R_EV6_VA_CTL_B_ENDIAN_MASK |
                    R_EV6_VA_CTL_VA_48_MASK |
                    R_EV6_VA_CTL_VA_FORM_32_MASK |
                    R_EV6_VA_CTL_VPTB_MASK;

    env->ipr.va_ctl = value & mask;
}

static const AlphaIPRegInfo ev6_ebox_ip_reginfo[] = {
    { .name = "CC", .ipr = EV6_IPR_CC,
      .type = ALPHA_IP_NO_RAW | ALPHA_IP_IO, .access = IPR_RW,
      .readfn = cc_read, .writefn = cc_write },
    { .name = "CC_CTL", .ipr = EV6_IPR_CC_CTL,
      .type = ALPHA_IP_IO, .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.cc_ctl),
      .writefn = cc_ctl_write, .raw_writefn = raw_write },
    { .name = "VA", .ipr = EV6_IPR_VA,
      .access = IPR_RW, .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.va) },
    { .name = "VA_FORM", .ipr = EV6_IPR_VA_FORM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev6_va_form_read },
    { .name = "VA_CTL", .ipr = EV6_IPR_VA_CTL,
      .access = IPR_RW, .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.va_ctl),
      .writefn = ev6_va_ctl_write, .raw_writefn = raw_write },
};

static void ev6_itb_tag_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    raw_write(env, ri, value & R_EV6_ITB_TAG_VA_MASK);
}

static void ev6_itb_pte_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Instruction;
    int asn = alpha_cur_asn(env, type, 0);
    uint64_t mask = R_EV6_ITB_PTE_ASM_MASK | R_EV6_ITB_PTE_GH_MASK |
                    R_EV6_ITB_PTE_KRE_MASK | R_EV6_ITB_PTE_ERE_MASK |
                    R_EV6_ITB_PTE_SRE_MASK | R_EV6_ITB_PTE_URE_MASK |
                    R_EV6_ITB_PTE_PFN_MASK;

    alpha_tlb_fill(env, type, env->ipr.itb_tag, value & mask, asn);
}

static void ev6_itb_iap_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    alpha_tlb_flush_process(env, AlphaTLBType_Instruction);
}

static void ev6_itb_ia_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    alpha_tlb_flush_all(env, AlphaTLBType_Instruction);
}

static void ev6_itb_is_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                         uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Instruction;
    int asn = alpha_cur_asn(env, type, 0);

    alpha_tlb_flush_single(env, type, value & R_EV6_ITB_IS_VA_MASK, asn);
}

static uint64_t ev6_iva_form_read_uncached(CPUAlphaState *env)
{
    return ev6_va_form(env, true);
}

static uint64_t ev6_iva_form_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return ev6_iva_form_read_uncached(env);
}

static void ev6_ier_cm_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    if (ri->ipr & 0b10) {
        uint64_t wmask = R_EV6_IER_CM_ASTEN_MASK | R_EV6_IER_CM_SIEN_MASK |
                         R_EV6_IER_CM_PCEN_MASK | R_EV6_IER_CM_CREN_MASK |
                         R_EV6_IER_CM_SLEN_MASK | R_EV6_IER_CM_EIEN_MASK;
        MERGE_FIELD(env->ipr.ier_cm, value, wmask);
    }

    if (ri->ipr & 0b01) {
        /* We may be performing a mode switch, so flush the TLBs. */
        MERGE_FIELD(env->ipr.ier_cm, value, R_EV6_IER_CM_CM_MASK);
        tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    }

    alpha_rebuild_hflags(env);
}

static void ev6_sirr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
    env->ipr.sirr = value & R_EV6_SIRR_SIR_MASK;
}

static uint64_t ev6_isum_read_uncached(CPUAlphaState *env)
{
    uint64_t ret = 0;
    int ast = alpha_pending_ast(env);

    SET_FIELD(ret, EV6_ISUM, EI, alpha_get_eir(env) & alpha_get_eien(env));
    SET_FIELD(ret, EV6_ISUM, SL, alpha_get_slr(env) & alpha_get_slen(env));
    SET_FIELD(ret, EV6_ISUM, CR, alpha_get_crr(env) & alpha_get_cren(env));
    SET_FIELD(ret, EV6_ISUM, PC, alpha_get_pcr(env) & alpha_get_pcen(env));
    SET_FIELD(ret, EV6_ISUM, SI, alpha_get_sir(env) & alpha_get_sien(env));
    SET_FIELD(ret, EV6_ISUM, ASTK, (ast & 0b0001) != 0);
    SET_FIELD(ret, EV6_ISUM, ASTE, (ast & 0b0010) != 0);
    SET_FIELD(ret, EV6_ISUM, ASTS, (ast & 0b0100) != 0);
    SET_FIELD(ret, EV6_ISUM, ASTU, (ast & 0b1000) != 0);
    return ret;
}

static uint64_t ev6_isum_read(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    return ev6_isum_read_uncached(env);
}

static void ev6_hw_int_clr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                                 uint64_t value)
{
    uint32_t cur_irq = env->irq_line_state;

    if (FIELD_EX64(value, EV6_HW_INT_CLR, CR)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_CRR, 1, 0);
    }
    if (FIELD_EX64(value, EV6_HW_INT_CLR, SL)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_SLR, 1, 0);
    }
    if (FIELD_EX64(value, EV6_HW_INT_CLR, PC)) {
        cur_irq = deposit64(cur_irq, ALPHA_CPU_INPUT_PC0, 3, 0);
    }
    env->irq_line_state = cur_irq;
}

static void ev6_ictl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
    uint64_t wmask = ~(R_EV6_I_CTL_SL_RCV_MASK |
                       R_EV6_I_CTL_SL_XMIT_MASK |
                       R_EV6_I_CTL_BIST_FAIL_MASK |
                       R_EV6_I_CTL_IC_EN_MASK |
                       R_EV6_I_CTL_CALL_PAL_R23_MASK |
                       R_EV6_I_CTL_CHIP_ID_MASK);

    MERGE_FIELD(env->ipr.i_ctl, value, wmask);
    env->ipr.i_ctl = sextract64(env->ipr.i_ctl, 0,
                                R_EV6_I_CTL_VPTB_SEXT0_SHIFT);

    alpha_rebuild_hflags(env);
}

static void ev6_ictl_reset(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    AlphaCPU *cpu = env_archcpu(env);

    /* These bits are hardwired. */
    SET_FIELD(env->ipr.i_ctl, EV6_I_CTL, IC_EN, 0b11);
    SET_FIELD(env->ipr.i_ctl, EV6_I_CTL, CALL_PAL_R23, 1);
    SET_FIELD(env->ipr.i_ctl, EV6_I_CTL, CHIP_ID, cpu->chip_id);
}

static void ev6_pctx_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
    int ppce, fpe, aster, astrr, asn;

    ppce = FIELD_EX64(value, EV6_PCTX, PPCE);
    fpe = FIELD_EX64(value, EV6_PCTX, FPE);
    aster = FIELD_EX64(value, EV6_PCTX, ASTER);
    astrr = FIELD_EX64(value, EV6_PCTX, ASTRR);
    asn = FIELD_EX64(value, EV6_PCTX, ASN);

    if (ri->ipr & EV6_IPR_PCTX_PPCE) {
        SET_FIELD(env->ipr.pctx, EV6_PCTX, PPCE, ppce);
    }
    if (ri->ipr & EV6_IPR_PCTX_FPE) {
        SET_FIELD(env->ipr.pctx, EV6_PCTX, FPE, fpe);
    }
    if (ri->ipr & EV6_IPR_PCTX_ASTER) {
        SET_FIELD(env->ipr.pctx, EV6_PCTX, ASTER, aster);
    }
    if (ri->ipr & EV6_IPR_PCTX_ASTRR) {
        SET_FIELD(env->ipr.pctx, EV6_PCTX, ASTRR, astrr);
    }
    if (ri->ipr & EV6_IPR_PCTX_ASN) {
        /* Only flush TLBs if we're switching ASIDs. */
        if (alpha_itb_asn(env) != asn) {
            tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
        }
        SET_FIELD(env->ipr.pctx, EV6_PCTX, ASN, asn);
    }
}

static void ev6_pctx_reset(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    SET_FIELD(env->ipr.pctx, EV6_PCTX, FPE, 1);
}

static void ev6_pctr_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                       uint64_t value)
{
    uint64_t wmask = R_EV6_PCTR_CTL_SL1_MASK | R_EV6_PCTR_CTL_SL0_MASK |
                     R_EV6_PCTR_CTL_PCTR1_MASK | R_EV6_PCTR_CTL_PCTR0_MASK;

    MERGE_FIELD(env->ipr.pctr_ctl, value, wmask);
    env->ipr.pctr_ctl = sextract64(env->ipr.pctr_ctl, 0,
                                   R_EV6_PCTR_CTL_PCTR0_CTL_SEXT0_SHIFT);
}

static const AlphaIPRegInfo ev6_ibox_ip_reginfo[] = {
    { .name = "ITB_TAG", .ipr = EV6_IPR_ITB_TAG,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.itb_tag),
      .writefn = ev6_itb_tag_write, .raw_writefn = raw_write },
    { .name = "ITB_PTE", .ipr = EV6_IPR_ITB_PTE,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_itb_pte_write },
    { .name = "ITB_IAP", .ipr = EV6_IPR_ITB_IAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_itb_iap_write },
    { .name = "ITB_IA", .ipr = EV6_IPR_ITB_IA,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_itb_ia_write },
    { .name = "ITB_IS", .ipr = EV6_IPR_ITB_IS,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_itb_is_write },
    { .name = "PMPC", .ipr = EV6_IPR_PMPC,
      .type = ALPHA_IP_CONST, .access = IPR_R,
      .resetvalue = 0 },
    { .name = "EXC_ADDR", .ipr = EV6_IPR_EXC_ADDR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_addr) },
    { .name = "IVA_FORM", .ipr = EV6_IPR_IVA_FORM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev6_iva_form_read },
    { .name = "CM", .ipr = EV6_IPR_CM,
      .type = ALPHA_IP_ALIAS, .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.ier_cm),
      .writefn = ev6_ier_cm_write, .raw_writefn = raw_write },
    { .name = "IER", .ipr = EV6_IPR_IER,
      .type = ALPHA_IP_ALIAS, .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.ier_cm),
      .writefn = ev6_ier_cm_write, .raw_writefn = raw_write },
    { .name = "IER_CM", .ipr = EV6_IPR_IER_CM,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.ier_cm),
      .writefn = ev6_ier_cm_write, .raw_writefn = raw_write },
    { .name = "SIRR", .ipr = EV6_IPR_SIRR,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.sirr),
      .writefn = ev6_sirr_write, .raw_writefn = raw_write },
    { .name = "ISUM", .ipr = EV6_IPR_ISUM,
      .type = ALPHA_IP_IO | ALPHA_IP_NO_RAW, .access = IPR_R,
      .readfn = ev6_isum_read },
    { .name = "HW_INT_CLR", .ipr = EV6_IPR_HW_INT_CLR,
      .type = ALPHA_IP_IO | ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_hw_int_clr_write },
    { .name = "EXC_SUM", .ipr = EV6_IPR_EXC_SUM,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.exc_sum) },
    { .name = "PAL_BASE", .ipr = EV6_IPR_PAL_BASE,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pal_base),
      .writefn = pal_base_write, .raw_writefn = raw_write },
    { .name = "I_CTL", .ipr = EV6_IPR_I_CTL,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.i_ctl),
      .writefn = ev6_ictl_write, .raw_writefn = raw_write,
      .resetfn = ev6_ictl_reset },
    { .name = "I_STAT", .ipr = EV6_IPR_I_STAT,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "IC_FLUSH", .ipr = EV6_IPR_IC_FLUSH,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "IC_FLUSH_ASM", .ipr = EV6_IPR_IC_FLUSH_ASM,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "CLR_MAP", .ipr = EV6_IPR_CLR_MAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = alpha_ip_write_ignore },
    { .name = "SLEEP", .ipr = EV6_IPR_SLEEP,
      .type = ALPHA_IP_NOP, .access = IPR_W },
    { .name = "PCTX0", .ipr = EV6_IPR_PCTX0,
      .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pctx),
      .writefn = ev6_pctx_write, .raw_writefn = raw_write,
      .resetfn = ev6_pctx_reset },
    { .name = "PCTX1", .ipr = EV6_IPR_PCTX1,
      .type = ALPHA_IP_ALIAS, .access = IPR_RW,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pctx),
      .writefn = ev6_pctx_write, .raw_writefn = raw_write },
    { .name = "PCTR_CTL", .ipr = EV6_IPR_PCTR_CTL,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.pctr_ctl),
      .writefn = ev6_pctr_write, .raw_writefn = raw_write },
};

static void ev6_dtb_tag_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                          uint64_t value)
{
    raw_write(env, ri, value & R_EV6_DTB_TAG_VA_MASK);
}

static void ev6_dtb_pte_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Data;
    int index = !!(ri->ipr == EV6_IPR_DTB_PTE1);
    int asn = alpha_cur_asn(env, type, index);
    uint64_t mask = R_EV6_DTB_PTE_FOR_MASK | R_EV6_DTB_PTE_FOW_MASK |
                    R_EV6_DTB_PTE_ASM_MASK | R_EV6_DTB_PTE_GH_MASK |
                    R_EV6_DTB_PTE_KRE_MASK | R_EV6_DTB_PTE_ERE_MASK |
                    R_EV6_DTB_PTE_SRE_MASK | R_EV6_DTB_PTE_URE_MASK |
                    R_EV6_DTB_PTE_KWE_MASK | R_EV6_DTB_PTE_EWE_MASK |
                    R_EV6_DTB_PTE_SWE_MASK | R_EV6_DTB_PTE_UWE_MASK |
                    R_EV6_DTB_PTE_PFN_MASK;

    alpha_tlb_fill(env, type, env->ipr.dtb_tag[index], value & mask, asn);
}

static void ev6_dtb_altmode_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                                  uint64_t value)
{
    CPUState *cs = env_cpu(env);

    env->ipr.dtb_altmode = value & R_EV6_DTB_ALT_MODE_MODE_MASK;
    tlb_flush_by_mmuidx(cs, altmode_tlbmask());
}

static void ev6_dtb_iap_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    alpha_tlb_flush_process(env, AlphaTLBType_Data);
}

static void ev6_dtb_ia_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    alpha_tlb_flush_all(env, AlphaTLBType_Data);
}

static void ev6_dtb_is_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                             uint64_t value)
{
    AlphaTLBType type = AlphaTLBType_Data;
    int index = !!(ri->ipr == EV6_IPR_DTB_IS1);
    int asn = alpha_cur_asn(env, type, index);

    alpha_tlb_flush_single(env, type, value & R_EV6_DTB_IS_VA_MASK, asn);
}

static void ev6_dtb_asn_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                              uint64_t value)
{
    int index = !!(ri->ipr == EV6_IPR_DTB_ASN1);
    int asn = FIELD_EX64(value, EV6_DTB_ASN, ASN);

    /* Only flush TLBs if we're switching ASIDs. */
    if (alpha_dtb_asn(env, index) != asn) {
        tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
    }

    raw_write(env, ri, value & R_EV6_DTB_ASN_ASN_MASK);
}

static void ev6_mctl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value)
{
    uint64_t wmask = R_EV6_M_CTL_SPE_MASK | R_EV6_M_CTL_SMC_MASK;

    MERGE_FIELD(env->ipr.m_ctl, value, wmask);
}

static void ev6_dcctl_write(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                            uint64_t value)
{
    uint64_t mask = R_EV6_DC_CTL_SET_EN_MASK |
                    R_EV6_DC_CTL_F_HIT_MASK |
                    R_EV6_DC_CTL_FLUSH_MASK |
                    R_EV6_DC_CTL_F_BAD_TPAR_MASK |
                    R_EV6_DC_CTL_F_BAD_DECC_MASK |
                    R_EV6_DC_CTL_DCTAG_PAR_EN_MASK |
                    R_EV6_DC_CTL_DCDAT_ERR_EN_MASK;

    raw_write(env, ri, (value & mask) | R_EV6_DC_CTL_SET_EN_MASK);
}

static void ev6_dcctl_reset(CPUAlphaState *env, const AlphaIPRegInfo *ri)
{
    SET_FIELD(env->ipr.dc_ctl, EV6_DC_CTL, SET_EN, 0b11);
}

static const AlphaIPRegInfo ev6_mbox_ip_reginfo[] = {
    { .name = "DTB_TAG0", .ipr = EV6_IPR_DTB_TAG0,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_tag[0]),
      .writefn = ev6_dtb_tag_write, .raw_writefn = raw_write },
    { .name = "DTB_TAG1", .ipr = EV6_IPR_DTB_TAG1,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_tag[1]),
      .writefn = ev6_dtb_tag_write, .raw_writefn = raw_write },
    { .name = "DTB_PTE0", .ipr = EV6_IPR_DTB_PTE0,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_dtb_pte_write },
    { .name = "DTB_PTE1", .ipr = EV6_IPR_DTB_PTE1,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_dtb_pte_write },
    { .name = "DTB_ALTMODE", .ipr = EV6_IPR_DTB_ALTMODE,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_altmode),
      .writefn = ev6_dtb_altmode_write, .raw_writefn = raw_write },
    { .name = "DTB_IAP", .ipr = EV6_IPR_DTB_IAP,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_dtb_iap_write },
    { .name = "DTB_IA", .ipr = EV6_IPR_DTB_IA,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_dtb_ia_write },
    { .name = "DTB_IS0", .ipr = EV6_IPR_DTB_IS0,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_dtb_is_write },
    { .name = "DTB_IS1", .ipr = EV6_IPR_DTB_IS1,
      .type = ALPHA_IP_NO_RAW, .access = IPR_W,
      .writefn = ev6_dtb_is_write },
    { .name = "DTB_ASN0", .ipr = EV6_IPR_DTB_ASN0,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_asn[0]),
      .writefn = ev6_dtb_asn_write, .raw_writefn = raw_write },
    { .name = "DTB_ASN1", .ipr = EV6_IPR_DTB_ASN1,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dtb_asn[1]),
      .writefn = ev6_dtb_asn_write, .raw_writefn = raw_write },
    { .name = "MM_STAT", .ipr = EV6_IPR_MM_STAT,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.mm_stat),
      .writefn = alpha_ip_write_ignore, .raw_writefn = raw_write },
    { .name = "M_CTL", .ipr = EV6_IPR_M_CTL,
      .access = IPR_W,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.m_ctl),
      .writefn = ev6_mctl_write, .raw_writefn = raw_write },
    { .name = "DC_CTL", .ipr = EV6_IPR_DC_CTL,
      .access = IPR_RW,
      .resetvalue = 0,
      .fieldoffset = offsetof(CPUAlphaState, ipr.dc_ctl),
      .writefn = ev6_dcctl_write, .raw_writefn = raw_write,
      .resetfn = ev6_dcctl_reset },
    { .name = "DC_STAT", .ipr = EV6_IPR_DC_STAT,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "M_FIX", .ipr = EV6_IPR_M_FIX,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
};

static const AlphaIPRegInfo ev6_cbox_ip_reginfo[] = {
    { .name = "C_DATA", .ipr = EV6_IPR_C_DATA,
      .type = ALPHA_IP_CONST, .access = IPR_RW,
      .resetvalue = 0 },
    { .name = "C_SHFT", .ipr = EV6_IPR_C_SHFT,
      .type = ALPHA_IP_NOP, .access = IPR_W },
};

static void register_ev6_ipregs(AlphaCPU *cpu)
{
    int i;

    define_alpha_ipregs(cpu, ev6_ebox_ip_reginfo);
    define_alpha_ipregs(cpu, ev6_ibox_ip_reginfo);
    define_alpha_ipregs(cpu, ev6_mbox_ip_reginfo);
    define_alpha_ipregs(cpu, ev6_cbox_ip_reginfo);

    for (i = 1; i < 0x20; i++) {
        char *pctx0_name = g_strdup_printf("PCTX0[%d]", i);
        char *pctx1_name = g_strdup_printf("PCTX1[%d]", i);
        AlphaIPRegInfo pctx_regs[] = {
            { .name = pctx0_name, .ipr = EV6_IPR_PCTX0 + i,
              .type = ALPHA_IP_ALIAS, .access = IPR_RW,
              .fieldoffset = offsetof(CPUAlphaState, ipr.pctx),
              .writefn = ev6_pctx_write, .raw_writefn = raw_write },
            { .name = pctx1_name, .ipr = EV6_IPR_PCTX1 + i,
              .type = ALPHA_IP_ALIAS, .access = IPR_RW,
              .fieldoffset = offsetof(CPUAlphaState, ipr.pctx),
              .writefn = ev6_pctx_write, .raw_writefn = raw_write },
        };
        define_alpha_ipregs(cpu, pctx_regs);
        g_free(pctx0_name);
        g_free(pctx1_name);
    }
}

void alpha_cpu_register_ipregs(AlphaCPU *cpu)
{
    switch (cpu->isa_implver) {
    case IMPLVER_2106x:
        register_ev4_ipregs(cpu);
        break;
    case IMPLVER_21164:
        register_ev5_ipregs(cpu);
        break;
    case IMPLVER_21264:
        register_ev6_ipregs(cpu);
        break;
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_exception_unmasked(CPUState *cs, int excp_idx)
{
    CPUAlphaState *env = cpu_env(cs);
    bool unmasked = false;

    /* We never take interrupts while in PALmode. */
    if (alpha_is_pal(env)) {
        return false;
    }

    switch (excp_idx) {
    case EXCP_IRQ:
        unmasked = alpha_cpu_eirq_pending(env);
        break;
    case EXCP_SIRQ:
        unmasked = alpha_cpu_sirq_pending(env);
        break;
    case EXCP_ASTIRQ:
        unmasked = alpha_cpu_astirq_pending(env);
        break;
    default:
        g_assert_not_reached();
    }

    return unmasked;
}

bool alpha_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUClass *cc = CPU_GET_CLASS(cs);
    int excp_idx;

    /*
     * Interrupt priorities are handled per the VMS priority levels. Device
     * IRQs take highest priority, followed by software interrupts, finally
     * followed by ASTs.
     */
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        excp_idx = EXCP_IRQ;
        if (alpha_exception_unmasked(cs, excp_idx)) {
            goto found;
        }
    }
    if (interrupt_request & CPU_INTERRUPT_SIRQ) {
        excp_idx = EXCP_SIRQ;
        if (alpha_exception_unmasked(cs, excp_idx)) {
            goto found;
        }
    }
    if (interrupt_request & CPU_INTERRUPT_ASTIRQ) {
        excp_idx = EXCP_ASTIRQ;
        if (alpha_exception_unmasked(cs, excp_idx)) {
            goto found;
        }
    }
    return false;

found:
    cs->exception_index = excp_idx;
    (*cc->tcg_ops->do_interrupt)(cs);
    return true;
}

bool alpha_cpu_exec_halt(CPUState *cs)
{
    return cpu_has_work(cs);
}

static void alpha_cpu_dump_ipreg_value(gpointer key, gpointer opaque)
{
    AlphaCPU *cpu = opaque;
    CPUAlphaState *env = &cpu->env;
    const AlphaIPRegInfo *ri;

    ri = g_hash_table_lookup(cpu->ipregs, key);
    if (!(ri->type & (ALPHA_IP_NO_RAW | ALPHA_IP_ALIAS))) {
        g_autofree char *prefix = g_strdup_printf("%s:", ri->name);

        qemu_printf("0x%04x/%-16s 0x" TARGET_FMT_lx "\n",
                    ri->ipr, prefix, read_raw_ipreg(env, ri));
    }
}

void alpha_cpu_dump_iprs(CPUState *cs)
{
    AlphaCPU *cpu = ALPHA_CPU(cs);
    GList *keys;

    cpu_synchronize_state(cs);
    keys = g_hash_table_get_keys(cpu->ipregs);
    keys = g_list_sort_with_data(keys, (GCompareDataFunc)ipreg_key_compare, NULL);
    g_list_foreach(keys, alpha_cpu_dump_ipreg_value, cpu);
    g_list_free(keys);
}
