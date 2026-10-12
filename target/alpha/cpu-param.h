/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Alpha CPU parameters for QEMU.
 */

#ifndef _ALPHA_CPU_PARAM_H
#define _ALPHA_CPU_PARAM_H

#define TARGET_PAGE_BITS                13      /* 8k pages */

#define TARGET_PHYS_ADDR_SPACE_BITS     48
#define TARGET_VIRT_ADDR_SPACE_BITS     52

/* Alpha processors have a weak memory model. */
#define TCG_GUEST_DEFAULT_MO            (0)

#endif /* _ALPHA_CPU_PARAM_H */
