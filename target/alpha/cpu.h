/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU
 */

#ifndef _ALPHA_CPU_H
#define _ALPHA_CPU_H

#include "qemu/cpu-float.h"
#include "qemu/timer.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-interrupt.h"
#include "system/memory.h"
#include "exec/page-protection.h"
#include "hw/core/clock.h"
#include "hw/core/registerfields.h"
#include "qom/object.h"
#include "qapi/qapi-types-common.h"
#include "cpu-bits.h"
#include "cpu-features.h"
#include "cpu-ipr.h"
#include "cpu-qom.h"

#ifdef CONFIG_USER_ONLY
#error "Alpha does not support user mode"
#endif

#define CPU_RESOLVING_TYPE      TYPE_ALPHA_CPU

/*
 * Alpha-specific interrupt pending bits.
 */
#define CPU_INTERRUPT_ASTIRQ    CPU_INTERRUPT_TGT_INT_0
#define CPU_INTERRUPT_SIRQ      CPU_INTERRUPT_TGT_INT_1

/*
 * Alpha exception vector definitions.
 *
 * NB: add new EXCP_ defines to the array in alpha_log_exception() too!
 */
enum {
    EXCP_NONE = -1,
    /* External exceptions. */
    EXCP_DTBM_DOUBLE_3,         /* DTB double miss (43-bit VA) */
    EXCP_DTBM_DOUBLE_4,         /* DTB double miss exception (48-bit VA) */
    EXCP_FEN,                   /* Floating-point unavailable exception */
    EXCP_UNALIGNED,             /* Unaligned load/store exception */
    EXCP_DTBM_SINGLE,           /* DTB single miss exception */
    EXCP_DFAULT,                /* DTB fault exception */
    EXCP_OPCDEC,                /* Illegal instruction exception */
    EXCP_IACV,                  /* ITB access violation exception */
    EXCP_MCHK,                  /* Machine check exception */
    EXCP_ITB_MISS,              /* ITB miss exception */
    EXCP_ARITH,                 /* Arithmetic exception */
    EXCP_IRQ,                   /* External interrupt request exception */
    EXCP_MT_FPCR,               /* MT_FPCR exception */
    EXCP_RESET,                 /* Reset/wake exception */
    EXCP_CALL_PAL,              /* CALL_PAL instruction */
    /* Internal exceptions. */
    EXCP_ASTIRQ,                /* AST interrupt assertion */
    EXCP_SIRQ,                  /* Software interrupt assertion */
    EXCP_HW_REI,                /* HW_REI/HW_RET instruction */
    EXCP_LAST,
};

/*
 * Alpha hardware interrupt sources.
 */
enum {
    ALPHA_CPU_INPUT_IRQ0,
    ALPHA_CPU_INPUT_IRQ1,
    ALPHA_CPU_INPUT_IRQ2,
    ALPHA_CPU_INPUT_IRQ3,
    ALPHA_CPU_INPUT_IRQ4,
    ALPHA_CPU_INPUT_IRQ5,
    ALPHA_CPU_INPUT_PWRFAIL,
    ALPHA_CPU_INPUT_MCHK,
    ALPHA_CPU_INPUT_HLT,
    ALPHA_CPU_INPUT_CRR,
    ALPHA_CPU_INPUT_SLR,
    ALPHA_CPU_INPUT_PC0,
    ALPHA_CPU_INPUT_PC1,
    ALPHA_CPU_INPUT_PC2,
    ALPHA_CPU_INPUT_COUNT,
};

/*
 * Alpha memory management unit state.
 */
#define MAX_TLB_ENTRIES         128

typedef struct CPUAlphaTLBEntry {
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t match_mask;
    uint64_t keep_mask;
    uint16_t flags;
    uint8_t asn;
} CPUAlphaTLBEntry;

typedef struct CPUAlphaTLBContext {
    uint8_t last_way;   /* last used way used to allocate TLB in a LRU way */
    CPUAlphaTLBEntry entries[MAX_TLB_ENTRIES];
} CPUAlphaTLBContext;

