/*
 * IBM RS/6000 7006 operator panel
 *
 * The word at NVRAM offset 0x300 drives the three-digit display: its top
 * three nibbles are the digits, and a nibble of 0xf leaves its digit dark.
 * The key mode switch is read in the system registers. In Secure the ROS
 * stops at 200; Normal boots from the normal boot list, and Service from
 * the service list or the service load of the boot record (diagnostics).
 *
 * The panel has a window of its own, where a click on a position turns
 * the key. The "key" property sets it from the command line or monitor.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/vgafont.h"
#include "hw/ppc/rs6k_panel.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(PanelState, RS6K_PANEL)

struct PanelState {
    SysBusDevice parent_obj;

    MemoryRegion led_io;
    uint32_t led;
    unsigned key;
    QemuConsole *con;
    QemuInputHandlerState *input;
    Notifier machine_done;
    int pointer_x;
    bool click;
    bool redraw;
};

#define PANEL_W     420
#define PANEL_H     120

/* digits: segment centre lines DIGIT_W by DIGIT_H, SEG_T thick */
#define DIGIT_X     40
#define DIGIT_Y     28
#define DIGIT_W     36
#define DIGIT_H     64
#define DIGIT_STEP  60
#define SEG_T       8
#define BEZEL_X     20
#define BEZEL_Y     12
#define BEZEL_W     196
#define BEZEL_H     96

/* key: dial centre and radius, position labels above it */
#define KEY_X       318
#define KEY_Y       78
#define KEY_R       24
#define LABEL_DX    62
#define LABEL_Y     14

#define COL_CASE        0x2a2a2a
#define COL_EDGE        0x5a5a5a
#define COL_BEZEL       0x050505
#define COL_SEG_ON      0xff3018
#define COL_SEG_OFF     0x2a0a06
#define COL_LABEL       0x808080
#define COL_LABEL_ON    0xffffff
#define COL_DIAL        0xb4b4b4
#define COL_RIM         0x6e6e6e
#define COL_SLOT        0x1a1a1a

/* segments a-g in bits 0-6 */
static const uint8_t panel_font[16] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07,
    0x7f, 0x6f, 0x77, 0x7c, 0x58, 0x5e, 0x79, 0x00,
};

/* segment ends as digit column 0-1 and row 0-2 */
static const uint8_t panel_segs[7][4] = {
    { 0, 0, 1, 0 }, { 1, 0, 1, 1 }, { 1, 1, 1, 2 }, { 0, 2, 1, 2 },
    { 0, 1, 0, 2 }, { 0, 0, 0, 1 }, { 0, 1, 1, 1 },
};

static const char *const key_names[] = {
    [RS6K_KEY_SECURE] = "secure",
    [RS6K_KEY_SERVICE] = "service",
    [RS6K_KEY_NORMAL] = "normal",
};

static const int key_label_x[] = {
    [RS6K_KEY_SECURE] = KEY_X - LABEL_DX,
    [RS6K_KEY_SERVICE] = KEY_X + LABEL_DX,
    [RS6K_KEY_NORMAL] = KEY_X,
};

/* where the key slot points, scaled by 256 */
static const int key_dir[][2] = {
    [RS6K_KEY_SECURE] = { -190, -172 },
    [RS6K_KEY_SERVICE] = { 190, -172 },
    [RS6K_KEY_NORMAL] = { 0, -256 },
};

unsigned rs6k_panel_key(DeviceState *dev)
{
    return RS6K_PANEL(dev)->key;
}

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

static void panel_draw_key(PanelState *s, uint32_t *fb)
{
    const int *u = key_dir[s->key];

    for (int dy = -KEY_R; dy <= KEY_R; dy++) {
        for (int dx = -KEY_R; dx <= KEY_R; dx++) {
            int r2 = dx * dx + dy * dy;
            int along = dx * u[0] + dy * u[1];
            int across = abs(dy * u[0] - dx * u[1]);
            uint32_t c = COL_DIAL;

            if (r2 > KEY_R * KEY_R) {
                continue;
            }
            if (along >= 0 && along <= (KEY_R - 5) * 256 && across <= 3 * 256) {
                c = COL_SLOT;
            } else if (r2 > (KEY_R - 3) * (KEY_R - 3)) {
                c = COL_RIM;
            }
            fb[(KEY_Y + dy) * PANEL_W + KEY_X + dx] = c;
        }
    }
}

static void panel_draw(PanelState *s, uint32_t *fb)
{
    panel_fill(fb, 0, 0, PANEL_W, PANEL_H, COL_CASE);
    panel_fill(fb, BEZEL_X - 1, BEZEL_Y - 1, BEZEL_W + 2, BEZEL_H + 2,
               COL_EDGE);
    panel_fill(fb, BEZEL_X, BEZEL_Y, BEZEL_W, BEZEL_H, COL_BEZEL);
    for (int i = 0; i < 3; i++) {
        uint8_t on = panel_font[extract32(s->led, 28 - 4 * i, 4)];
        int x = DIGIT_X + i * DIGIT_STEP;

        for (int j = 0; j < 7; j++) {
            const uint8_t *e = panel_segs[j];

            panel_seg(fb, x + e[0] * DIGIT_W, DIGIT_Y + e[1] * DIGIT_H / 2,
                      x + e[2] * DIGIT_W, DIGIT_Y + e[3] * DIGIT_H / 2,
                      on & BIT(j) ? COL_SEG_ON : COL_SEG_OFF);
        }
    }
    for (unsigned k = RS6K_KEY_SECURE; k <= RS6K_KEY_NORMAL; k++) {
        const char *name = key_names[k];

        panel_text(fb, key_label_x[k] - (int)strlen(name) * FONT_WIDTH / 2,
                   LABEL_Y, name, k == s->key ? COL_LABEL_ON : COL_LABEL);
    }
    panel_draw_key(s, fb);
}

