/* SPDX-License-Identifier: MIT */

#ifndef _ALPHA_CPU_BITS_H
#define _ALPHA_CPU_BITS_H

#include "qemu/bitops.h"

#define ALPHA_BIT_NR(bit)       (bit)
#define ALPHA_BIT(bit)          (UINT64_C(1) << (bit))
#define ALPHA_BIT32_NR(bit)     ((bit) - 32)
#define ALPHA_BIT32(bit)        (UINT32_C(1) << ALPHA_BIT32_NR(bit))
#define ALPHA_BITMASK(be, bs)   MAKE_64BIT_MASK((bs), ((be) - (bs) + 1))
#define ALPHA_BITMASK_SHIFT(val, be, bs)                                \
    (extract64(val, (bs), (be) - (bs) + 1))
#define ALPHA_BITMASK_SHIFT_SEXT(op, be, bs)                            \
    (sextract64(op, (bs), (be) - (bs) + 1))

#define MERGE_VALUE(dst, src, mask)     (((dst) & ~(mask)) | ((src) & (mask)))
#define MERGE_FIELD(v, data, mask)      ((v) = MERGE_VALUE(v, data, mask))

#define SET_FIELD(storage, reg, field, val)                             \
    (storage) = FIELD_DP64(storage, reg, field, val)

#endif /* _ALPHA_CPU_BITS_H */