FIELD(PTE, VALID, 0, 1)
FIELD(PTE, FOR, 1, 1)
FIELD(PTE, FOW, 2, 1)
FIELD(PTE, ASM, 4, 1)
FIELD(PTE, GH, 5, 2)
FIELD(PTE, KRE, 8, 1)
FIELD(PTE, ERE, 9, 1)
FIELD(PTE, SRE, 10, 1)
FIELD(PTE, URE, 11, 1)
FIELD(PTE, KWE, 12, 1)
FIELD(PTE, EWE, 13, 1)
FIELD(PTE, SWE, 14, 1)
FIELD(PTE, UWE, 15, 1)

/**
 * CPUAlphaState:
 *
 * The whole Alpha CPU context.
 */
typedef struct CPUArchState {
    uint64_t gpregs[32];        /* general purpose registers */
    uint64_t fpregs[32];        /* floating point registers */
    uint64_t pc;                /* program counter */
    uint64_t rc;
    bool pal_mode;              /* true if the CPU is in PAL mode */

    /* Cached TBFLAGS state. */
    uint32_t hflags;

    /*
     * Floating point state.
     *
     * We store several fpcr fields separately for convenience.
     */
    uint32_t fpcr;              /* floating point control register */
    uint32_t fpcr_dyn_round;
    bool fpcr_flush_to_zero;
    float_status fp_status;     /* floating point execution context */

    /* Load locked/store conditional state. */
#define LLSC_ADDR_NONE  (-1ULL) /* use -1 to indicate no active lock */
    uint64_t llsc_addr;
    uint64_t llsc_val;

    /* Composite state for the cycle counter. */
    uint64_t cc_ns_then;
    uint64_t cc_ticks_then;
    uint64_t cc_ticks_read;

    /* Information associated with an exception about to be taken. */
    uint64_t excp_stats[EXCP_LAST];
    struct {
        uint32_t syndrome;      /* error syndrome */
        uint64_t vaddress;      /* VA associated with exception, if any */
    } exception;

    /* Internal processor registers. */
    uint64_t pal_shadow[8];
    struct {
        uint64_t aster;
        uint64_t astrr;
        uint64_t abox_ctl;
        uint64_t biu_addr;
        uint64_t biu_stat;
        uint64_t cc;
        uint64_t cc_ctl;
        union {
            uint64_t dc_ctl;
            uint64_t dc_mode;
        };
        union {
            uint64_t altmode;
            uint64_t dtb_altmode;
        };
        uint64_t dtb_cm;
        uint64_t dtb_pte;
        uint64_t dtb_pte_temp;
        uint64_t dtb_asn[2];
        uint64_t dtb_tag[2];
        uint64_t exc_addr;
        uint64_t exc_mask;
        uint64_t exc_sum;
        uint64_t hier;
        union {
            uint64_t i_ctl;
            uint64_t icsr;
            uint64_t iccsr;
        };
        union {
            uint64_t icm;
            uint64_t ier_cm;
            uint64_t ps;
        };
        uint64_t itb_asn;
        uint64_t itb_tag;
        uint64_t itb_pte;
        uint64_t itb_pte_temp;
        uint64_t ipl;
        uint64_t ivptbr;
        union {
            uint64_t m_ctl;
            uint64_t mcsr;
        };
        union {
            uint64_t mm_stat;
            uint64_t mmcsr;
        };
        uint64_t mvptbr;
        uint64_t pal_base;
        union {
            uint64_t pctr_ctl;
            uint64_t pmctr;
        };
        uint64_t pctx;
        uint64_t sier;
        uint64_t sirr;
        uint64_t tb_ctl;
        uint64_t tb_tag;
        uint64_t pmpc;
        uint64_t va;
        uint64_t va_ctl;
        uint64_t temp[32];
    } ipr;

    /* Interrupt request state. */
    uint32_t irq_line_state;

    /* TLB context, only relevant for full system emulation. */
    CPUAlphaTLBContext tlb[2];

    /* Fields up to this point are cleared by a CPU reset. */
    struct {} end_reset_fields;

    /* Fields from here on are preserved across CPU reset. */
    int bfd_mach;
    uint64_t excp_vectors[EXCP_LAST];   /* exception vectors */
    uint64_t hreset_vector;
    uint64_t features;
    uint32_t amask;             /* architectural AMASK value */
    uint32_t implver;           /* implementation version */
} CPUAlphaState;

