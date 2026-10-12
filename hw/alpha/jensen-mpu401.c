/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Jensen ISA MPU-401 UART-only interface, with raw MIDI chardev transport.
 *
 * UART command, reset and status behavior adapted from 86Box
 * src/sound/snd_mpu401.c:
 * Copyright 2008-2024 DOSBox Team.
 * Copyright 2016-2024 Miran Grca.
 * Copyright 2016-2024 TheCollector1995.
 * Adapted to QEMU's ISA/PIC, timer and nonblocking chardev interfaces.
 * Reference: Roland MPU-401 command/status interface.
 * Intelligent sequencing, MIDI synthesis and wire-rate timing are absent.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "cpu.h"

#define TYPE_JENSEN_MPU401 "jensen-mpu401"
OBJECT_DECLARE_SIMPLE_TYPE(JensenMpu401, JENSEN_MPU401)
#define MPU_FIFO_SIZE 256
#define MPU_RESET_NS 1000000

struct JensenMpu401 {
    ISADevice parent_obj;
    DeviceState *sb16;
    MemoryRegion io;
    CharFrontend chr;
    qemu_irq irq;
    QEMUTimer *reset_timer;
    uint32_t iobase, isairq;
    uint8_t rx[MPU_FIFO_SIZE], tx[MPU_FIFO_SIZE];
    unsigned rx_head, rx_count, tx_head, tx_count;
    guint watch;
    bool uart, busy, pending;
    uint8_t pending_cmd;
};

static void mpu_command(JensenMpu401 *s, uint8_t value);
static void mpu_flush(JensenMpu401 *s);

static uint64_t mpu_caller(void)
{
    return current_cpu ?
        alpha_cpu_get_reg_value(&ALPHA_CPU(current_cpu)->env, 26) : 0;
}

static void mpu_irq(JensenMpu401 *s)
{
    qemu_set_irq(s->irq, s->rx_count != 0);
}

static void mpu_enqueue(JensenMpu401 *s, uint8_t value)
{
    if (s->rx_count == MPU_FIFO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "MPU-401 input FIFO overflow\n");
        return;
    }
    s->rx[(s->rx_head + s->rx_count++) % MPU_FIFO_SIZE] = value;
    mpu_irq(s);
}

static gboolean mpu_writable(void *unused, GIOCondition cond, void *opaque)
{
    JensenMpu401 *s = opaque;
    s->watch = 0;
    if (!(cond & G_IO_HUP)) {
        mpu_flush(s);
    }
    return G_SOURCE_REMOVE;
}

static void mpu_flush(JensenMpu401 *s)
{
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        /* Accept output without a backend, like a disconnected MIDI lead. */
        s->tx_head = s->tx_count = 0;
        return;
    }
    while (s->tx_count) {
        unsigned amount = MIN(s->tx_count, MPU_FIFO_SIZE - s->tx_head);
        int written = qemu_chr_fe_write(&s->chr, s->tx + s->tx_head, amount);
        if (written <= 0) {
            if (!s->watch) {
                s->watch = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                                mpu_writable, s);
            }
            return;
        }
        s->tx_head = (s->tx_head + written) % MPU_FIFO_SIZE;
        s->tx_count -= written;
    }
    g_clear_handle_id(&s->watch, g_source_remove);
}

static int mpu_can_receive(void *opaque)
{
    JensenMpu401 *s = opaque;
    return s->uart && !s->busy ? MPU_FIFO_SIZE - s->rx_count : 0;
}

static void mpu_receive(void *opaque, const uint8_t *buf, int size)
{
    JensenMpu401 *s = opaque;
    for (int i = 0; i < size; i++) {
        mpu_enqueue(s, buf[i]);
    }
}

static void mpu_event(void *opaque, QEMUChrEvent event)
{
    JensenMpu401 *s = opaque;
    if (event == CHR_EVENT_OPENED) {
        mpu_flush(s);
    } else if (event == CHR_EVENT_CLOSED) {
        g_clear_handle_id(&s->watch, g_source_remove);
    }
}

static void mpu_clear(JensenMpu401 *s)
{
    g_clear_handle_id(&s->watch, g_source_remove);
    s->rx_head = s->rx_count = s->tx_head = s->tx_count = 0;
    s->uart = s->busy = s->pending = false;
    mpu_irq(s);
}

static void mpu_reset_done(void *opaque)
{
    JensenMpu401 *s = opaque;
    s->busy = false;
    if (s->pending) {
        uint8_t command = s->pending_cmd;
        s->pending = false;
        mpu_command(s, command);
    }
    qemu_chr_fe_accept_input(&s->chr);
}

