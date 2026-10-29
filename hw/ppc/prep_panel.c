/*
 * IBM 40P front panel
 *
 * The 40P has a power light and a disk light, which follows the Hardfile
 * Light register in the system I/O ports. AIX sets that only now and
 * then, so reads and writes on the hard disks light it too.
 *
 * The IBM firmware also writes its progress codes to port 0x680, where
 * nothing on the board listens. Four digits show their low 16 bits.
 *
 * AIX 4 shows its codes (showled, the boot progress) through the planar
 * PAL only when the residual data list an LED display, and the 40P's do
 * not. So the panel adds one at port 0x680 to the residual data the
 * firmware leaves near the top of memory, before the operating system
 * reads them. AIX writes 16 bits there, and the last three digits show
 * them as on the RS/6000, where a digit of 0xf is dark.
 *
 * The panel has a window of its own with the SDL display.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/crc32c.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/isa/isa.h"
#include "system/block-backend.h"
#include "system/physmem.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/vgafont.h"
#include "trace.h"

#define TYPE_PREP_PANEL "prep-panel"
OBJECT_DECLARE_SIMPLE_TYPE(PrepPanelState, PREP_PANEL)

struct PrepPanelState {
    ISADevice parent_obj;

    MemoryRegion io;
    uint32_t code;
    bool code_valid;
    bool aix;
    bool disk;
    bool disk_lit;
    uint64_t disk_ops;
    int64_t disk_until;
    QemuConsole *con;
    QemuInputHandlerState *input;
    Notifier machine_done;
    int pointer_x, pointer_y;
    bool redraw;
    QEMUTimer *res_timer;
    int64_t res_until;
    uint32_t res_crc;
};

#define PANEL_W     440
#define PANEL_H     120

/* lights: centre and radius, label to the right */
#define LIGHT_Y     60
#define LIGHT_R     7
#define POWER_X     28
#define DISK_X      108
#define LABEL_DX    14

/* digits: segment centre lines DIGIT_W by DIGIT_H, SEG_T thick */
#define DIGIT_X     188
#define DIGIT_Y     28
#define DIGIT_W     36
#define DIGIT_H     64
#define DIGIT_STEP  60
#define SEG_T       8
#define BEZEL_X     168
#define BEZEL_Y     12
#define BEZEL_W     256
#define BEZEL_H     96

/* each disk access keeps the light on long enough to be seen */
#define DISK_HOLD_MS    80

#define COL_CASE        0x2a2a2a
#define COL_EDGE        0x5a5a5a
#define COL_BEZEL       0x050505
#define COL_SEG_ON      0xff3018
#define COL_SEG_OFF     0x2a0a06
#define COL_LABEL       0x808080
#define COL_POWER_ON    0x30e030
#define COL_DISK_ON     0xffb000
#define COL_DISK_OFF    0x3a2a08

/* segments a-g in bits 0-6 */
static const uint8_t panel_font[16] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
    0x7f, 0x6f, 0x77, 0x7c, 0x39, 0x5e, 0x79, 0x71,
};

/* the RS/6000's, with a lowercase c and 0xf dark */
static const uint8_t panel_aix_font[16] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
    0x7f, 0x6f, 0x77, 0x7c, 0x58, 0x5e, 0x79, 0x00,
};

/* segment ends as digit column 0-1 and row 0-2 */
static const uint8_t panel_segs[7][4] = {
    { 0, 0, 1, 0 }, { 1, 0, 1, 1 }, { 1, 1, 1, 2 }, { 0, 2, 1, 2 },
    { 0, 1, 0, 2 }, { 0, 0, 0, 1 }, { 0, 1, 1, 1 },
};

static const char *const panel_arrow[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X",
    "X.......X", "X........X", "X.....XXXXX", "X..X..X", "X.X X..X",
    "XX  X..X", "X    X..X", "     X..X", "      XX",
};

static void panel_fill(uint32_t *fb, int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            fb[j * PANEL_W + i] = c;
        }
    }
}

/* a horizontal or vertical segment with pointed ends */
static void panel_seg(uint32_t *fb, int x0, int y0, int x1, int y1,
                      uint32_t c)
{
    bool horiz = y0 == y1;
    int gap = SEG_T / 2 + 1;

    if (horiz) {
        x0 += gap;
        x1 -= gap;
    } else {
        y0 += gap;
        y1 -= gap;
    }
    for (int y = y0 - SEG_T / 2; y <= y1 + SEG_T / 2; y++) {
        for (int x = x0 - SEG_T / 2; x <= x1 + SEG_T / 2; x++) {
            int along = horiz ? MAX(x0 - x, x - x1) : MAX(y0 - y, y - y1);
            int across = horiz ? abs(y - y0) : abs(x - x0);

            if (MAX(along, 0) + across <= SEG_T / 2) {
                fb[y * PANEL_W + x] = c;
            }
        }
    }
}

