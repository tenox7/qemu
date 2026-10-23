/*
 * IBM RS/6000 7006 integrated Ethernet
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_RS6K_ENT_H
#define HW_PPC_RS6K_ENT_H

#include "hw/core/qdev.h"

#define TYPE_RS6K_ENT "rs6k-ent"

const uint8_t *rs6k_ent_macaddr(DeviceState *dev);

#endif
