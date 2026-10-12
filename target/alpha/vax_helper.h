/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2003-2017, Robert M Supnik

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   Respectfully dedicated to the great people of the Alpha chip, systems, and
   software development projects; and to the memory of Peter Conklin, of the
   Alpha Program Office.  */

#ifndef _ALPHA_VAX_HELPER_H
#define _ALPHA_VAX_HELPER_H

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "fpu/softfloat.h"

#define UF_V_NM         63
#define UF_NM           BIT_ULL(UF_V_NM)
#define UF_FRND         0x0000008000000000  /* F round */
#define UF_DRND         0x0000000000000080  /* D round */
#define UF_GRND         0x0000000000000400  /* G round */

#define F_V_SIGN        15
#define F_V_EXP         7
#define F_V_FRAC        29
#define F_M_EXP         0xFF
#define F_SIGN          BIT(F_V_SIGN)
#define F_BIAS          0x80
#define F_EXP           (F_M_EXP << F_V_EXP)

#define G_V_SIGN        15
#define G_V_EXP         4
#define G_M_EXP         0x7FF
#define G_SIGN          BIT(G_V_SIGN)
#define G_BIAS          0x400
#define G_EXP           (G_M_EXP << G_V_EXP)

#define FDR_V_SIGN      63
#define FDR_V_EXP       55
#define FDR_M_EXP       0xFF
#define FDR_SIGN        BIT_ULL(FDR_V_SIGN)
#define FDR_EXP         MAKE_64BIT_MASK(55, 8)
#define FDR_FRAC        MAKE_64BIT_MASK(0, 55)
#define FDR_HB          BIT_ULL(FDR_V_EXP)
#define FDR_GUARD       (UF_V_NM - FDR_V_EXP)

#define FPR_V_SIGN      63
#define FPR_V_EXP       52
#define FPR_M_EXP       0x7FF
#define FPR_SIGN        BIT_ULL(FPR_V_SIGN)
#define FPR_EXP         MAKE_64BIT_MASK(52, 11)
#define FPR_FRAC        MAKE_64BIT_MASK(0, 52)
#define FPR_HB          BIT_ULL(FPR_V_EXP)
#define FPR_GUARD       (UF_V_NM - FPR_V_EXP)

#define D_BIAS          0x80

/*
 * Convert a value from F-memory format to register format.
 */
static inline uint64_t vax_single_to_register(uint32_t val)
{
    bool sign = extract64(val, F_V_SIGN, 1);
    int exp = extract64(val, F_V_EXP, 8);
    uint32_t frac = hswap32(val & ~(F_SIGN | F_EXP));
    uint64_t ret;

    if (exp != 0) {
        /* Rebias the exponent. */
        exp = exp + G_BIAS - F_BIAS;
    }

    ret  = (uint64_t)sign << FPR_V_SIGN;
    ret |= (uint64_t)exp << FPR_V_EXP;
    ret |= (uint64_t)frac << F_V_FRAC;
    return ret;
}

/*
 * Convert a value from register format to F-memory format.
 */
static inline uint32_t vax_register_to_single(uint64_t val)
{
    bool sign = extract64(val, FPR_V_SIGN, 1);
    int exp = extract64(val, FPR_V_EXP, 11);
    uint32_t frac = extract64(val, F_V_FRAC, 23);
    uint32_t ret;

    if (exp != 0) {
        /* Rebias the exponent. */
        exp = exp + F_BIAS - G_BIAS;
    }

    ret  = (uint32_t)(sign << F_V_SIGN);
    ret |= (uint32_t)(exp << F_V_EXP) & F_EXP;
    ret |= (uint32_t)(hswap32(frac) & ~(F_SIGN | F_EXP));
    return ret;
}

/*
 * Convert a value from G-memory format to register format.
 */
static inline uint64_t vax_double_to_register(uint32_t val)
{
    return hswap64(val);
}

/*
 * Convert a value from register format to G-memory format.
 */
static inline uint32_t vax_register_to_double(uint64_t val)
{
    return hswap64(val);
}

#endif /* _ALPHA_VAX_HELPER_H */