static void panel_text(uint32_t *fb, int x, int y, const char *s, uint32_t c)
{
    for (; *s; s++, x += FONT_WIDTH) {
        const uint8_t *glyph = vgafont16 +
                               (uint8_t)g_ascii_toupper(*s) * FONT_HEIGHT;

        for (int j = 0; j < FONT_HEIGHT; j++) {
            for (int i = 0; i < FONT_WIDTH; i++) {
                if (glyph[j] & (0x80 >> i)) {
                    fb[(y + j) * PANEL_W + x + i] = c;
                }
            }
        }
    }
}

static void panel_light(uint32_t *fb, int x, const char *label, uint32_t c)
{
    for (int dy = -LIGHT_R; dy <= LIGHT_R; dy++) {
        for (int dx = -LIGHT_R; dx <= LIGHT_R; dx++) {
            int r2 = dx * dx + dy * dy;

            if (r2 <= LIGHT_R * LIGHT_R) {
                fb[(LIGHT_Y + dy) * PANEL_W + x + dx] =
                    r2 > (LIGHT_R - 2) * (LIGHT_R - 2) ? COL_EDGE : c;
            }
        }
    }
    panel_text(fb, x + LABEL_DX, LIGHT_Y - FONT_HEIGHT / 2, label, COL_LABEL);
}

/*
 * The UI hides the host pointer over a window with absolute input and
 * shows it again once the pointer reaches the edge, so draw one inside.
 */
static void panel_draw_pointer(PrepPanelState *s, uint32_t *fb)
{
    int x = s->pointer_x, y = s->pointer_y;

    if (x <= 0 || y <= 0 || x >= PANEL_W - 1 || y >= PANEL_H - 1) {
        return;
    }
    for (unsigned j = 0; j < ARRAY_SIZE(panel_arrow) && y + j < PANEL_H; j++) {
        for (unsigned i = 0; panel_arrow[j][i] && x + i < PANEL_W; i++) {
            char c = panel_arrow[j][i];

            if (c != ' ') {
                fb[(y + j) * PANEL_W + x + i] = c == 'X' ? 0 : 0xffffff;
            }
        }
    }
}

static void panel_draw(PrepPanelState *s, uint32_t *fb)
{
    panel_fill(fb, 0, 0, PANEL_W, PANEL_H, COL_CASE);
    panel_light(fb, POWER_X, "power", COL_POWER_ON);
    panel_light(fb, DISK_X, "disk", s->disk_lit ? COL_DISK_ON : COL_DISK_OFF);
    panel_fill(fb, BEZEL_X - 1, BEZEL_Y - 1, BEZEL_W + 2, BEZEL_H + 2,
               COL_EDGE);
    panel_fill(fb, BEZEL_X, BEZEL_Y, BEZEL_W, BEZEL_H, COL_BEZEL);
    for (int i = 0; i < 4; i++) {
        unsigned d = extract32(s->code, 12 - 4 * i, 4);
        uint8_t on = !s->code_valid ? 0 :
                     !s->aix ? panel_font[d] :
                     i ? panel_aix_font[d] : 0;
        int x = DIGIT_X + i * DIGIT_STEP;

        for (int j = 0; j < 7; j++) {
            const uint8_t *e = panel_segs[j];

            panel_seg(fb, x + e[0] * DIGIT_W, DIGIT_Y + e[1] * DIGIT_H / 2,
                      x + e[2] * DIGIT_W, DIGIT_Y + e[3] * DIGIT_H / 2,
                      on & BIT(j) ? COL_SEG_ON : COL_SEG_OFF);
        }
    }
    panel_draw_pointer(s, fb);
}

static uint64_t panel_disk_ops(void)
{
    uint64_t ops = 0;

    for (BlockBackend *blk = blk_next(NULL); blk; blk = blk_next(blk)) {
        DeviceState *dev = blk_get_attached_dev(blk);

        if (dev && object_dynamic_cast(OBJECT(dev), "scsi-hd")) {
            BlockAcctStats *stats = blk_get_stats(blk);

            ops += stats->nr_ops[BLOCK_ACCT_READ] +
                   stats->nr_ops[BLOCK_ACCT_WRITE];
        }
    }
    return ops;
}

