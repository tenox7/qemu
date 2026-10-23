/*
 * IBM RS/6000 7006 mouse port
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_RS6K_MSE_H
#define HW_PPC_RS6K_MSE_H

#include "hw/core/qdev.h"

#define TYPE_RS6K_MSE "rs6k-mse"

void rs6k_mse_set_reset(DeviceState *dev, bool reset);
MemoryRegion *rs6k_mse_io(DeviceState *dev);

#endif