/* CPU power state. */
typedef enum AlphaCPUPowerState {
    CPU_POWER_ON = 0,
    CPU_POWER_OFF = 1,
} AlphaCPUPowerState;

/**
 * AlphaCPUAlias:
 * @alias: The alias name.
 * @model: The CPU model @alias refers to, that directly resolves into CPU type
 *
 * A mapping entry from CPU @alias to CPU @model.
 */
typedef struct AlphaCPUAlias {
    const char *alias;
    const char *model;
} AlphaCPUAlias;

extern const AlphaCPUAlias alpha_cpu_aliases[];

/**
 * AlphaCPU:
 * @env: #CPUAlphaState
 *
 * An Alpha CPU core.
 */
struct ArchCPU {
    CPUState parent_obj;
    CPUAlphaState env;

    /* Implementation versions and feature sets. */
    int bfd_mach;
    uint64_t chip_id;
    uint64_t proc_id;
    uint64_t isa_implver;
    uint64_t isa_amask;

    /* Target clock frequency. */
    Clock *refclk;
    Clock *sysclk_div;          /* divider for the RPCC clock */
    Clock *sysclk;              /* RPCC clock */

    /* IPR definitions. */
    GHashTable *ipregs;

    /*
     * For marshalling register state between two QEMUs (for migration),
     * we use these arrays.
     */
    uint64_t *ipreg_indexes;
    uint64_t *ipreg_values;
    int32_t ipreg_array_len;

    /*
     * These are used only for migration: incoming data arrives in
     * these fields and is sanity checked in post_load before copying
     * to the working data structures above.
     */
    uint64_t *ipreg_vmstate_indexes;
    uint64_t *ipreg_vmstate_values;
    int32_t ipreg_vmstate_array_len;

    /* Current power state, access guarded by BQL */
    AlphaCPUPowerState power_state;
};

typedef struct AlphaCPUInfo {
    const char *name;
    void (*initfn)(Object *obj);
    void (*class_init)(ObjectClass *oc, const void *data);
} AlphaCPUInfo;

/**
 * AlphaCPUClass:
 * @parent_realize: The parent class' realize handler.
 * @parent_phases: The parent class' reset phase handlers.
 *
 * An Alpha CPU model.
 */
struct AlphaCPUClass {
    CPUClass parent_class;

    const AlphaCPUInfo *info;
    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
    ResettablePhases parent_phases;
};

extern const struct VMStateDescription vmstate_alpha_cpu;

void alpha_cpu_post_init(Object *obj);
void alpha_cpu_dump_state(CPUState *cs, FILE *f, int);
void alpha_cpu_dump_iprs(CPUState *cs);
void alpha_cpu_dump_mmu(CPUState *cs);
int alpha_cpu_gdb_read_register(CPUState *cpu, GByteArray *buf, int reg);
int alpha_cpu_gdb_write_register(CPUState *cpu, uint8_t *buf, int reg);
uint64_t alpha_cpu_get_reg_value(CPUAlphaState *env, int ra);
void alpha_cpu_set_reg_value(CPUAlphaState *env, int ra, uint64_t val);
void alpha_cpu_do_interrupt(CPUState *cpu);
void alpha_cpu_do_system_reset(CPUState *cs);
bool alpha_cpu_exec_interrupt(CPUState *cpu, int interrupt_request);
bool alpha_cpu_exec_halt(CPUState *cs);
void alpha_cpu_set_pc(CPUState *cs, vaddr value);
vaddr alpha_cpu_get_pc(CPUState *cs);
hwaddr alpha_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr);
int alpha_cpu_write_elf64_note(WriteCoreDumpFunction f, CPUState *cs,
                               int cpuid, DumpState *s);
int alpha_cpu_write_elf32_note(WriteCoreDumpFunction f, CPUState *cs,
                               int cpuid, DumpState *s);
