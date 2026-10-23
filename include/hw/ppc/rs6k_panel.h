/*
 * IBM RS/6000 7006 operator panel: three-digit display and key mode switch
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_RS6K_PANEL_H
#define HW_PPC_RS6K_PANEL_H

#include "hw/core/qdev.h"

#define TYPE_RS6K_PANEL "rs6k-panel"

#define RS6K_KEY_SECURE     1
#define RS6K_KEY_SERVICE    2
#define RS6K_KEY_NORMAL     3

unsigned rs6k_panel_key(DeviceState *dev);

#endif
