/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU -- internal functions and types
 */

#ifndef _ALPHA_INTERNALS_H
#define _ALPHA_INTERNALS_H

#include "hw/core/registerfields.h"
#include "exec/cpu-common.h"
#include "fpu/softfloat.h"
#include "cpu.h"
#include "instmap.h"

extern const char *const alpha_gregnames[32];
extern const char *const alpha_fregnames[32];

#define IPR_EX64(member, reg, field)                                     \
    FIELD_EX64(env->ipr.member, reg, field)
#define IPR_DP64(member, reg, field, val)                               \
    FIELD_DP64(env->ipr.member, reg, field, val)

#define IPL_HIGH                31      /* highest level */
#define IPL_MCHK                31      /* machine check */
#define IPL_PWRFAIL             30      /* power failure level  */
#define IPL_CLOCK               29      /* performance counter level */
#define IPL_EIRQ_MAX            23      /* highest hardware level */
#define IPL_EIRQ_MIN            20      /* lowest hardware level */
#define IPL_SIRQ_MAX            15      /* highest software level */
#define IPL_AST                 2       /* AST enable level */

/**
 * excp_is_internal:
 * Return true if this exception number represents a QEMU-internal exception
 * that will not be passed to the guest.
 */
static inline bool excp_is_internal(int excp)
{
    switch (excp) {
    case EXCP_INTERRUPT:
    case EXCP_HLT:
    case EXCP_DEBUG:
    case EXCP_HALTED:
    case EXCP_CALL_PAL:
    case EXCP_HW_REI:
        return true;
    default:
        return false;
    }
}

/**
 * raise_exception: Raise the specified exception.
 *
 * Raise a guest exception with the specified value and syndrome. This should
 * be called from helper functions, and never returns because we will
 * longjump back up to the CPU main loop.
 */
G_NORETURN void do_raise_exception(CPUAlphaState *env, int excp,
                                   uint32_t syndrome);

/*
 * Similarly, but also use unwinding to restore cpu state.
 */
G_NORETURN void do_raise_exception_ra(CPUAlphaState *env, int excp,
                                      uint32_t syndrome, uintptr_t ra);

const char *alpha_cpu_exception_name(int excp);
void alpha_cpu_register(const AlphaCPUInfo *info);
void alpha_cpu_register_ipregs(AlphaCPU *cpu);
void alpha_cpu_reset_ipregs(AlphaCPU *cpu);
void alpha_cpu_init_ipreg_list(AlphaCPU *cpu);
void alpha_translate_init(void);
void alpha_translate_code(CPUState *cs, TranslationBlock *tb,
                          int *max_insns, vaddr pc, void *host_pc);
void alpha_restore_state_to_opc(CPUState *cs, const TranslationBlock *tb,
                                const uint64_t *data);
void alpha_synchronize_from_tb(CPUState *cs, const TranslationBlock *tb);
void assert_hflags_rebuild_correctly(CPUAlphaState *env);

/**
 * AlphaFaultType: type of an Alpha MMU fault.
 */
typedef enum AlphaFaultType {
    AlphaFault_None,
    AlphaFault_Address,
    AlphaFault_Permission,
    AlphaFault_Translation,
    AlphaFault_Double,
    AlphaFault_Alignment,
    AlphaFault_Access,
} AlphaFaultType;

typedef enum AlphaTLBType {
    AlphaTLBType_Instruction,
    AlphaTLBType_Data,
} AlphaTLBType;

void alpha_tlb_flush_all(CPUAlphaState *env, AlphaTLBType type);
void alpha_tlb_flush_process(CPUAlphaState *env, AlphaTLBType type);
void alpha_tlb_flush_single(CPUAlphaState *env, AlphaTLBType type, vaddr addr,
                            int asn);
void alpha_tlb_fill(CPUAlphaState *env, AlphaTLBType type, vaddr address,
                    uint64_t pte, int asn);

/**
 * AlphaMMUFaultInfo: Information describing an Alpha MMU fault.
 * @type: Type of fault
 * @fault_read: True if the fault-on-read (FOR) bit is set
 * @fault_write: True if the fault-on-read (FOW) bit is set
 * @ifetch: True if this is an instruction fetch
 */
typedef struct AlphaMMUFaultInfo {
    AlphaFaultType type;
    bool fault_read;
    bool fault_write;
    bool ifetch;
} AlphaMMUFaultInfo;

/* Fields that are valid upon success. */
typedef struct GetPhysAddrResult {
    CPUTLBEntryFull f;
} GetPhysAddrResult;

