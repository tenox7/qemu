/*
 * IBM RS/6000 7006 standard I/O planar ("Rainbow" SIO, MCA id 0xf6fe)
 *
 * Two 16550 serial ports at I/O 0x30 and 0x38 with an 8 MHz clock share
 * interrupt level 2; reading 0x40 shows which port interrupts (bit n for
 * port n). A port can feed its transmitter by third-party DMA: writing 0x40
 * sets the arbitration level and bit n of 0x41 enables DMA for port n.
 * The 82077 floppy controller at 0x60 interrupts on level 6 and does DMA on
 * arbitration level 0, through the ISA DMA interface of this device. The
 * keyboard port at 0x50, the mouse port at 0x48 and the tablet port, a
 * 16550 at 0x70 with no tablet attached, share interrupt level 1; bit 7 of
 * POS register 2 resets the mouse port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/char/serial.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/isa/isa.h"
#include "system/reset.h"
#include "hw/ppc/rs6k_iocc.h"
#include "hw/ppc/rs6k_sio.h"
#include "hw/ppc/rs6k_kbd.h"
#include "hw/ppc/rs6k_mse.h"
#include "hw/core/or-irq.h"

OBJECT_DECLARE_SIMPLE_TYPE(Rs6kSIOState, RS6K_SIO)

#define SIO_SLOT        15
#define SIO_ID          0xf6fe
#define SIO_UART_LEVEL  2
#define SIO_DMA_ADDR    0x40
#define SIO_FDC_ADDR    0x60
#define SIO_FDC_LEVEL   6
#define SIO_FDC_ARB     0
#define SIO_KBD_ADDR    0x50
#define SIO_KBD_LEVEL   1
#define SIO_MSE_ADDR    0x48
#define SIO_TABLET_ADDR 0x70
#define SIO_POS_RESET   2
#define SIO_RESET_MSE   0x80
#define SIO_UARTS       2
#define UART_LSR_THRE   0x20

static const hwaddr uart_addr[SIO_UARTS] = { 0x30, 0x38 };

struct Rs6kSIOState {
    DeviceState parent_obj;

    Rs6kIOCCState *iocc;
    SerialState uart[SIO_UARTS];
    uint8_t uart_irq;
    qemu_irq irq;
    MemoryRegion dma_io;
    QEMUTimer *dma_timer;
    uint8_t dma_arb;
    uint8_t dma_ctl;
    DeviceState *fdc;
    IsaDmaTransferHandler fdc_handler;
    void *fdc_opaque;
    bool fdc_dreq;
    QEMUTimer *fdc_timer;
    DeviceState *kbd;
    DeviceState *mse;
    OrIRQState kbd_irq;
    SerialState tablet;
};

static void sio_uart_irq(void *opaque, int n, int level)
{
    Rs6kSIOState *s = opaque;

    s->uart_irq = deposit32(s->uart_irq, n, 1, level != 0);
    qemu_set_irq(s->irq, s->uart_irq != 0);
}

/* A transmitter with DMA enabled requests a byte whenever THR is empty */
static void sio_dma_tick(void *opaque)
{
    Rs6kSIOState *s = opaque;
    bool more = false;

    for (int i = 0; i < SIO_UARTS; i++) {
        SerialState *u = &s->uart[i];
        bool tc = false;
        uint8_t b;

        if (!(s->dma_ctl & (1 << i))) {
            continue;
        }
        if ((u->lsr & UART_LSR_THRE) &&
            rs6k_iocc_dma(s->iocc, s->dma_arb & 0xf, &b, 1, true, &tc)) {
            serial_io_ops.write(u, 0, b, 1);
        }
        more |= !tc;
    }
    if (more) {
        timer_mod(s->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->uart[0].char_transmit_time);
    }
}

static uint64_t sio_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kSIOState *s = opaque;

    return addr ? s->dma_ctl : s->uart_irq;
}

static void sio_dma_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    Rs6kSIOState *s = opaque;

    if (!addr) {
        s->dma_arb = val;
        return;
    }
    s->dma_ctl = val;
    timer_del(s->dma_timer);
    sio_dma_tick(s);
}

