/*
 * IBM POWER GXT150L graphics adapter for the RS/6000 7006
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_RS6K_GXT_H
#define HW_PPC_RS6K_GXT_H

#include "hw/core/qdev.h"

#define TYPE_RS6K_GXT "gxt150l"

/* The interrupt for source is asserted at prio, or withdrawn */
typedef void (*Rs6kGxtPresent)(void *opaque, uint32_t source, uint8_t prio,
                               bool asserted);

void rs6k_gxt_set_presenter(DeviceState *dev, Rs6kGxtPresent fn, void *opaque);
uint32_t rs6k_gxt_cfg_read(DeviceState *dev, hwaddr addr);
void rs6k_gxt_cfg_write(DeviceState *dev, hwaddr addr, uint32_t val);
uint8_t rs6k_gxt_rom_read(DeviceState *dev, hwaddr addr);

#endif