bool alpha_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr);
ObjectClass *alpha_cpu_class_by_name(const char *name);
void alpha_cpu_list(void);

void cpu_interrupt_exittb(CPUState *cs);
void cpu_get_tb_cpu_state(CPUAlphaState *env, vaddr *pc,
                          uint64_t *cs_base, uint32_t *flags);
#define cpu_list alpha_cpu_list

/*
 * Alpha floating-point control register definitions.
 */
#define FPCR_BIT_NR(bit)        ALPHA_BIT32_NR(bit)
#define FPCR_BIT(bit)           ALPHA_BIT32(bit)

#define FPCR_SUM                FPCR_BIT(63)
#define FPCR_INED               FPCR_BIT(62)
#define FPCR_UNFD               FPCR_BIT(61)
#define FPCR_UNDZ               FPCR_BIT(60)
#define FPCR_IOV                FPCR_BIT(57)
#define FPCR_INE                FPCR_BIT(56)
#define FPCR_UNF                FPCR_BIT(55)
#define FPCR_OVF                FPCR_BIT(54)
#define FPCR_DZE                FPCR_BIT(53)
#define FPCR_INV                FPCR_BIT(52)
#define FPCR_OVFD               FPCR_BIT(51)
#define FPCR_DZED               FPCR_BIT(50)
#define FPCR_INVD               FPCR_BIT(49)
#define FPCR_DNZ                FPCR_BIT(48)
#define FPCR_DNOD               FPCR_BIT(47)
#define FPCR_DISABLE_MASK       (FPCR_INED | FPCR_UNFD | FPCR_OVFD \
                                 | FPCR_DZED | FPCR_INVD)
#define FPCR_STATUS_MASK        (FPCR_IOV | FPCR_INE | FPCR_UNF \
                                 | FPCR_OVF | FPCR_DZE | FPCR_INV)
#define FPCR_STATUS_SHIFT       FPCR_BIT_NR(52)

#define FPCR_DYN_SHIFT          FPCR_BIT_NR(58)
#define FPCR_DYN_CHOPPED        (0 << FPCR_DYN_SHIFT)
#define FPCR_DYN_MINUS          (1 << FPCR_DYN_SHIFT)
#define FPCR_DYN_NORMAL         (2 << FPCR_DYN_SHIFT)
#define FPCR_DYN_PLUS           (3 << FPCR_DYN_SHIFT)
#define FPCR_DYN_MASK           (3 << FPCR_DYN_SHIFT)
#define FPCR_MASK               (FPCR_DISABLE_MASK | FPCR_STATUS_MASK \
                                 | FPCR_DYN_MASK | FPCR_UNDZ | FPCR_DNZ | FPCR_DNOD)

/**
 * alpha_cpu_get_fpcr: Return the current FPCR value.
 * @env: CPU context
 */
uint64_t alpha_cpu_get_fpcr(CPUAlphaState *env);

/**
 * alpha_cpu_set_fpcr: Write a new FPCR value.
 * @env: CPU context
 * @value: new value
 */
void alpha_cpu_set_fpcr(CPUAlphaState *env, uint64_t value);

/**
 * write_list_to_cpustate
 * @cpu: AlphaCPU
 *
 * For each register listed in the AlphaCPU ipreg_indexes list, write
 * its value from the cpreg_values list into the AlphaCPUState structure.
 * This updates TCG's working data structures from incoming migration state.
 *
 * Returns: true if all register values were updated correctly,
 * false if some register was unknown or could not be written.
 * Note that we do not stop early on failure -- we will attempt
 * writing all registers in the list.
 */
bool write_list_to_cpustate(AlphaCPU *cpu);

/**
 * write_cpustate_to_list:
 * @cpu: AlphaCPU
 *
 * For each register listed in the AlphaCPU ipreg_indexes list, write
 * its value from the AlphaCPUState structure into the ipreg_values list.
 * This is used to copy info from TCG's working data structures
 * for outbound migration.
 *
 * Returns: true if all register values were read correctly,
 * false if some register was unknown or could not be read.
 * Note that we do not stop early on failure -- we will attempt
 * reading all registers in the list.
 */