static const MemoryRegionOps sio_dma_ops = {
    .read = sio_dma_read,
    .write = sio_dma_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* The floppy transfer runs once its DMA channel has been enabled */
static void sio_fdc_dma_run(void *opaque)
{
    Rs6kSIOState *s = opaque;
    int count;

    if (!s->fdc_dreq || !s->fdc_handler) {
        return;
    }
    count = rs6k_iocc_dma_count(s->iocc, SIO_FDC_ARB);
    if (!count) {
        timer_mod(s->fdc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SCALE_MS);
        return;
    }
    s->fdc_handler(s->fdc_opaque, 0, 0, count);
}

static int sio_isadma_read(IsaDma *obj, int nchan, void *buf, int pos,
                           int len)
{
    Rs6kSIOState *s = RS6K_SIO(obj);
    bool tc;

    return rs6k_iocc_dma(s->iocc, SIO_FDC_ARB, buf, len, true, &tc);
}

static int sio_isadma_write(IsaDma *obj, int nchan, void *buf, int pos,
                            int len)
{
    Rs6kSIOState *s = RS6K_SIO(obj);
    bool tc;

    return rs6k_iocc_dma(s->iocc, SIO_FDC_ARB, buf, len, false, &tc);
}

static void sio_isadma_hold(IsaDma *obj, int nchan)
{
    RS6K_SIO(obj)->fdc_dreq = true;
}

static void sio_isadma_release(IsaDma *obj, int nchan)
{
    RS6K_SIO(obj)->fdc_dreq = false;
}

static void sio_isadma_schedule(IsaDma *obj)
{
    Rs6kSIOState *s = RS6K_SIO(obj);

    timer_mod(s->fdc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void sio_isadma_register(IsaDma *obj, int nchan,
                                IsaDmaTransferHandler handler, void *opaque)
{
    Rs6kSIOState *s = RS6K_SIO(obj);

    s->fdc_handler = handler;
    s->fdc_opaque = opaque;
}

static bool sio_isadma_autoinit(IsaDma *obj, int nchan)
{
    return false;
}

static void rs6k_sio_reset(DeviceState *dev)
{
    Rs6kSIOState *s = RS6K_SIO(dev);

    timer_del(s->dma_timer);
    timer_del(s->fdc_timer);
    s->dma_arb = 0;
    s->dma_ctl = 0;
    s->fdc_dreq = false;
}

static void rs6k_sio_realize_fdc(Rs6kSIOState *s, Error **errp)
{
    SysBusDevice *sbd;

    s->fdc = qdev_new("sysbus-fdc");
    sbd = SYS_BUS_DEVICE(s->fdc);
    object_property_add_child(OBJECT(s), "fdc", OBJECT(s->fdc));
    object_property_set_link(OBJECT(s->fdc), "dma", OBJECT(s), &error_abort);
    qdev_prop_set_int32(s->fdc, "dma-channel", 0);
    if (!sysbus_realize_and_unref(sbd, errp)) {
        return;
    }
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(s->iocc),
                                                SIO_FDC_LEVEL));
    memory_region_add_subregion(&s->iocc->bus_io, SIO_FDC_ADDR,
                                sysbus_mmio_get_region(sbd, 0));
    s->fdc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sio_fdc_dma_run, s);
}

static DeviceState *sio_input_port(Rs6kSIOState *s, const char *type,
                                   const char *name, hwaddr addr, int line,
                                   MemoryRegion *(*io)(DeviceState *),
                                   Error **errp)
{
    DeviceState *dev = qdev_new(type);

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    if (!qdev_realize_and_unref(dev, NULL, errp)) {
        return NULL;
    }
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in(DEVICE(&s->kbd_irq), line));
    memory_region_add_subregion(&s->iocc->bus_io, addr, io(dev));
    qemu_register_resettable(OBJECT(dev));
    return dev;
}

