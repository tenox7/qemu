/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD Am29F016 (16-Megabit (2,097,152 x 8-Bit) flash emulation model.
 */

#ifndef _ALPHA_AM29F016_H
#define _ALPHA_AM29F016_H

#include "qom/object.h"
#include "hw/alpha/tigbus.h"
#include "system/blockdev.h"

#define TYPE_PFLASH_AM29F016 "pflash-am29f016"
OBJECT_DECLARE_SIMPLE_TYPE(Am29F016State, PFLASH_AM29F016)

#endif /* _ALPHA_AM29F016_H */
