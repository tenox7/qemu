/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU -- error syndrome functions and types
 */

#ifndef _ALPHA_SYNDROME_H
#define _ALPHA_SYNDROME_H

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "hw/core/registerfields.h"

/*
 * Alpha exception internal fields.
 */
enum {
    EC_UNCATEGORIZED    = 0x00,
    EC_CALLPAL          = 0x01,
    EC_MMUABORT         = 0x02,
    EC_ARITH            = 0x03,
};

FIELD(SYN, ISS, 0, 24);
FIELD(SYN, ISV, 24, 1);
FIELD(SYN, IL, 25, 1);
FIELD(SYN, EC, 26, 6);

#define DEFINE_SYN_HELPER(name, field)                              \
    static inline uint32_t glue(syn_get_, name)(uint32_t syn)       \
    {                                                               \
        return FIELD_EX32(syn, SYN, field);                         \
    }                                                               \
    static inline uint32_t glue(syn_set_, name)(uint32_t syn,       \
                                                uint32_t name)      \
    {                                                               \
        return FIELD_DP32(syn, SYN, field, name);                   \
    }
DEFINE_SYN_HELPER(iss, ISS)
DEFINE_SYN_HELPER(isv, ISV)
DEFINE_SYN_HELPER(il, IL)
DEFINE_SYN_HELPER(ec, EC)
#undef DEFINE_SYN_HELPER

/*
 * Utility functions for constructing various kinds of syndrome values.
 */
FIELD(ARITH_ISS, SWC, 0, 1)
FIELD(ARITH_ISS, INV, 1, 1)
FIELD(ARITH_ISS, DZE, 2, 1)
FIELD(ARITH_ISS, FOV, 3, 1)
FIELD(ARITH_ISS, UNF, 4, 1)
FIELD(ARITH_ISS, INE, 5, 1)
FIELD(ARITH_ISS, IOV, 6, 1)
FIELD(ARITH_ISS, INT, 7, 1)
FIELD(ARITH_ISS, SET_INV, 8, 1)
FIELD(ARITH_ISS, SET_DZE, 9, 1)
FIELD(ARITH_ISS, SET_FOV, 10, 1)
FIELD(ARITH_ISS, SET_UNF, 11, 1)
FIELD(ARITH_ISS, SET_INE, 12, 1)
FIELD(ARITH_ISS, SET_IOV, 13, 1)
FIELD(MMU_ISS, WR, 0, 1)
FIELD(MMU_ISS, ACV, 1, 1)
FIELD(MMU_ISS, FOR, 2, 1)
FIELD(MMU_ISS, FOW, 3, 1)
FIELD(MMU_ISS, IVA, 4, 1)
FIELD(MMU_ISS, OVFL, 5, 1)

static inline uint32_t syn_uncategorized(void)
{
    return (EC_UNCATEGORIZED << R_SYN_EC_SHIFT) | R_SYN_IL_MASK;
}

static inline uint32_t syn_mmu_abort(uint32_t imm24)
{
    return (EC_MMUABORT << R_SYN_EC_SHIFT) | R_SYN_IL_MASK | R_SYN_ISV_MASK |
           (imm24 & 0x00ffffff);
}

static inline uint32_t syn_call_pal(uint32_t imm24)
{
    return (EC_MMUABORT << R_SYN_EC_SHIFT) | R_SYN_IL_MASK | R_SYN_ISV_MASK |
           (imm24 & 0x00ffffff);
}

static inline uint32_t syn_arith(uint32_t imm24)
{
    return (EC_ARITH << R_SYN_EC_SHIFT) | R_SYN_IL_MASK | R_SYN_ISV_MASK |
           (imm24 & 0x00ffffff);
}

#endif /* _ALPHA_SYNDROME_H */