bool write_cpustate_to_list(AlphaCPU *cpu);

/*
 * Alpha MMU definitions.
 */
typedef enum AlphaMMUIdx {
    /* Core TLBs. */
    AlphaMMUIdx_Kernel,
    AlphaMMUIdx_Executive,
    AlphaMMUIdx_Supervisor,
    AlphaMMUIdx_User,

    /* TLBs with 1:1 mapping to the physical address space. */
    AlphaMMUIdx_PAL,
    AlphaMMUIdx_Physical,

    /* Virtual TLBs used to make handling hw_ld and hw_st less painful. */
    AlphaMMUIdx_Privileged,
    AlphaMMUIdx_PrivilegedVPTE,
    AlphaMMUIdx_PrivilegedWChk,
    AlphaMMUIdx_AltMode,
    AlphaMMUIdx_AltModeWChk,
} AlphaMMUIdx;

#define MMU_USER_IDX 3
QEMU_BUILD_BUG_ON(MMU_USER_IDX != AlphaMMUIdx_User);

/*
 * Bit macros for the core-mmu-index values for each index, for use when
 * calling tlb_flush_by_mmuidx() and friends.
 */
#define MMUIDX_TO_BIT(NAME) \
    AlphaMMUIdxBit_##NAME = 1 << (AlphaMMUIdx_##NAME)

typedef enum AlphaMMUIdxBit {
    MMUIDX_TO_BIT(Kernel),
    MMUIDX_TO_BIT(Executive),
    MMUIDX_TO_BIT(Supervisor),
    MMUIDX_TO_BIT(User),
    MMUIDX_TO_BIT(PAL),
    MMUIDX_TO_BIT(Physical),
    MMUIDX_TO_BIT(Privileged),
    MMUIDX_TO_BIT(PrivilegedVPTE),
    MMUIDX_TO_BIT(PrivilegedWChk),
    MMUIDX_TO_BIT(AltMode),
    MMUIDX_TO_BIT(AltModeWChk),
} AlphaMMUIdxBit;

#undef MMUIDX_TO_BIT



/* Bit definitions for PC */
FIELD(PC, PAL_MODE, 0, 1)
FIELD(PC, VALUE, 2, 62)

/*
 * Translation block flags.
 *
 * Unless otherwise noted, these bits are cached in env->hflags.
 */
FIELD(TBFLAG, IMMU_IDX, 0, 4)
FIELD(TBFLAG, DMMU_IDX, 4, 4)
FIELD(TBFLAG, FEN, 8, 1)
FIELD(TBFLAG, SDE, 9, 1)
FIELD(TBFLAG, HWE, 10, 1)
FIELD(TBFLAG, PAL_MODE, 11, 1)

/*
 * Helpers for using the above.
 */
#define EX_TBFLAG(IN, WHICH)        FIELD_EX32(IN, TBFLAG, WHICH)
#define DP_TBFLAG(DST, WHICH, VAL)  (DST = FIELD_DP32(DST, TBFLAG, WHICH, VAL))

/**
 * alpha_emulate_srom_reset: Emulate SROM CPU reset handling
 * @cpu: CPU (which must have been freshly reset)
 */
void alpha_emulate_srom_reset(CPUState *cs);

/**
 * alpha_rebuild_hflags:
 * Rebuild the cached TBFLAGS for arbitrary changed processor state.
 */
void alpha_rebuild_hflags(CPUAlphaState *env);

/**
 * alpha_is_pal:
 * Returns true if we're in PAL mode.
 */
static inline bool alpha_is_pal(CPUAlphaState *env)
{
    return env->pal_mode;
}

/**
 * alpha_is_privileged:
 * Returns true if we're in PAL mode or kernel mode.
 */
static inline bool alpha_is_privileged(AlphaMMUIdx mmu_idx)
{
    switch (mmu_idx) {
    case AlphaMMUIdx_PAL:
    case AlphaMMUIdx_Kernel:
        return true;
    default:
        return false;
    }
}

#endif /* _ALPHA_CPU_H */
