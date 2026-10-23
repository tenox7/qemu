/*
 * IBM RS/6000 7006 mouse port
 *
 * A 16-bit write to I/O 0x48 carries a port command in the low byte (0x49);
 * with command 0 the high byte (0x48) goes to the mouse, command 1 echoes
 * it back. AIX writes the two bytes separately. Commands 2 and 3 enable and
 * disable the interrupt, 4 and 5 turn blocking of 3-byte reports on and
 * off; the port acknowledges each with 0xfa. Reading 0x4a shows bit 0 while
 * the interrupt is enabled and bit 1 while blocking. The 32-bit word at 0x4c
 * holds the oldest report: a status byte (0x01 valid, 0x08, 0x10 and 0x20
 * for each data byte present) and up to three data bytes; reading it
 * removes the report. The port interrupts on level 1 while a report waits
 * and the interrupt is enabled.
 *
 * The mouse is QEMU's PS/2 mouse. Blocking only applies to bytes it sends
 * on its own; replies to a command go through one at a time. After the
 * sequence 0xe8 0x00 0xe6 0xe6 0xe6 the status request 0xe9 reports three
 * buttons in its second byte, as on the IBM 3-button mouse.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/input/ps2.h"
#include "hw/ppc/rs6k_mse.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(Rs6kMseState, RS6K_MSE)

#define MSE_CMD         0x0
#define MSE_STATUS      0x2
#define MSE_REPORT      0x4

#define CMD_SEND        0
#define CMD_WRAP        1
#define CMD_IRQ_ON      2
#define CMD_IRQ_OFF     3
#define CMD_BLOCK_ON    4
#define CMD_BLOCK_OFF   5

#define ST_IRQ_ON       0x01
#define ST_BLOCK        0x02

#define RPT_VALID       0x01
#define RPT_BYTE1       0x08

#define MSE_REPLY_ACK   0xfa
#define MSE_SET_RES     0xe8
#define MSE_SCALE_11    0xe6
#define MSE_STATUS_REQ  0xe9
#define MSE_BUTTONS     3
#define MSE_QUEUE       16

struct Rs6kMseState {
    DeviceState parent_obj;

    MemoryRegion io;
    qemu_irq irq;
    PS2MouseState ps2;

    uint8_t status;
    uint8_t data;
    uint32_t report[MSE_QUEUE];
    unsigned head, count;
    uint8_t part[3];
    unsigned npart;
    bool draining;
    bool reply;
    unsigned magic;
    unsigned reply_bytes;
};

static void mse_update_irq(Rs6kMseState *s)
{
    qemu_set_irq(s->irq, s->count && (s->status & ST_IRQ_ON));
}

static void mse_post(Rs6kMseState *s, const uint8_t *b, unsigned n)
{
    uint32_t r = RPT_VALID << 24;

    if (s->count == MSE_QUEUE) {
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        r |= (RPT_BYTE1 << i) << 24 | b[i] << (16 - 8 * i);
    }
    s->report[(s->head + s->count++) % MSE_QUEUE] = r;
    mse_update_irq(s);
}

/* While blocking, bytes from the mouse are passed on three at a time */
static void mse_byte(Rs6kMseState *s, uint8_t b)
{
    if (s->reply && s->magic == 6 && s->reply_bytes++ == 2) {
        b = MSE_BUTTONS;
    }
    if (s->reply || !(s->status & ST_BLOCK)) {
        mse_post(s, &b, 1);
        return;
    }
    s->part[s->npart++] = b;
    if (s->npart == sizeof(s->part)) {
        mse_post(s, s->part, s->npart);
        s->npart = 0;
    }
}

/* Reading re-raises the PS/2 interrupt: the outer call keeps the order */
static void mse_ps2_irq(void *opaque, int n, int level)
{
    Rs6kMseState *s = opaque;
    PS2State *ps2 = PS2_DEVICE(&s->ps2);

    if (!level || s->draining) {
        return;
    }
    s->draining = true;
    while (ps2->queue.count) {
        mse_byte(s, ps2_read_data(ps2));
    }
    s->draining = false;
}

