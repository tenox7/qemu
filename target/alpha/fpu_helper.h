/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Helpers for floating point instructions.
 */

#ifndef _ALPHA_FPU_HELPER_H
#define _ALPHA_FPU_HELPER_H

#include "qemu/bitops.h"
#include "fpu/softfloat.h"

typedef enum AlphaFPRounding {
    FPCR_DYN_RM_CHOPPED,    /* FE_TOWARDZERO */
    FPCR_DYN_RM_MINUS,      /* FE_DOWNWARD */
    FPCR_DYN_RM_NORMAL,     /* FE_TONEAREST */
    FPCR_DYN_RM_PLUS,       /* FE_UPWARD */
} AlphaFPRounding;

/*
 * Convert a value from S-memory format to register format.
 */
static inline uint64_t ieee_single_to_register(float32 val)
{
    bool sign = extract32(val, 31, 1);
    int exp = extract32(val, 23, 8);
    uint32_t frac = extract32(val, 0, 23);
    uint64_t ret;

    /*
     * 2.2.6.1 S_Floating
     *
     * The S_floating load instruction reorders bits on the way in from
     * memory, expanding the exponent from 8 to 11 bits, and sets the
     * low-order fraction bits to zero. This produces in the register
     * an equivalent T_floating number, suitable for either S_floating
     * or T_floating operations.
     */
    if (unlikely(exp == 0xff)) {
        /* Inf or NAN. */
        exp = 0x7ff;
    } else if (exp != 0) {
        /* Zero or Denormalized operand. */
        exp = exp + 1023 - 127;
    }

    ret  = (uint64_t)sign << 63;
    ret |= (uint64_t)exp << 52;
    ret |= (uint64_t)frac << 29;
    return ret;
}

/*
 * Convert a value from register format to S-memory format.
 */
static inline float32 ieee_register_to_single(uint64_t val)
{
    uint64_t ret;

    /* No denormalization required (includes Inf, NaN). */
    ret  = extract64(val, 62, 2) << 30;
    ret |= extract64(val, 29, 30);
    return ret;
}

#endif /* _ALPHA_FPU_HELPER_H */
