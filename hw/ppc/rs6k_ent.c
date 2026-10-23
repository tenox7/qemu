/*
 * IBM RS/6000 7006 integrated Ethernet (Intel 82596, MCA id 0x8f98, slot 14)
 *
 * The 82596 keeps its memory structures little endian and masters the
 * Micro Channel through the IOCC TCW table. Its registers are at Micro
 * Channel I/O 0xf0: PORT takes a command in two word writes, low half
 * first, each halfword in the first two byte lanes; any write to 0xf4 is
 * a channel attention; the status word at 0xf8 reads bit 0 set while the
 * chip is not interrupting. It interrupts on level 5 until the CPU has
 * acknowledged every SCB status bit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "hw/net/i82596.h"
#include "hw/ppc/rs6k_iocc.h"
#include "hw/ppc/rs6k_ent.h"

OBJECT_DECLARE_SIMPLE_TYPE(Rs6kEntState, RS6K_ENT)

#define ENT_SLOT        14
#define ENT_ID          0x8f98
#define ENT_IO_ADDR     0xf0
#define ENT_IO_SIZE     0x10
#define ENT_PORT        0x0
#define ENT_CA          0x4
#define ENT_STATUS      0x8
#define ENT_NO_INTR     0x1
#define ENT_LEVEL       5
#define SCB_STAT_ACK    0xf000

struct Rs6kEntState {
    SysBusDevice parent_obj;

    Rs6kIOCCState *iocc;
    I82596State chip;
    MemoryRegion regs;
    qemu_irq irq;
    uint32_t port;
    bool port_high;
};

static bool ent_interrupting(Rs6kEntState *s)
{
    return s->chip.scb_status & SCB_STAT_ACK;
}

static void ent_update_irq(Rs6kEntState *s)
{
    qemu_set_irq(s->irq, ent_interrupting(s));
}

static void ent_chip_irq(void *opaque, int n, int level)
{
    ent_update_irq(opaque);
}

static uint64_t ent_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kEntState *s = opaque;

    if (addr == ENT_STATUS) {
        return ent_interrupting(s) ? 0 : ENT_NO_INTR;
    }
    qemu_log_mask(LOG_UNIMP, "rs6k-ent: read 0x%" HWADDR_PRIx "\n", addr);
    return 0;
}

static void ent_port_write(Rs6kEntState *s, uint16_t half)
{
    s->port_high = !s->port_high;
    if (s->port_high) {
        s->port = half;
        return;
    }
    s->port |= (uint32_t)half << 16;
    i82596_ioport_writew(&s->chip, s->port & 0xf, s->port & ~0xf);
}

static void ent_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rs6kEntState *s = opaque;

    switch (addr) {
    case ENT_PORT:
        ent_port_write(s, bswap16(val >> 16));
        break;
    case ENT_CA:
        i82596_ioport_writew(&s->chip, PORT_CA, 0);
        break;
    case ENT_STATUS:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rs6k-ent: write 0x%" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", addr, val);
    }
    ent_update_irq(s);
}

static const MemoryRegionOps ent_ops = {
    .read = ent_read,
    .write = ent_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static NetClientInfo ent_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = i82596_can_receive,
    .receive = i82596_receive,
    .receive_iov = i82596_receive_iov,
    .poll = i82596_poll,
    .link_status_changed = i82596_set_link_status,
};

const uint8_t *rs6k_ent_macaddr(DeviceState *dev)
{
    return RS6K_ENT(dev)->chip.conf.macaddr.a;
}

static void rs6k_ent_realize(DeviceState *dev, Error **errp)
{
    Rs6kEntState *s = RS6K_ENT(dev);

    if (!s->iocc) {
        error_setg(errp, "iocc link not set");
        return;
    }
    s->irq = qdev_get_gpio_in(DEVICE(s->iocc), ENT_LEVEL);
    qdev_init_gpio_in(dev, ent_chip_irq, 1);
    s->chip.irq = qdev_get_gpio_in(dev, 0);
    s->chip.dma_as = &s->iocc->dma_as;
    s->chip.little_endian = true;
    i82596_common_init(dev, &s->chip, &ent_net_info);

    memory_region_init_io(&s->regs, OBJECT(dev), &ent_ops, s, "ent.regs",
                          ENT_IO_SIZE);
    memory_region_add_subregion(&s->iocc->bus_io, ENT_IO_ADDR, &s->regs);
    rs6k_iocc_set_slot(s->iocc, ENT_SLOT, ENT_ID, NULL, s);
}

static void rs6k_ent_reset_hold(Object *obj, ResetType type)
{
    Rs6kEntState *s = RS6K_ENT(obj);

    s->port_high = false;
    i82596_h_reset(&s->chip);
}

static const Property rs6k_ent_properties[] = {
    DEFINE_PROP_LINK("iocc", Rs6kEntState, iocc, TYPE_RS6K_IOCC,
                     Rs6kIOCCState *),
    DEFINE_NIC_PROPERTIES(Rs6kEntState, chip.conf),
};

static void rs6k_ent_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = rs6k_ent_reset_hold;
    dc->realize = rs6k_ent_realize;
    device_class_set_props(dc, rs6k_ent_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo rs6k_ent_types[] = {
    {
        .name = TYPE_RS6K_ENT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Rs6kEntState),
        .class_init = rs6k_ent_class_init,
    },
};

DEFINE_TYPES(rs6k_ent_types)
