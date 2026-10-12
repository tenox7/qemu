/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ALPHA_TRANSLATE_H
#define _ALPHA_TRANSLATE_H

#include "tcg/tcg-op.h"
#include "tcg/tcg-op-gvec.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "exec/helper-gen.h"
#include "cpu.h"
#include "internals.h"

#define DISAS_EXIT         DISAS_TARGET_0  /* exit to main loop, pc updated */
#define DISAS_EXIT_UPDATE  DISAS_TARGET_1  /* exit to main loop, pc stale */
#define DISAS_CHAIN        DISAS_TARGET_2  /* lookup next tb, pc updated */
#define DISAS_CHAIN_UPDATE DISAS_TARGET_3  /* lookup next tb, pc stale */

extern const TranslatorOps alpha_translator_ops;

typedef struct DisasContext {
    DisasContextBase base;

    /* CPU state. */
    CPUState *cs;

    /* MMU index to use for normal loads/stores */
    AlphaMMUIdx normal_mmu_idx;
    AlphaMMUIdx altmode_mmu_idx;

    /* The address of the current instruction being translated. */
    target_ulong pc_curr;
    target_ulong pc_save;

    /* Current opcode. */
    uint32_t insn;

    /* Internal state. */
    GHashTable *ipregs;
    uint32_t isa_procid;
    uint32_t isa_amask;
    uint32_t isa_implver;
    int tb_rm;
    int tb_ftz;
    bool pal_mode;
    bool hwe_enabled;
    bool sde_enabled;
    bool fp_enabled;
    bool insn_start_updated;
} DisasContext;

/*
 * Save pc_save across a branch, so that we may restore the value from
 * before the branch at the point the label is emitted.
 */
typedef struct DisasLabel {
    TCGLabel *label;
    target_ulong pc_save;
} DisasLabel;

/* Function prototypes for gen_ functions for calling helpers */
typedef void LoadFpFn(DisasContext *, TCGv_i64, TCGv_i64);
typedef void StoreFpFn(DisasContext *, TCGv_i64, TCGv_i64);
typedef void IeeeUnaryFn(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i32);
typedef void IeeeConvertFn(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i32);
typedef void IeeeBinaryFn(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i64, TCGv_i32);
typedef void IeeeCompareFn(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i64, TCGv_i32);
typedef void VaxUnaryFn(TCGv_i64, TCGv_ptr, TCGv_i64);
typedef void VaxConvertFn(TCGv_i64, TCGv_ptr, TCGv_i64);
typedef void VaxBinaryFn(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i64);
typedef void VaxCompareFn(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i64);

static inline void disas_save_opcode(DisasContext *s, uint32_t code)
{
    /* Check for multiple updates.  */
    assert(!s->insn_start_updated);
    s->insn_start_updated = true;
    tcg_set_insn_start_param(s->base.insn_start, 1, s->insn);
    tcg_set_insn_start_param(s->base.insn_start, 2, code);
}

/*
 * Constant expanders for the decoders.
 */

static inline int32_t shl_16(DisasContext *s, int32_t x)
{
    return (uint32_t)x << 16;
}

static inline uint32_t lshr_8(DisasContext *s, uint32_t x)
{
    return x >> 8;
}

/*
 * Helper functions.
 */

static inline int curr_insn_len(DisasContext *s)
{
    return s->base.pc_next - s->pc_curr;
}

static inline int get_mem_index(DisasContext *s)
{
    return s->normal_mmu_idx;
}

static inline int get_mem_index_altmode(DisasContext *s)
{
    return s->altmode_mmu_idx;
}

/*
 * gen_disas_label:
 * Create a label and cache a copy of pc_save.
 */
static inline DisasLabel gen_disas_label(DisasContext *s)
{
    return (DisasLabel){
        .label = gen_new_label(),
        .pc_save = s->pc_save,
    };
}

/*
 * set_disas_label:
 * Emit a label and restore the cached copy of pc_save.
 */
static inline void set_disas_label(DisasContext *s, DisasLabel l)
{
    gen_set_label(l.label);
    s->pc_save = l.pc_save;
}

/**
 * arm_tbflags_from_tb:
 * @tb: the TranslationBlock
 *
 * Extract the flag values from @tb.
 */
static inline uint32_t alpha_tbflags_from_tb(const TranslationBlock *tb)
{
    return tb->flags;
}

/*
 * Forward to the isar_feature_* tests given a DisasContext pointer.
 */
#define dc_amask_feature(name, ctx) \
    ({ DisasContext *__ctx = (ctx); amask_feature_##name(__ctx->isa_amask); })

/*
 * Helpers for implementing sets of trans_* functions.
 * Defer the implementation of NAME to FUNC, with optional extra arguments.
 */
#define TRANS(NAME, FUNC, ...) \
    static bool trans_##NAME(DisasContext *s, arg_##NAME *a) \
    { return FUNC(s, __VA_ARGS__); }
#define TRANS_FEAT(NAME, FEAT, FUNC, ...) \
    static bool trans_##NAME(DisasContext *s, arg_##NAME *a) \
    { return dc_amask_feature(FEAT, s) && FUNC(s, __VA_ARGS__); }

/* Store to the CPU state */
#define store_cpu_field(val, name)                                      \
    ({                                                                  \
        QEMU_BUILD_BUG_ON(sizeof_field(CPUAlphaState, name) != 4        \
                          && sizeof_field(CPUAlphaState, name) != 1);   \
        store_cpu_offset(val, offsetof(CPUAlphaState, name),            \
                         sizeof_field(CPUAlphaState, name));            \
    })

#define store_cpu_field_constant(val, name) \
    store_cpu_field(tcg_constant_i32(val), name)

/* Load from the CPU state */
#define load_cpu_field(name)                                            \
    ({                                                                  \
        QEMU_BUILD_BUG_ON(sizeof_field(CPUAlphaState, name) != 4        \
                          && sizeof_field(CPUAlphaState, name) != 1);   \
        load_cpu_offset(offsetof(CPUAlphaState, name),                  \
                        sizeof_field(CPUAlphaState, name));             \
    })

static inline TCGv_i32 load_cpu_offset(int offset, int size)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    switch (size) {
    case 1:
        tcg_gen_ld8u_i32(tmp, tcg_env, offset);
        break;
    case 4:
        tcg_gen_ld_i32(tmp, tcg_env, offset);
        break;
    }
    return tmp;
}

/*
 * Store var into env + offset to a member with size bytes.
 */
static inline void store_cpu_offset(TCGv_i32 var, int offset, int size)
{
    switch (size) {
    case 1:
        tcg_gen_st8_i32(var, tcg_env, offset);
        break;
    case 4:
        tcg_gen_st_i32(var, tcg_env, offset);
        break;
    default:
        g_assert_not_reached();
    }
}

#endif /* _ALPHA_TRANSLATE_H */