/* Counts the steps of the button query: 0xe8 0x00 0xe6 0xe6 0xe6 0xe9 */
static void mse_send(Rs6kMseState *s, uint8_t b)
{
    static const uint8_t magic[] = {
        MSE_SET_RES, 0x00, MSE_SCALE_11, MSE_SCALE_11, MSE_SCALE_11,
        MSE_STATUS_REQ,
    };

    s->magic = b == magic[s->magic % 6] ? s->magic + 1 :
               b == magic[0] ? 1 : 0;
    s->reply = true;
    s->reply_bytes = 0;
    ps2_write_mouse(&s->ps2, b);
    s->reply = false;
    s->magic %= 6;
}

static void mse_command(Rs6kMseState *s, uint16_t val)
{
    static const uint8_t ack = MSE_REPLY_ACK;

    switch (val & 0xff) {
    case CMD_SEND:
        mse_send(s, val >> 8);
        return;
    case CMD_WRAP:
        s->data = val >> 8;
        mse_post(s, &s->data, 1);
        return;
    case CMD_IRQ_ON:
        s->status |= ST_IRQ_ON;
        break;
    case CMD_IRQ_OFF:
        s->status &= ~ST_IRQ_ON;
        break;
    case CMD_BLOCK_ON:
        s->status |= ST_BLOCK;
        s->npart = 0;
        break;
    case CMD_BLOCK_OFF:
        s->status &= ~ST_BLOCK;
        s->npart = 0;
        break;
    }
    mse_post(s, &ack, 1);
}

static uint64_t mse_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kMseState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case MSE_STATUS:
        val = s->status;
        break;
    case MSE_REPORT:
        if (!s->count) {
            break;
        }
        val = s->report[s->head];
        s->head = (s->head + 1) % MSE_QUEUE;
        s->count--;
        mse_update_irq(s);
        break;
    }
    trace_rs6k_mse_read(addr, size, val);
    return val;
}

static void mse_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rs6kMseState *s = opaque;

    trace_rs6k_mse_write(addr, size, val);
    if (addr == MSE_CMD && size == 2) {
        mse_command(s, val);
    } else if (addr == MSE_CMD) {
        s->data = val;
    } else if (addr == MSE_CMD + 1) {
        mse_command(s, s->data << 8 | val);
    }
}

static const MemoryRegionOps mse_ops = {
    .read = mse_read,
    .write = mse_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

void rs6k_mse_set_reset(DeviceState *dev, bool reset)
{
    Rs6kMseState *s = RS6K_MSE(dev);

    if (!reset) {
        return;
    }
    s->status = 0;
    s->count = 0;
    s->npart = 0;
    mse_update_irq(s);
}

static void rs6k_mse_reset(DeviceState *dev)
{
    rs6k_mse_set_reset(dev, true);
}

static void rs6k_mse_realize(DeviceState *dev, Error **errp)
{
    Rs6kMseState *s = RS6K_MSE(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ps2), errp)) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(&s->ps2), PS2_DEVICE_IRQ,
                          qemu_allocate_irq(mse_ps2_irq, s, 0));
    memory_region_init_io(&s->io, OBJECT(dev), &mse_ops, s, "rs6k-mse", 0x8);
    qdev_init_gpio_out(dev, &s->irq, 1);
}

static void rs6k_mse_init(Object *obj)
{
    Rs6kMseState *s = RS6K_MSE(obj);

    object_initialize_child(obj, "ps2", &s->ps2, TYPE_PS2_MOUSE_DEVICE);
}

MemoryRegion *rs6k_mse_io(DeviceState *dev)
{
    return &RS6K_MSE(dev)->io;
}

static void rs6k_mse_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rs6k_mse_realize;
    device_class_set_legacy_reset(dc, rs6k_mse_reset);
    dc->user_creatable = false;
}

static const TypeInfo rs6k_mse_types[] = {
    {
        .name = TYPE_RS6K_MSE,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(Rs6kMseState),
        .instance_init = rs6k_mse_init,
        .class_init = rs6k_mse_class_init,
    },
};

DEFINE_TYPES(rs6k_mse_types)