static bool panel_update_display(void *opaque)
{
    PrepPanelState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    uint64_t ops = panel_disk_ops();
    bool disk;

    if (ops != s->disk_ops) {
        s->disk_ops = ops;
        s->disk_until = now + DISK_HOLD_MS;
    }
    disk = s->disk || now < s->disk_until;
    if (disk != s->disk_lit) {
        s->disk_lit = disk;
        s->redraw = true;
    }
    if (!s->redraw || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    panel_draw(s, (uint32_t *)surface_data(surface));
    qemu_console_update(s->con, 0, 0, PANEL_W, PANEL_H);
    s->redraw = false;
    return true;
}

static void panel_invalidate_display(void *opaque)
{
    PrepPanelState *s = opaque;

    s->redraw = true;
}

static const GraphicHwOps panel_gfx_ops = {
    .gfx_update = panel_update_display,
    .invalidate = panel_invalidate_display,
};

static int panel_axis(int value, int size)
{
    return qemu_input_scale_axis(value, INPUT_EVENT_ABS_MIN,
                                 INPUT_EVENT_ABS_MAX, 0, size);
}

/* clicks on the panel go nowhere, not to the guest's mouse */
static void panel_input_event(DeviceState *dev, QemuConsole *src,
                              QemuInputEvent *evt)
{
    PrepPanelState *s = PREP_PANEL(dev);

    if (evt->type != INPUT_EVENT_KIND_ABS) {
        return;
    }
    if (evt->abs.axis == INPUT_AXIS_X) {
        s->pointer_x = panel_axis(evt->abs.value, PANEL_W);
    } else {
        s->pointer_y = panel_axis(evt->abs.value, PANEL_H);
    }
    s->redraw = true;
}

static const QemuInputHandler panel_input = {
    .name = "40p front panel",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = panel_input_event,
};

static void panel_disk_light(void *opaque, int n, int level)
{
    PrepPanelState *s = opaque;

    if (s->disk && !level) {
        s->disk_until = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + DISK_HOLD_MS;
    }
    s->disk = level;
}

/*
 * PReP residual data, big-endian: the devices, then a heap with a list
 * of PnP packets for each device's allocated, possible and compatible
 * resources
 */
#define RES_LENGTH      0x6a0c
#define RES_MODEL       0x8
#define RES_NUM_DEVS    0x608
#define RES_DEVS        0x60c
#define RES_DEV_SIZE    36
#define RES_MAX_DEVS    256
#define RES_HEAP        0x2a0c
#define RES_HEAP_SIZE   0x4000
#define RES_SEARCH      (256 * KiB)
#define RES_POLL_MS     10
#define RES_POLL_FOR_MS 10000

/* IBM000C on ISA, flagged as the disk light: operator panel, status LED */
static const uint8_t panel_res_dev[24] = {
    0x00, 0x00, 0x00, 0x01, 0x24, 0x4d, 0x00, 0x0c,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x28, 0x00,
    0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* I/O ports 0x680-0x683, then empty possible and compatible lists */
static const uint8_t panel_res_packets[] = {
    0x47, 0x01, 0x80, 0x06, 0x80, 0x06, 0x01, 0x04, 0x78,
    0x78,
    0x78,
};

/* where a packet list ends, or 0 if it runs off the heap */
static uint32_t panel_res_list_end(const uint8_t *heap, uint32_t off)
{
    while (off < RES_HEAP_SIZE) {
        uint8_t tag = heap[off];

        if (tag & 0x80) {
            if (off + 3 > RES_HEAP_SIZE) {
                break;
            }
            off += 3 + lduw_le_p(heap + off + 1);
        } else if (tag >> 3 == 0xf) {
            return off + 1 + (tag & 7);
        } else {
            off += 1 + (tag & 7);
        }
    }
    return 0;
}

/* add the LED unless the residual data have one or no room for it */
static bool panel_res_add_led(uint8_t *res)
{
    uint32_t n = ldl_be_p(res + RES_NUM_DEVS), end = 0;
    uint8_t *dev;

    if (!n || n >= RES_MAX_DEVS) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        dev = res + RES_DEVS + i * RES_DEV_SIZE;
        if (dev[16] == 8 && dev[17] == 8 && dev[18] == 6) {
            return false;
        }
        for (int j = 0; j < 3; j++) {
            uint32_t e = panel_res_list_end(res + RES_HEAP,
                                            ldl_be_p(dev + 24 + 4 * j));

            if (!e) {
                return false;
            }
            end = MAX(end, e);
        }
    }
    if (end + sizeof(panel_res_packets) > RES_HEAP_SIZE) {
        return false;
    }
    dev = res + RES_DEVS + n * RES_DEV_SIZE;
    memcpy(dev, panel_res_dev, sizeof(panel_res_dev));
    stl_be_p(dev + 24, end);
    stl_be_p(dev + 28, end + sizeof(panel_res_packets) - 2);
    stl_be_p(dev + 32, end + sizeof(panel_res_packets) - 1);
    memcpy(res + RES_HEAP + end, panel_res_packets,
           sizeof(panel_res_packets));
    stl_be_p(res + RES_NUM_DEVS, n + 1);
    return true;
}

/*
 * The firmware writes the residual data after its last progress code,
 * just before it starts the boot image, and AIX takes them some 150 ms
 * later. So look for them every RES_POLL_MS for a while after each code,
 * with the CPU stopped, and change them once they stop changing.
 */
static void panel_res_poll(CPUState *cpu, run_on_cpu_data data)
{
    PrepPanelState *s = data.host_ptr;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    hwaddr top = current_machine->ram_size;
    hwaddr start = top > RES_SEARCH ? top - RES_SEARCH : 0;
    g_autofree uint8_t *buf = g_malloc(top - start);
    uint8_t *res = NULL;
    uint32_t crc;

    if (now < s->res_until) {
        timer_mod(s->res_timer, now + RES_POLL_MS);
    }
    physical_memory_read(start, buf, top - start);
    for (hwaddr off = 0; off + RES_LENGTH <= top - start; off += 4) {
        if (ldl_be_p(buf + off) == RES_LENGTH &&
            !memcmp(buf + off + RES_MODEL, "IBM", 3)) {
            res = buf + off;
            break;
        }
    }
    if (!res) {
        s->res_crc = 0;
        return;
    }
    crc = crc32c(0, res, RES_LENGTH);
    if (crc != s->res_crc) {
        s->res_crc = crc;
        return;
    }
    if (panel_res_add_led(res)) {
        physical_memory_write(start + (res - buf), res, RES_LENGTH);
        s->res_crc = crc32c(0, res, RES_LENGTH);
        trace_prep_panel_residual(start + (res - buf),
                                  ldl_be_p(res + RES_NUM_DEVS) - 1);
    }
}

static void panel_res_timer(void *opaque)
{
    async_safe_run_on_cpu(first_cpu, panel_res_poll,
                          RUN_ON_CPU_HOST_PTR(opaque));
}

/* nothing drives the port, so reads float */
static uint64_t panel_code_read(void *opaque, hwaddr addr, unsigned size)
{
    return UINT32_MAX;
}

static void panel_code_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    PrepPanelState *s = opaque;

    s->code = deposit32(s->code, addr * 8, size * 8, val);
    s->code_valid = true;
    s->aix = size == 2;
    s->redraw = true;
    trace_prep_panel_code(s->code, size);
    if (size == 4) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        s->res_until = now + RES_POLL_FOR_MS;
        if (!timer_pending(s->res_timer)) {
            timer_mod(s->res_timer, now + RES_POLL_MS);
        }
    }
}

