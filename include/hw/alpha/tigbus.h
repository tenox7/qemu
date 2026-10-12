/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ALPHA_TIGBUS_H
#define _ALPHA_TIGBUS_H

#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "qom/object.h"

#define TYPE_TIG_BUS "tig-bus"
OBJECT_DECLARE_SIMPLE_TYPE(TigBus, TIG_BUS)

struct TigBus {
    /*< private >*/
    BusState parent_obj;

    /*< public >*/
    AddressSpace downstream_as;
    MemoryRegion downstream;
};

static inline MemoryRegion *tigbus_address_space(TigBus *tb)
{
    return &tb->downstream;
}

#define TYPE_TIG_BUS_DEVICE "tig-bus-device"
OBJECT_DECLARE_SIMPLE_TYPE(TigBusDevice, TIG_BUS_DEVICE)

struct TigBusDevice {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    uint8_t cs_index;
};

TigBusDevice *tigbus_get_cs(TigBus *bus, uint8_t cs_index);
TigBusDevice *tigbus_create_device(TigBus *bus, const char *name);
TigBus *tigbus_from_device(TigBusDevice *dev);
TigBus *tigbus_create_bus(DeviceState *parent, const char *name);
bool tigbus_realize_and_unref(TigBusDevice *dev, TigBus *bus, Error **errp);

#endif /* _ALPHA_TIGBUS_H */
