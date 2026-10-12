
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD Am29F016 (16-Megabit (2,097,152 x 8-Bit) flash emulation model.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/block/flash.h"
#include "hw/block/block.h"
#include "hw/alpha/tigbus.h"
#include "hw/alpha/am29f016.h"
#include "hw/core/qdev-properties.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "system/blockdev.h"
#include "trace.h"

struct Am29F016State {
    /*< private >*/
    TigBusDevice parent_obj;

    /*< public >*/
    DeviceState *pflash;
    BlockBackend *blk;
    char *name;
};

static const Property pflash_am29f016_properties[] = {
    DEFINE_PROP_DRIVE("drive", Am29F016State, blk),
    DEFINE_PROP_STRING("name", Am29F016State, name),
};

static void pflash_am29f016_realize(DeviceState *dev, Error **errp)
{
    Am29F016State *s = PFLASH_AM29F016(dev);
    TigBusDevice *tbd = TIG_BUS_DEVICE(dev);
    SysBusDevice *sbd;
    DeviceState *pflash;
    MemoryRegion *iomem;
    uint32_t size = 2 * MiB;
    uint32_t sector_len = 64 * KiB;

    if (unlikely(s->name == NULL)) {
        error_setg(errp, "%s: attribute \"name\" not specified", __func__);
        return;
    }

    pflash = qdev_new(TYPE_PFLASH_CFI02);
    assert(QEMU_IS_ALIGNED(size, sector_len));
    if (s->blk) {
        blk_detach_dev(s->blk, dev);
        qdev_prop_set_drive_err(pflash, "drive", s->blk, &error_fatal);
    }
    qdev_prop_set_uint32(pflash, "num-blocks", size / sector_len);
    qdev_prop_set_uint32(pflash, "sector-length", sector_len);
    qdev_prop_set_uint8(pflash, "width", 1);
    qdev_prop_set_uint8(pflash, "mappings", 1);
    qdev_prop_set_uint8(pflash, "big-endian", false);
    qdev_prop_set_uint16(pflash, "id0", 0x0001);
    qdev_prop_set_uint16(pflash, "id1", 0x00ad);
    qdev_prop_set_uint16(pflash, "id2", 0x0000);
    qdev_prop_set_uint16(pflash, "id3", 0x0000);
    qdev_prop_set_uint16(pflash, "unlock-addr0", 0x5555);
    qdev_prop_set_uint16(pflash, "unlock-addr1", 0x2aaa);
    qdev_prop_set_string(pflash, "name", s->name);
    object_property_add_child(OBJECT(s), "pflash", OBJECT(pflash));

    sbd = SYS_BUS_DEVICE(pflash);
    sysbus_realize_and_unref(sbd, &error_fatal);

    iomem = tigbus_address_space(tigbus_from_device(tbd));
    memory_region_add_subregion(iomem, 0, sysbus_mmio_get_region(sbd, 0));
}

static void pflash_am29f016_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pflash_am29f016_realize;
    device_class_set_props(dc, pflash_am29f016_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo pflash_am29f016_info = {
    .name          = TYPE_PFLASH_AM29F016,
    .parent        = TYPE_TIG_BUS_DEVICE,
    .instance_size = sizeof(Am29F016State),
    .class_init    = pflash_am29f016_class_init,
};

static void pflash_am29f016_register_types(void)
{
    type_register_static(&pflash_am29f016_info);
}

type_init(pflash_am29f016_register_types)