uint32_t alpha_ldl_phys(CPUState *cs, hwaddr addr);
uint64_t alpha_ldq_phys(CPUState *cs, hwaddr addr);
void alpha_stl_phys(CPUState *cs, hwaddr addr, uint32_t val);
void alpha_stq_phys(CPUState *cs, hwaddr addr, uint64_t val);


/**
 * alpha_cpu_do_transaction_failed:
 * Handle a memory system error response (eg "no device/memory present at
 * address") by raising a machine check exception.
 */
void alpha_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr,
                                     vaddr addr, unsigned size,
                                     MMUAccessType access_type,
                                     int mmu_idx, MemTxAttrs attrs,
                                     MemTxResult response, uintptr_t retaddr);

/**
 * alpha_cpu_do_unaligned_access:
 * Raise a data fault alignment exception for the specified virtual address.
 */
G_NORETURN void alpha_cpu_do_unaligned_access(CPUState *cs, vaddr address,
                                              MMUAccessType access_type,
                                              int mmu_idx, uintptr_t retaddr);

static inline int alpha_env_mmu_index(CPUAlphaState *env, bool ifetch)
{
    return ifetch ? EX_TBFLAG(env->hflags, IMMU_IDX)
                  : EX_TBFLAG(env->hflags, DMMU_IDX);
}

/**
 * alpha_env_set_pc:
 * Set the current PC to the given value and respect the PAL mode bit.
 */
static inline void alpha_env_set_pc(CPUAlphaState *env, uint64_t value)
{
    env->pc = value & R_PC_VALUE_MASK;
    env->pal_mode = value & R_PC_PAL_MODE_MASK;
}

/**
 * alpha_env_reset_float_exceptions:
 * Clear all floating point exceptions.
 */
static inline void alpha_env_reset_float_exceptions(CPUAlphaState *env)
{
    set_float_exception_flags(0, &env->fp_status);
}

/**
 * alpha_pamax:
 * Returns the implementation defined bit-width of addressable physical memory.
 */
static inline unsigned alpha_pamax(uint32_t implver)
{
    static const unsigned pamax_map[] = {
        [IMPLVER_2106x] = 34,
        [IMPLVER_21164] = 40,
        [IMPLVER_21264] = 44,
        [IMPLVER_21364] = 44,
    };
    assert(implver < ARRAY_SIZE(pamax_map));
    return pamax_map[implver];
}

static inline uint64_t alpha_pamask(uint32_t implver)
{
    return MAKE_64BIT_MASK(0, alpha_pamax(implver));
}

/**
 * alpha_vamax:
 * Returns the implementation defined bit-width of addressable virtual memory.
 */
static inline unsigned alpha_vamax(uint32_t implver)
{
    static const unsigned vamax_map[] = {
        [IMPLVER_2106x] = 43,
        [IMPLVER_21164] = 43,
        [IMPLVER_21264] = 48,
        [IMPLVER_21364] = 48,
    };
    assert(implver < ARRAY_SIZE(vamax_map));
    return vamax_map[implver];
}

static inline uint64_t alpha_vamask(uint32_t implver)
{
    return MAKE_64BIT_MASK(0, alpha_vamax(implver));
}

/*
 * Register field accessors.
 */

static inline uint64_t alpha_get_pal_base_mask(int implver)
{
    switch (implver) {
    case IMPLVER_2106x:
        return R_EV4_PAL_BASE_PAL_BASE_MASK;
    case IMPLVER_21164:
        return R_EV5_PAL_BASE_PAL_BASE_MASK;
    default:
        return R_EV6_PAL_BASE_PAL_BASE_MASK;
    }
}

static inline uint64_t alpha_get_pal_base(CPUAlphaState *env)
{
    return env->ipr.pal_base & alpha_get_pal_base_mask(env->implver);
}

static inline uint32_t alpha_get_ipl(CPUAlphaState *env)
{
    assert(env->implver == IMPLVER_21164);
    return IPR_EX64(ipl, EV5_IPL, IPL);
}

static inline bool alpha_ipl_test(CPUAlphaState *env, int req_level)
{
    return (req_level > alpha_get_ipl(env));
}

