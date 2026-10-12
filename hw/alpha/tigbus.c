/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/alpha/tigbus.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "trace.h"

static const Property tigbus_device_properties[] = {
    DEFINE_PROP_UINT8("cs", TigBusDevice, cs_index, -1),
};

static void tigbus_device_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->bus_type = TYPE_TIG_BUS;
    device_class_set_props(dc, tigbus_device_properties);
}

static const TypeInfo tigbus_device_info = {
    .name          = TYPE_TIG_BUS_DEVICE,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(TigBusDevice),
    .class_init    = tigbus_device_class_init,
    .abstract      = true,
};

static MemTxResult tigbus_unassigned_write(void *opaque, hwaddr addr,
                                           uint64_t val, unsigned size,
                                           MemTxAttrs attrs)
{
    trace_tigbus_unassigned_write(addr, val, size);
    return MEMTX_DECODE_ERROR;
}

static MemTxResult tigbus_unassigned_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    trace_tigbus_unassigned_read(addr, size);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps tigbus_unassigned_ops = {
    .read_with_attrs = tigbus_unassigned_read,
    .write_with_attrs = tigbus_unassigned_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static TigBus *tigbus_find(void)
{
    /* Returns NULL unless there is exactly one nubus device */
    return TIG_BUS(object_resolve_path_type("", TYPE_TIG_BUS, NULL));
}

static void tigbus_realize(BusState *bus, Error **errp)
{
    TigBus *tb = TIG_BUS(bus);

    if (!tigbus_find()) {
        error_setg(errp, "only one %s device is permitted", TYPE_TIG_BUS);
        return;
    }

    address_space_init(&tb->downstream_as, &tb->downstream, "tigbus");
}

static void tigbus_unrealize(BusState *bus)
{
    TigBus *tb = TIG_BUS(bus);

    address_space_destroy(&tb->downstream_as);
}

static void tigbus_init(Object *obj)
{
    TigBus *tb = TIG_BUS(obj);

    memory_region_init_io(&tb->downstream, obj, &tigbus_unassigned_ops,
                          tb, "tigbus", 16 * MiB);
}

TigBusDevice *tigbus_get_cs(TigBus *bus, uint8_t cs_index)
{
    BusState *b = BUS(bus);
    BusChild *kid;

    QTAILQ_FOREACH(kid, &b->children, sibling) {
        TigBusDevice *kid_tbd = TIG_BUS_DEVICE(kid->child);
        if (kid_tbd->cs_index == cs_index) {
            return kid_tbd;
        }
    }

    return NULL;
}

bool tigbus_realize_and_unref(TigBusDevice *dev, TigBus *bus, Error **errp)
{
    return qdev_realize_and_unref(DEVICE(dev), &bus->parent_obj, errp);
}

TigBusDevice *tigbus_create_device(TigBus *bus, const char *name)
{
    TigBusDevice *dev = TIG_BUS_DEVICE(qdev_new(name));

    tigbus_realize_and_unref(dev, bus, &error_fatal);
    return dev;
}

TigBus *tigbus_from_device(TigBusDevice *dev)
{
    return TIG_BUS(qdev_get_parent_bus(DEVICE(dev)));
}

TigBus *tigbus_create_bus(DeviceState *parent, const char *name)
{
    BusState *bus = qbus_new(TYPE_TIG_BUS, DEVICE(parent), name);

    return TIG_BUS(bus);
}

static bool tigbus_check_address(BusState *b, DeviceState *dev, Error **errp)
{
    TigBusDevice *tbd = TIG_BUS_DEVICE(dev);
    TigBusDevice *cs = tigbus_get_cs(TIG_BUS(b), tbd->cs_index);

    if (cs) {
        error_setg(errp, "CS index '0x%x' already in use by a %s device",
                   tbd->cs_index, object_get_typename(OBJECT(cs)));
        return false;
    }
    return true;
}

static void tigbus_class_init(ObjectClass *klass, const void *data)
{
    BusClass *bc = BUS_CLASS(klass);

    bc->realize = tigbus_realize;
    bc->unrealize = tigbus_unrealize;
    bc->check_address = tigbus_check_address;
}

static const TypeInfo tigbus_bus_info = {
    .name          = TYPE_TIG_BUS,
    .parent        = TYPE_BUS,
    .instance_size = sizeof(TigBus),
    .instance_init = tigbus_init,
    .class_init    = tigbus_class_init,
};

static void tigbus_register_types(void)
{
    type_register_static(&tigbus_bus_info);
    type_register_static(&tigbus_device_info);
}

type_init(tigbus_register_types)
