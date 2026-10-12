/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU -- feature test functions.
 */

#ifndef _ALPHA_CPU_FEATURES_H
#define _ALPHA_CPU_FEATURES_H

#include "qemu/bitops.h"

enum {
    IMPLVER_2106x       = 0,    /* EV4, EV45 & LCA45 */
    IMPLVER_21164       = 1,    /* EV5, EV56 & PCA45 */
    IMPLVER_21264       = 2,    /* EV6, EV67 & EV68x */
    IMPLVER_21364       = 3,    /* EV7 & EV79 */
};

enum {
    PROCID_EV3          = 1,    /* EV3 */
    PROCID_EV4          = 2,    /* EV4 (21064) */
    PROCID_LCA4         = 4,    /* LCA4 (21066/21068) */
    PROCID_EV5          = 5,    /* EV5 (21164) */
    PROCID_EV45         = 6,    /* EV4.5 (21064/xxx) */
    PROCID_EV56         = 7,    /* EV5.6 (21164) */
    PROCID_EV6          = 8,    /* EV6 (21264) */
    PROCID_PCA56        = 9,    /* PCA56 (21164PC) */
    PROCID_PCA57        = 10,   /* PCA57 */
    PROCID_EV67         = 11,   /* EV67 (21264A) */
    PROCID_EV68CB       = 12,   /* EV68CB (21264C) */
    PROCID_EV68AL       = 13,   /* EV68AL (21264B) */
    PROCID_EV68CX       = 14,   /* EV68CX (21264D) */
    PROCID_EV7          = 15,   /* EV7 (21364) */
    PROCID_EV79         = 16,   /* EV79 (21364?? */
    PROCID_EV69         = 17,   /* EV69 (21264/EV69A) */
};

/* AMASK definitions. */
enum {
    AMASK_BWX           = BIT(0),
    AMASK_FIX           = BIT(1),
    AMASK_CIX           = BIT(2),
    AMASK_MVI           = BIT(8),
    AMASK_TRAP          = BIT(9),
    AMASK_PREFETCH      = BIT(12),
};

#define FEATURE_TEST(name, bit)                                 \
static inline bool glue(amask_feature_, name)(uint32_t amask)   \
{                                                               \
    return (amask & bit) != 0;                                  \
}

FEATURE_TEST(bwx, AMASK_BWX)
FEATURE_TEST(fix, AMASK_FIX)
FEATURE_TEST(cix, AMASK_CIX)
FEATURE_TEST(mvi, AMASK_MVI)
FEATURE_TEST(trap, AMASK_TRAP)
FEATURE_TEST(prefetch, AMASK_PREFETCH)

/*
 * Forward to the above feature tests given an AlphaCPU pointer.
 */
#define cpu_amask_feature(name, cpu) \
    ({ AlphaCPU *__cpu = (cpu); amask_feature_##name(__cpu->isa_amask); })

#endif /* _ALPHA_CPU_FEATURES_H */
