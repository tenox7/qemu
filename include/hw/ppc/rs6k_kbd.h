/*
 * IBM RS/6000 keyboard/tablet/speaker adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_RS6K_KBD_H
#define HW_PPC_RS6K_KBD_H

#include "hw/core/qdev.h"

#define TYPE_RS6K_KBD "rs6k-kbd"

MemoryRegion *rs6k_kbd_io(DeviceState *dev);

#endif