static bool panel_update_display(void *opaque)
{
    PanelState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

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
    PanelState *s = opaque;

    s->redraw = true;
}

static const GraphicHwOps panel_gfx_ops = {
    .gfx_update = panel_update_display,
    .invalidate = panel_invalidate_display,
};

static void panel_turn_key(PanelState *s, unsigned key)
{
    if (s->key != key) {
        trace_rs6k_key(key_names[key]);
    }
    s->key = key;
    s->redraw = true;
}

static void panel_input_event(DeviceState *dev, QemuConsole *src,
                              QemuInputEvent *evt)
{
    PanelState *s = RS6K_PANEL(dev);

    if (evt->type == INPUT_EVENT_KIND_ABS && evt->abs.axis == INPUT_AXIS_X) {
        s->pointer_x = qemu_input_scale_axis(evt->abs.value,
                                             INPUT_EVENT_ABS_MIN,
                                             INPUT_EVENT_ABS_MAX, 0, PANEL_W);
    } else if (evt->type == INPUT_EVENT_KIND_BTN &&
               evt->btn.button == INPUT_BUTTON_LEFT && evt->btn.down) {
        s->click = true;
    }
}

/* the button comes before the pointer position of the same click */
static void panel_input_sync(DeviceState *dev)
{
    PanelState *s = RS6K_PANEL(dev);
    int x = s->pointer_x;

    if (!s->click) {
        return;
    }
    s->click = false;
    if (x < BEZEL_X + BEZEL_W) {
        return;
    }
    panel_turn_key(s, x < KEY_X - LABEL_DX / 2 ? RS6K_KEY_SECURE :
                      x > KEY_X + LABEL_DX / 2 ? RS6K_KEY_SERVICE :
                      RS6K_KEY_NORMAL);
}

static const QemuInputHandler panel_input = {
    .name = "rs6k operator panel",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = panel_input_event,
    .sync = panel_input_sync,
};

static uint64_t panel_led_read(void *opaque, hwaddr addr, unsigned size)
{
    PanelState *s = opaque;

    return extract32(s->led, (4 - addr - size) * 8, size * 8);
}

static void panel_led_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    PanelState *s = opaque;

    s->led = deposit32(s->led, (4 - addr - size) * 8, size * 8, val);
    s->redraw = true;
    trace_rs6k_led(s->led >> 20);
}

static const MemoryRegionOps panel_led_ops = {
    .read = panel_led_read,
    .write = panel_led_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* The window follows the graphics adapters, which stay the main display */
static void panel_machine_done(Notifier *notifier, void *data)
{
    PanelState *s = container_of(notifier, PanelState, machine_done);
    DeviceState *dev = DEVICE(s);

    s->con = qemu_graphic_console_create(dev, 0, &panel_gfx_ops, s);
    qemu_console_resize(s->con, PANEL_W, PANEL_H);
    s->input = qemu_input_handler_register(dev, &panel_input);
    qemu_input_handler_bind(s->input, dev->id, 0, &error_abort);
}

static void panel_realize(DeviceState *dev, Error **errp)
{
    PanelState *s = RS6K_PANEL(dev);

    memory_region_init_io(&s->led_io, OBJECT(dev), &panel_led_ops, s,
                          "rs6k.led", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->led_io);
    s->led = ~0;
    s->redraw = true;
    s->machine_done.notify = panel_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static char *panel_get_key(Object *obj, Error **errp)
{
    return g_strdup(key_names[RS6K_PANEL(obj)->key]);
}

static void panel_set_key(Object *obj, const char *value, Error **errp)
{
    for (unsigned k = RS6K_KEY_SECURE; k <= RS6K_KEY_NORMAL; k++) {
        if (!strcmp(value, key_names[k])) {
            panel_turn_key(RS6K_PANEL(obj), k);
            return;
        }
    }
    error_setg(errp, "key must be normal, service or secure");
}

static void panel_instance_init(Object *obj)
{
    RS6K_PANEL(obj)->key = RS6K_KEY_NORMAL;
}

static void panel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "IBM RS/6000 7006 operator panel";
    dc->realize = panel_realize;
    object_class_property_add_str(klass, "key", panel_get_key, panel_set_key);
    object_class_property_set_description(klass, "key",
                                          "normal, service or secure");
}

static const TypeInfo panel_info = {
    .name = TYPE_RS6K_PANEL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PanelState),
    .instance_init = panel_instance_init,
    .class_init = panel_class_init,
};

static void panel_register_types(void)
{
    type_register_static(&panel_info);
}

type_init(panel_register_types)