static inline uint32_t alpha_get_sir(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(sirr, EV4_SIRR, SIR);
    case IMPLVER_21164:
        return IPR_EX64(sirr, EV5_SIRR, SIR);
    case IMPLVER_21264:
        return IPR_EX64(sirr, EV6_SIRR, SIR);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_slr(CPUAlphaState *env)
{
    return extract64(env->irq_line_state, ALPHA_CPU_INPUT_SLR, 1);
}

static inline uint32_t alpha_get_crr(CPUAlphaState *env)
{
    return extract64(env->irq_line_state, ALPHA_CPU_INPUT_CRR, 1);
}

static inline uint32_t alpha_get_pcr(CPUAlphaState *env)
{
    return extract64(env->irq_line_state, ALPHA_CPU_INPUT_PC0, 3);
}

static inline uint32_t alpha_get_eir(CPUAlphaState *env)
{
    return extract64(env->irq_line_state, ALPHA_CPU_INPUT_IRQ0, 6);
}

static inline uint32_t alpha_get_sien_from_ipl(CPUAlphaState *env)
{
    uint32_t sien = MAKE_64BIT_MASK(0, R_EV5_SIRR_SIR_LENGTH);
    int ipl = alpha_get_ipl(env);
    int bit;

    assert(env->implver == IMPLVER_21164);
    for (bit = 0; bit < MIN(ipl, IPL_SIRQ_MAX); bit++) {
        sien &= ~BIT(bit);
    }
    return sien;
}

static inline uint32_t alpha_get_eien_from_ipl(CPUAlphaState *env)
{
    uint32_t eien = MAKE_64BIT_MASK(0, R_EV5_ICSR_IMSK_LENGTH);
    int ipl = alpha_get_ipl(env);
    int bit;

    for (bit = IPL_EIRQ_MIN;
         bit < CLAMP(ipl, IPL_EIRQ_MIN, IPL_EIRQ_MAX + 1);
         bit++) {
        eien &= ~BIT(bit - IPL_EIRQ_MIN);
    }
    return eien;
}

static inline uint32_t alpha_get_sien(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(sier, EV4_SIER, SIER);
    case IMPLVER_21164:
        return alpha_get_sien_from_ipl(env);
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, SIEN);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_eien(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(hier, EV4_HIER, HIER);
    case IMPLVER_21164:
        return ((~IPR_EX64(icsr, EV5_ICSR, IMSK)) & 0b1111) &
               alpha_get_eien_from_ipl(env);
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, EIEN);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_slen(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(hier, EV4_HIER, SLE);
    case IMPLVER_21164:
        return IPR_EX64(icsr, EV5_ICSR, SLE);
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, SLEN);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_cren(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(hier, EV4_HIER, CRE);
    case IMPLVER_21164:
        return IPR_EX64(icsr, EV5_ICSR, CRDE);
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, CREN);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_pcen(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(hier, EV4_HIER, PC0) |
               IPR_EX64(hier, EV4_HIER, PC1) << 1;
    case IMPLVER_21164:
        if (alpha_ipl_test(env, IPL_CLOCK)) {
            return MAKE_64BIT_MASK(0, 2);
        }
        return 0;
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, PCEN);
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_get_asten(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(sier, EV4_SIER, SIER) & BIT(IPL_AST);
    case IMPLVER_21164:
        return alpha_ipl_test(env, IPL_AST);
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, ASTEN);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_aster(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(aster, EV4_ASTER, ASTER);
    case IMPLVER_21164:
        return IPR_EX64(aster, EV5_ASTER, ASTER);
    case IMPLVER_21264:
        return IPR_EX64(pctx, EV6_PCTX, ASTER);
    default:
        g_assert_not_reached();
    }
}

static inline uint32_t alpha_get_astrr(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(astrr, EV4_ASTRR, ASTRR);
    case IMPLVER_21164:
        return IPR_EX64(astrr, EV5_ASTRR, ASTRR);
    case IMPLVER_21264:
        return IPR_EX64(pctx, EV6_PCTX, ASTRR);
    default:
        g_assert_not_reached();
    }
}

static inline uint8_t alpha_dtb_asn(CPUAlphaState *env, int index)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return 0;
    case IMPLVER_21164:
        return IPR_EX64(dtb_asn[0], EV5_DTB_ASN, ASN);
    case IMPLVER_21264:
        if (index) {
            return IPR_EX64(dtb_asn[0], EV6_DTB_ASN, ASN);
        } else {
            return IPR_EX64(dtb_asn[1], EV6_DTB_ASN, ASN);
        }
    default:
        g_assert_not_reached();
    }
}

