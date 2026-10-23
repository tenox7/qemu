/*
 * IBM RS/6000 7006 keyboard port
 *
 * The 7006 standard I/O talks to the keyboard directly: a byte written to
 * I/O 0x50 goes to the keyboard and bytes from the keyboard are read there.
 * Reading 0x51 shows 0x40 while a received byte waits and 0x20 while the
 * transmitter is free. The port interrupts on level 1 while a byte waits.
 * Writes to 0x54 and 0x55 set the speaker tone, which is not emulated.
 *
 * The keyboard is QEMU's PS/2 keyboard, which AIX switches to scan code
 * set 3. The IBM keyboard also answers the layout ID command 0xef, with
 * 0xb0 0xbf for the US 101-key layout, and takes a list of keys after the
 * set key type commands 0xfb-0xfd. Key types are not kept: every key sends
 * make and break codes, which is what AIX asks for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/input/ps2.h"
#include "hw/ppc/rs6k_kbd.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(Rs6kKbdState, RS6K_KBD)

#define KBD_DATA        0x0
#define KBD_STATUS      0x1
#define KBD_ST_ERROR    0x80
#define KBD_ST_RX       0x40
#define KBD_ST_TX       0x20

#define KBD_CMD_SET_LEDS 0xed
#define KBD_CMD_LAYOUT  0xef
#define KBD_CMD_ALL_REP 0xf7
#define KBD_CMD_ALL_MAKE 0xf9
#define KBD_CMD_KEY_REP 0xfb
#define KBD_CMD_KEY_MAKE 0xfd
#define KBD_REPLY_ACK   0xfa

static const uint8_t layout_us[] = { KBD_REPLY_ACK, 0xb0, 0xbf };

struct Rs6kKbdState {
    DeviceState parent_obj;

    MemoryRegion io;
    qemu_irq irq;
    PS2KbdState ps2;

    uint8_t ctl;
    bool keylist;
    bool rx_full;
    uint8_t rx;
    uint8_t reply[8];
    unsigned nreply;
};

static void kbd_update_irq(Rs6kKbdState *s)
{
    qemu_set_irq(s->irq, s->rx_full);
}

/* Replies made here go before anything the PS/2 keyboard has queued */
static void kbd_fill(Rs6kKbdState *s)
{
    PS2State *ps2 = PS2_DEVICE(&s->ps2);

    if (s->rx_full) {
        return;
    }
    if (s->nreply) {
        s->rx = s->reply[0];
        memmove(s->reply, s->reply + 1, --s->nreply);
        s->rx_full = true;
    } else if (ps2->queue.count) {
        /* reading re-raises the PS/2 interrupt, which comes back here */
        s->rx_full = true;
        s->rx = ps2_read_data(ps2);
    }
    kbd_update_irq(s);
}

static void kbd_ps2_irq(void *opaque, int n, int level)
{
    if (level) {
        kbd_fill(opaque);
    }
}

static uint64_t kbd_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kKbdState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case KBD_DATA:
        val = s->rx;
        s->rx_full = false;
        kbd_fill(s);
        break;
    case KBD_STATUS:
        val = KBD_ST_TX | (s->rx_full ? KBD_ST_RX : 0);
        break;
    }
    trace_rs6k_kbd_read(addr, val);
    return val;
}

static void kbd_reply(Rs6kKbdState *s, const uint8_t *b, unsigned n)
{
    if (s->nreply + n > sizeof(s->reply)) {
        return;
    }
    memcpy(s->reply + s->nreply, b, n);
    s->nreply += n;
    kbd_fill(s);
}

static void kbd_command(Rs6kKbdState *s, uint8_t val)
{
    static const uint8_t ack = KBD_REPLY_ACK;

    if (s->keylist && val < KBD_CMD_SET_LEDS) {
        kbd_reply(s, &ack, 1);
        return;
    }
    s->keylist = false;
    switch (val) {
    case KBD_CMD_LAYOUT:
        kbd_reply(s, layout_us, sizeof(layout_us));
        return;
    case KBD_CMD_KEY_REP ... KBD_CMD_KEY_MAKE:
        s->keylist = true;
        /* fall through */
    case KBD_CMD_ALL_REP ... KBD_CMD_ALL_MAKE:
        kbd_reply(s, &ack, 1);
        return;
    }
    ps2_write_keyboard(&s->ps2, val);
}

static void kbd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rs6kKbdState *s = opaque;

    trace_rs6k_kbd_write(addr, size, val);
    switch (addr) {
    case KBD_DATA:
        kbd_command(s, val);
        break;
    case KBD_STATUS:
        s->ctl = val;
        break;
    }
}

static const MemoryRegionOps kbd_ops = {
    .read = kbd_read,
    .write = kbd_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void rs6k_kbd_reset(DeviceState *dev)
{
    Rs6kKbdState *s = RS6K_KBD(dev);

    s->ctl = 0;
    s->keylist = false;
    s->rx_full = false;
    s->nreply = 0;
    kbd_update_irq(s);
}

static void rs6k_kbd_realize(DeviceState *dev, Error **errp)
{
    Rs6kKbdState *s = RS6K_KBD(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ps2), errp)) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(&s->ps2), PS2_DEVICE_IRQ,
                          qemu_allocate_irq(kbd_ps2_irq, s, 0));
    memory_region_init_io(&s->io, OBJECT(dev), &kbd_ops, s, "rs6k-kbd", 0x8);
    qdev_init_gpio_out(dev, &s->irq, 1);
}

static void rs6k_kbd_init(Object *obj)
{
    Rs6kKbdState *s = RS6K_KBD(obj);

    object_initialize_child(obj, "ps2", &s->ps2, TYPE_PS2_KBD_DEVICE);
}

MemoryRegion *rs6k_kbd_io(DeviceState *dev)
{
    return &RS6K_KBD(dev)->io;
}

static void rs6k_kbd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rs6k_kbd_realize;
    device_class_set_legacy_reset(dc, rs6k_kbd_reset);
    dc->user_creatable = false;
}

static const TypeInfo rs6k_kbd_types[] = {
    {
        .name = TYPE_RS6K_KBD,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(Rs6kKbdState),
        .instance_init = rs6k_kbd_init,
        .class_init = rs6k_kbd_class_init,
    },
};

DEFINE_TYPES(rs6k_kbd_types)
