#ifndef _ALPHA_IPREGS_H
#define _ALPHA_IPREGS_H

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "cpu.h"

/*
 * Internal processor register type field bits.
 */
enum {
    /** Flag: reads produce resetvalue; writes ignored. */
    ALPHA_IP_CONST              = BIT(0),

    /**
     * Flag: TB should not be ended after a write to this register
     * (the default is that the TB ends after register writes).
     */
    ALPHA_IP_SUPPRESS_TB_END    = BIT(1),

    /**
     * Flag: Register is an alias view of some underlying state which is also
     * visible via another register, and that the other register is handling
     * migration and reset; registers marked ALPHA_IP_ALIAS will not be
     * migrated.
     */
    ALPHA_IP_ALIAS              = BIT(2),

    /**
     * Flag: Register does I/O and therefore its accesses need to be marked
     * with translator_io_start() and also end the TB. In particular,
     * registers which implement clocks or timers require this.
     */
    ALPHA_IP_IO                 = BIT(3),

    /**
     * Flag: Register has no underlying state and does not support raw access
     * for state saving/loading.
     */
    ALPHA_IP_NO_RAW             = BIT(4),

    /**
     * Flag: The read or write hook might raise an exception; the generated
     * code will synchronize the CPU state before calling the hook so that it
     * is safe for the hook to call raise_exception().
     */
    ALPHA_IP_RAISES_EXC         = BIT(5),

    /**
     * Flag: no change to processor state: writes ignored, reads ignored.
     */
    ALPHA_IP_NOP                = BIT(6),
};

/**
 * IPAccessRights: Access rights.
 */
typedef enum IPAccessRights {
    IPR_R = 0x2,
    IPR_W = 0x1,

    IPR_RW = IPR_R | IPR_W,
} IPAccessRights;

/**
 * AlphaIPRegInfo: Interface for defining internal processor registers.
 *
 * Registers are defined in tables of alpha_ip_reginfo structs
 * which are passed to define_alpha_ip_regs().
 */
typedef struct AlphaIPRegInfo AlphaIPRegInfo;

/**
 * AlphaIPRegInfo: Definition of an Alpha internal processor register.
 */
struct AlphaIPRegInfo {
    /** Name of register (useful mainly for debugging, need not be unique) */
    const char *name;

    /** Location of register. */
    uint16_t ipr;

    /** Register type: ALPHA_IP_* bits/values */
    int type;

    /** Access rights: IPR_[RW] */
    IPAccessRights access;

    /**
     * The opaque pointer passed to "define_alpha_ip_regs_with_opaque()" when
     * this register was defined: can be used to hand data through to the
     * register read/write functions, since they are passed a pointer to a
     * "AlphaIPRegInfo" structure.
     */
    void *opaque;

    /**
     * Value of this register, if it is "ALPHA_IP_CONST". Otherwise, if
     * fieldoffset is non-zero, the reset value of the register.
     */
    uint64_t resetvalue;

    /**
     * Offset of the field in "CPUAlphaState" for this register.
     *
     * This is not needed if both readfn and writefn are specified.
     */
    ptrdiff_t fieldoffset; /* offsetof(CPUAlphaState, field) */

    /**
     * Function for handling reads of this register. If NULL, then reads
     * will be done by loading from the offset into CPUAlphaState specified
     * by fieldoffset.
     */
    uint64_t (*readfn)(CPUAlphaState *env, const AlphaIPRegInfo *opaque);

    /**
     * Function for handling writes of this register. If NULL, then writes
     * will be done by writing to the offset into CPUAlphaState specified
     * by fieldoffset.
     */
    void (*writefn)(CPUAlphaState *env, const AlphaIPRegInfo *opaque,
                    uint64_t value);

    /**
     * Function for doing a "raw" read; this only needs to be provided if
     * there is also a readfn and it has side effects (for instance
     * clear-on-read semantics).
     */
    uint64_t (*raw_readfn)(CPUAlphaState *env, const AlphaIPRegInfo *opaque);

    /**
     * Function for doing a "raw" write. This only needs to be provided if
     * there is also a writefn and it masks out "unwritable" bits or has
     * write-one-to-clear semantics or similar behaviour.
     */
    void (*raw_writefn)(CPUAlphaState *env, const AlphaIPRegInfo *opaque,
                        uint64_t value);

    /**
     * Function for resetting the register. If NULL, then reset will be done
     * by writing resetvalue to the field specified in fieldoffset. If
     * fieldoffset is 0, then no reset will be done.
     */
    void (*resetfn)(CPUAlphaState *env, const AlphaIPRegInfo *opaque);
};

/*
 * Macros which are lvalues for the field in CPUAlphaState for the
 * AlphaIPRegInfo *ri.
 */
#define IPREG_FIELD(env, ri)                                          \
    (*(uint64_t *)((uintptr_t)(env) + (ri)->fieldoffset))

void define_one_alpha_ipreg_with_opaque(AlphaCPU *cpu,
                                        const AlphaIPRegInfo *reg,
                                        void *opaque);

static inline void define_one_alpha_ipreg(AlphaCPU *cpu,
                                           const AlphaIPRegInfo *regs)
{
    define_one_alpha_ipreg_with_opaque(cpu, regs, NULL);
}

void define_alpha_ipregs_with_opaque_len(AlphaCPU *cpu,
                                         const AlphaIPRegInfo *regs,
                                         void *opaque, size_t len);

#define define_alpha_ipregs_with_opaque(CPU, REGS, OPAQUE)              \
    do {                                                                \
        QEMU_BUILD_BUG_ON(ARRAY_SIZE(REGS) == 0);                       \
        define_alpha_ipregs_with_opaque_len(CPU, REGS, OPAQUE,          \
                                            ARRAY_SIZE(REGS));          \
    } while (0)

#define define_alpha_ipregs(CPU, REGS) \
    define_alpha_ipregs_with_opaque(CPU, REGS, NULL)

const AlphaIPRegInfo *get_alpha_ipreg_info(GHashTable *cpregs, uint16_t iprn);

/* Write function that can be used to implement write-ignored (WI) behavior. */
void alpha_ip_write_ignore(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                           uint64_t value);

/* Read function that can be used for "read-as-zero" (RAZ) behavior. */
uint64_t alpha_ip_read_zero(CPUAlphaState *env, const AlphaIPRegInfo *ri);

/* Reset function that does nothing. */
void alpha_ip_reset_ignore(CPUAlphaState *env, const AlphaIPRegInfo *opaque);

/* Raw read of a internal processor register (as needed for migration, etc) */
uint64_t read_raw_ipreg(CPUAlphaState *env, const AlphaIPRegInfo *ri);

/* Raw write of a internal processor register (as needed for migration, etc) */
void write_raw_ipreg(CPUAlphaState *env, const AlphaIPRegInfo *ri,
                     uint64_t value);

static inline bool ipreg_access_ok(const AlphaIPRegInfo *ri, int isread)
{
    return (ri->access >> (isread)) & 0b01;
}

#endif /* _ALPHA_IPREGS_H */
