/* SPDX-License-Identifier: LGPL-2.0-or-later */
/*
 * QEMU Alpha CPU QOM header (target agnostic)
 */

#ifndef _ALPHA_CPU_QOM_H
#define _ALPHA_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_ALPHA_CPU "alpha-cpu"

OBJECT_DECLARE_CPU_TYPE(AlphaCPU, AlphaCPUClass, ALPHA_CPU)

#define ALPHA_CPU_TYPE_SUFFIX "-" TYPE_ALPHA_CPU
#define ALPHA_CPU_TYPE_NAME(model) model ALPHA_CPU_TYPE_SUFFIX

#endif /* _ALPHA_CPU_QOM_H */