static inline uint8_t alpha_itb_asn(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(iccsr, EV4_ICCSR, ASN);
    case IMPLVER_21164:
        return IPR_EX64(itb_asn, EV5_ITB_ASN, ASN);
    case IMPLVER_21264:
        return IPR_EX64(pctx, EV6_PCTX, ASN);
    default:
        g_assert_not_reached();
    }
}

static inline uint8_t alpha_cur_asn(CPUAlphaState *env, AlphaTLBType type,
                                    int index)
{
    switch (type) {
    case AlphaTLBType_Data:
        return alpha_dtb_asn(env, index);
    case AlphaTLBType_Instruction:
        assert(index == 0);
        return alpha_itb_asn(env);
    }
    g_assert_not_reached();
}

static inline uint8_t alpha_cur_mode(CPUAlphaState *env, bool ifetch)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(ps, EV4_PS, CM);
    case IMPLVER_21164:
        return ifetch ? IPR_EX64(icm, EV5_ICM, CM)
                      : IPR_EX64(dtb_cm, EV5_DTB_CM, CM);
    case IMPLVER_21264:
        return IPR_EX64(ier_cm, EV6_IER_CM, CM);
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t alpha_cur_vptb(CPUAlphaState *env, bool ifetch)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return 0;
    case IMPLVER_21164: {
        QEMU_BUILD_BUG_ON(ALPHA_BITMASK(63, 30) != R_EV5_MVPTBR_VPTB_MASK);
        if (ifetch) {
            return env->ipr.ivptbr & R_EV5_MVPTBR_VPTB_MASK;
        } else {
            return env->ipr.mvptbr & R_EV5_IVPTBR_VPTB_MASK;
        }
    }
    case IMPLVER_21264: {
        const uint64_t vptb_mask = ALPHA_BITMASK(47, 30);

        QEMU_BUILD_BUG_ON(ALPHA_BITMASK(47, 30) != R_EV6_I_CTL_VPTB_MASK);
        if (ifetch) {
            return sextract64(env->ipr.i_ctl & vptb_mask, 0, 48);
        } else {
            return sextract64(env->ipr.va_ctl & vptb_mask, 0, 48);
        }
    }
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t alpha_cur_fault_addr(CPUAlphaState *env, bool ifetch)
{
    return ifetch ? env->ipr.exc_addr : env->ipr.va;
}

static inline int alpha_mmu_index(CPUAlphaState *env, bool ifetch)
{
    if (ifetch && alpha_is_pal(env)) {
        return AlphaMMUIdx_PAL;
    }
    return alpha_cur_mode(env, ifetch);
}

static inline int alpha_mmu_index_altmode(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(altmode, EV4_ALT_MODE, AM);
    case IMPLVER_21164:
        return IPR_EX64(altmode, EV5_ALT_MODE, AM);
    case IMPLVER_21264:
        return IPR_EX64(dtb_altmode, EV6_DTB_ALT_MODE, MODE);
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_palshadow_enabled(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return false;
    case IMPLVER_21164:
        return IPR_EX64(icsr, EV5_ICSR, SDE);
    case IMPLVER_21264:
        return IPR_EX64(i_ctl, EV6_I_CTL, SDE);
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_palres_enabled(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(iccsr, EV4_ICCSR, HWE);
    case IMPLVER_21164:
        return IPR_EX64(icsr, EV5_ICSR, HWE);
    case IMPLVER_21264:
        return IPR_EX64(i_ctl, EV6_I_CTL, HWE);
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_fpu_enabled(CPUAlphaState *env)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return IPR_EX64(iccsr, EV4_ICCSR, FPE);
    case IMPLVER_21164:
        return IPR_EX64(icsr, EV5_ICSR, FPE);
    case IMPLVER_21264:
        return IPR_EX64(pctx, EV6_PCTX, FPE);
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_va48_enabled(CPUAlphaState *env, bool ifetch)
{
    switch (env->implver) {
    case IMPLVER_2106x:
    case IMPLVER_21164:
        return false;
    case IMPLVER_21264:
        /*
         * Table 5–11 Ibox Control Register Fields Description (Continued):
         *
         * This bit controls the format applied to effective virtual
         * addresses by the IVA_FORM register and the Ibox virtual address
         * sign extension checkers. [...] The effect of this bit on the
         * IVA_FORM register is identical to the effect of VA_CTL[VA_48]
         * on the VA_FORM register. See Section 5.1.5.
         */
        return ifetch ? IPR_EX64(i_ctl, EV6_I_CTL, VA_48)
                      : IPR_EX64(va_ctl, EV6_VA_CTL, VA_48);
    default:
        g_assert_not_reached();
    }
}

static inline bool alpha_va32_enabled(CPUAlphaState *env, bool ifetch)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return false;
    case IMPLVER_21164:
        return ifetch ? IPR_EX64(icsr, EV5_ICSR, SPE_1)
                      : IPR_EX64(mcsr, EV5_MCSR, SPE_1);
    case IMPLVER_21264:
        return ifetch ? IPR_EX64(i_ctl, EV6_I_CTL, VA_FORM_32)
                      : IPR_EX64(va_ctl, EV6_VA_CTL, VA_FORM_32);
    default:
        g_assert_not_reached();
    }
}

static inline int alpha_superpage_status(CPUAlphaState *env, bool ifetch)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        if (ifetch) {
            if (IPR_EX64(iccsr, EV4_ICCSR, MAP)) {
                return 0b0010;
            }
            return 0;
        }
        return IPR_EX64(abox_ctl, EV4_ABOX_CTL, SPE);
    case IMPLVER_21164:
        return ifetch ? IPR_EX64(icsr, EV5_ICSR, SPE)
                      : IPR_EX64(mcsr, EV5_MCSR, SPE);
    case IMPLVER_21264:
        return ifetch ? IPR_EX64(i_ctl, EV6_I_CTL, SPE)
                      : IPR_EX64(m_ctl, EV6_M_CTL, SPE);
    default:
        g_assert_not_reached();
    }
}