static void mpu_command(JensenMpu401 *s, uint8_t value)
{
    if (s->busy) {
        s->pending = true;
        s->pending_cmd = value;
        return;
    }
    if (value == 0xff) {
        bool was_uart = s->uart;
        mpu_clear(s);
        s->busy = true;
        timer_mod(s->reset_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MPU_RESET_NS);
        /* As in 86Box, leaving UART by reset does not generate an ACK. */
        if (!was_uart) {
            mpu_enqueue(s, 0xfe);
        }
    } else if (!s->uart && value == 0x3f) {
        s->uart = true;
        mpu_enqueue(s, 0xfe);
        qemu_chr_fe_accept_input(&s->chr);
    }
    /* UART-only devices ignore intelligent-mode commands, without an ACK. */
}

static uint64_t mpu_read(void *opaque, hwaddr address, unsigned size)
{
    JensenMpu401 *s = opaque;
    uint8_t value = 0xfe;
    if (address == 1) {
        value = 0x3f | (!s->rx_count ? 0x80 : 0) |
                (s->pending || s->tx_count == MPU_FIFO_SIZE ? 0x40 : 0);
        trace_jensen_mpu_read(address, value, mpu_caller());
        return value;
    }
    if (s->rx_count) {
        value = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % MPU_FIFO_SIZE;
        s->rx_count--;
        /* Reassert after every byte while more input remains (edge IRQ). */
        qemu_irq_lower(s->irq);
        mpu_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
    }
    trace_jensen_mpu_read(address, value, mpu_caller());
    return value;
}

static void mpu_write(void *opaque, hwaddr address, uint64_t value,
                      unsigned size)
{
    JensenMpu401 *s = opaque;
    trace_jensen_mpu_write(address, value, mpu_caller());
    if (address == 1) {
        mpu_command(s, value);
    } else {
        if (s->tx_count == MPU_FIFO_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR, "MPU-401 output while busy\n");
            return;
        }
        /* UART-only variants pass data bytes without intelligent parsing. */
        s->tx[(s->tx_head + s->tx_count++) % MPU_FIFO_SIZE] = value;
        mpu_flush(s);
    }
}

static const MemoryRegionOps mpu_ops = {
    .read = mpu_read,
    .write = mpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void mpu_reset(DeviceState *dev)
{
    JensenMpu401 *s = JENSEN_MPU401(dev);
    timer_del(s->reset_timer);
    mpu_clear(s);
}

static void mpu_realize(DeviceState *dev, Error **errp)
{
    JensenMpu401 *s = JENSEN_MPU401(dev);
    if (s->iobase < 0x100 || s->iobase > 0xfffe || (s->iobase & 1) ||
        s->isairq > 15 || s->isairq == 2) {
        error_setg(errp, "MPU-401 requires an even ISA base and a valid IRQ");
        return;
    }
    s->irq = s->sb16 ? qdev_get_gpio_in_named(s->sb16, "midi-irq", 0) :
        isa_bus_get_irq(isa_bus_from_device(ISA_DEVICE(dev)), s->isairq);
    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mpu_reset_done, s);
    memory_region_init_io(&s->io, OBJECT(dev), &mpu_ops, s,
                          TYPE_JENSEN_MPU401, 2);
    isa_register_ioport(ISA_DEVICE(dev), &s->io, s->iobase);
    qemu_chr_fe_set_handlers(&s->chr, mpu_can_receive, mpu_receive, mpu_event,
                             NULL, s, NULL, true);
    mpu_reset(dev);
}

static void mpu_unrealize(DeviceState *dev)
{
    JensenMpu401 *s = JENSEN_MPU401(dev);
    mpu_clear(s);
    timer_free(s->reset_timer);
    qemu_chr_fe_deinit(&s->chr, false);
}

static const Property mpu_properties[] = {
    DEFINE_PROP_UINT32("iobase", JensenMpu401, iobase, 0x330),
    DEFINE_PROP_UINT32("irq", JensenMpu401, isairq, 9),
    DEFINE_PROP_CHR("chardev", JensenMpu401, chr),
    DEFINE_PROP_LINK("sb16", JensenMpu401, sb16, "sb16", DeviceState *),
};

static const VMStateDescription mpu_vmstate = {
    .name = TYPE_JENSEN_MPU401,
    .unmigratable = true,
};

static void mpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = mpu_realize;
    dc->unrealize = mpu_unrealize;
    dc->legacy_reset = mpu_reset;
    dc->vmsd = &mpu_vmstate;
    device_class_set_props(dc, mpu_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo mpu_type = {
    .name = TYPE_JENSEN_MPU401,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(JensenMpu401),
    .class_init = mpu_class_init,
};

static void mpu_register_types(void)
{
    type_register_static(&mpu_type);
}

type_init(mpu_register_types)