static const MemoryRegionOps panel_code_ops = {
    .read = panel_code_read,
    .write = panel_code_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* The window follows the graphics adapter, which stays the main display */
static void panel_machine_done(Notifier *notifier, void *data)
{
    PrepPanelState *s = container_of(notifier, PrepPanelState, machine_done);
    DeviceState *dev = DEVICE(s);

    s->con = qemu_graphic_console_create(dev, 0, &panel_gfx_ops, s);
    qemu_console_resize(s->con, PANEL_W, PANEL_H);
    s->input = qemu_input_handler_register(dev, &panel_input);
    qemu_input_handler_bind(s->input, dev->id, 0, &error_abort);
}

static void panel_reset(DeviceState *dev)
{
    PrepPanelState *s = PREP_PANEL(dev);

    s->code_valid = false;
    s->aix = false;
    s->disk = false;
    s->disk_until = 0;
    s->redraw = true;
    timer_del(s->res_timer);
    s->res_until = 0;
    s->res_crc = 0;
}

static void panel_realize(DeviceState *dev, Error **errp)
{
    PrepPanelState *s = PREP_PANEL(dev);

    memory_region_init_io(&s->io, OBJECT(dev), &panel_code_ops, s,
                          "prep-panel", 4);
    isa_register_ioport(ISA_DEVICE(dev), &s->io, 0x680);
    qdev_init_gpio_in(dev, panel_disk_light, 1);
    s->res_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, panel_res_timer, s);
    s->machine_done.notify = panel_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void panel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "IBM 40P front panel";
    dc->realize = panel_realize;
    device_class_set_legacy_reset(dc, panel_reset);
    /* the machine wires the disk light and names the window */
    dc->user_creatable = false;
}

static const TypeInfo panel_info = {
    .name = TYPE_PREP_PANEL,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(PrepPanelState),
    .class_init = panel_class_init,
};

static void panel_register_types(void)
{
    type_register_static(&panel_info);
}

type_init(panel_register_types)