static inline int alpha_pending_ast(CPUAlphaState *env)
{
    if (!alpha_get_asten(env)) {
        return 0;
    }
    return alpha_get_aster(env) & alpha_get_astrr(env) &
           MAKE_64BIT_MASK(0, alpha_cur_mode(env, false) + 1);
}

static inline AddressSpace *alpha_cpu_address_space(CPUState *cs,
                                                    MemTxAttrs attrs)
{
    return cpu_get_address_space(cs, cpu_asidx_from_attrs(cs, attrs));
}

/**
 * alpha_clear_llsc_addr:
 * Clear the lock member for locked loads/stores.
 */
static inline void alpha_clear_llsc_addr(CPUAlphaState *env)
{
    env->llsc_addr = LLSC_ADDR_NONE;
}

/**
 * alpha_set_default_fp_behaviors:
 * Initialize the default float_status behavior.
 */
void alpha_set_default_fp_behaviors(float_status *fpst);

/**
 * alpha_read_cyclecounter:
 * Get the current value of the cycle counter.
 */
uint64_t alpha_read_cyclecounter(CPUAlphaState *env);

/**
 * altmode_tlbmask:
 * Return a mask of AlphaMMUIdxBit values corresponding to all AltMode types.
 */
static inline uint16_t altmode_tlbmask(void)
{
    return AlphaMMUIdxBit_AltMode |
           AlphaMMUIdxBit_AltModeWChk;
}

/**
 * kesu_tlbmask:
 * Return a mask of AlphaMMUIdxBit values corresponding to all regular
 * processor mode types.
 */
static inline uint16_t kesu_tlbmask(void)
{
    return AlphaMMUIdxBit_Kernel |
           AlphaMMUIdxBit_Executive |
           AlphaMMUIdxBit_Supervisor |
           AlphaMMUIdxBit_User |
           AlphaMMUIdxBit_Privileged |
           AlphaMMUIdxBit_PrivilegedVPTE |
           AlphaMMUIdxBit_PrivilegedWChk;
}

static inline uint16_t all_tlbmask(void)
{
    return altmode_tlbmask() | kesu_tlbmask();
}

/*
 * Accessors for GP and FP registers.
 */
#define gpr_offset(i)           offsetof(CPUAlphaState, gpregs[i])
#define fpr_offset(i)           offsetof(CPUAlphaState, fpregs[i])
#define shadow_offset(i)        offsetof(CPUAlphaState, pal_shadow[i])

static inline uint64_t *cpu_gpr_ptr(CPUAlphaState *env, int i)
{
    return (uint64_t *)((uintptr_t)env + gpr_offset(i));
}

static inline uint64_t *cpu_fpr_ptr(CPUAlphaState *env, int i)
{
    return (uint64_t *)((uintptr_t)env + fpr_offset(i));
}

static inline uint64_t *cpu_shadow_ptr(CPUAlphaState *env, int i)
{
    return (uint64_t *)((uintptr_t)env + shadow_offset(i));
}

#endif /* _ALPHA_INTERNALS_H */