/* The keyboard and mouse ports share their interrupt level */
static void rs6k_sio_realize_input(Rs6kSIOState *s, Error **errp)
{
    object_initialize_child(OBJECT(s), "kbd-irq", &s->kbd_irq, TYPE_OR_IRQ);
    qdev_prop_set_uint16(DEVICE(&s->kbd_irq), "num-lines", 3);
    if (!qdev_realize(DEVICE(&s->kbd_irq), NULL, errp)) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(&s->kbd_irq), 0,
                          qdev_get_gpio_in(DEVICE(s->iocc), SIO_KBD_LEVEL));
    s->kbd = sio_input_port(s, TYPE_RS6K_KBD, "kbd", SIO_KBD_ADDR, 0,
                            rs6k_kbd_io, errp);
    if (!s->kbd) {
        return;
    }
    s->mse = sio_input_port(s, TYPE_RS6K_MSE, "mse", SIO_MSE_ADDR, 1,
                            rs6k_mse_io, errp);
    s->tablet.irq = qdev_get_gpio_in(DEVICE(&s->kbd_irq), 2);
    if (!qdev_realize(DEVICE(&s->tablet), NULL, errp)) {
        return;
    }
    memory_region_init_io(&s->tablet.io, OBJECT(s), &serial_io_ops, &s->tablet,
                          "sio.tablet", 8);
    memory_region_add_subregion(&s->iocc->bus_io, SIO_TABLET_ADDR,
                                &s->tablet.io);
}

static void sio_pos_write(void *opaque, int reg, uint8_t val)
{
    Rs6kSIOState *s = opaque;

    if (reg == SIO_POS_RESET) {
        rs6k_mse_set_reset(s->mse, val & SIO_RESET_MSE);
    }
}

static void rs6k_sio_realize(DeviceState *dev, Error **errp)
{
    Rs6kSIOState *s = RS6K_SIO(dev);
    Object *obj = OBJECT(dev);

    if (!s->iocc) {
        error_setg(errp, "iocc link not set");
        return;
    }
    s->irq = qdev_get_gpio_in(DEVICE(s->iocc), SIO_UART_LEVEL);
    for (int i = 0; i < SIO_UARTS; i++) {
        SerialState *u = &s->uart[i];

        u->irq = qemu_allocate_irq(sio_uart_irq, s, i);
        if (!qdev_realize(DEVICE(u), NULL, errp)) {
            return;
        }
        memory_region_init_io(&u->io, obj, &serial_io_ops, u, "sio.uart", 8);
        memory_region_add_subregion(&s->iocc->bus_io, uart_addr[i], &u->io);
    }
    memory_region_init_io(&s->dma_io, obj, &sio_dma_ops, s, "sio.dma", 2);
    memory_region_add_subregion(&s->iocc->bus_io, SIO_DMA_ADDR, &s->dma_io);
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sio_dma_tick, s);
    rs6k_sio_realize_fdc(s, errp);
    rs6k_sio_realize_input(s, errp);
    rs6k_iocc_set_slot(s->iocc, SIO_SLOT, SIO_ID, sio_pos_write, s);
}

static void rs6k_sio_init(Object *obj)
{
    Rs6kSIOState *s = RS6K_SIO(obj);

    for (int i = 0; i < SIO_UARTS; i++) {
        g_autofree char *name = g_strdup_printf("uart%d", i);
        g_autofree char *chr = g_strdup_printf("chardev%d", i);

        object_initialize_child(obj, name, &s->uart[i], TYPE_SERIAL);
        qdev_prop_set_uint32(DEVICE(&s->uart[i]), "baudbase", 8000000 / 16);
        object_property_add_alias(obj, chr, OBJECT(&s->uart[i]), "chardev");
    }
    object_initialize_child(obj, "tablet", &s->tablet, TYPE_SERIAL);
}

static const Property rs6k_sio_properties[] = {
    DEFINE_PROP_LINK("iocc", Rs6kSIOState, iocc, TYPE_RS6K_IOCC,
                     Rs6kIOCCState *),
};

static void rs6k_sio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    IsaDmaClass *idc = ISADMA_CLASS(klass);

    dc->realize = rs6k_sio_realize;
    device_class_set_legacy_reset(dc, rs6k_sio_reset);
    device_class_set_props(dc, rs6k_sio_properties);
    dc->user_creatable = false;
    idc->has_autoinitialization = sio_isadma_autoinit;
    idc->read_memory = sio_isadma_read;
    idc->write_memory = sio_isadma_write;
    idc->hold_DREQ = sio_isadma_hold;
    idc->release_DREQ = sio_isadma_release;
    idc->schedule = sio_isadma_schedule;
    idc->register_channel = sio_isadma_register;
}

static const TypeInfo rs6k_sio_types[] = {
    {
        .name = TYPE_RS6K_SIO,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(Rs6kSIOState),
        .instance_init = rs6k_sio_init,
        .class_init = rs6k_sio_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_ISADMA },
            { }
        },
    },
};

DEFINE_TYPES(rs6k_sio_types)
