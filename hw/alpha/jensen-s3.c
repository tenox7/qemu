/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Jensen ISA S3 86C928, experimental graphics subset.
 *
 * Register decoding, locking and raster operations adapted from 86Box
 * src/video/vid_s3.c and src/video/ramdac/vid_ramdac_bt48x.c:
 * Copyright 2008-2019 Sarah Walker.
 * Copyright 2016-2019 Miran Grca.
 * Copyright 2018 TheCollector1995.
 * Adapted to QEMU's VGA core and synchronous, bounded command execution.
 * Reference: S3 DB02-C, 86C928 GUI Accelerator, September 1992.
 * No vendor option ROM is included; the board retains its VGA console ROM.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/isa/isa.h"
#include "../display/vga_int.h"
#include "trace.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"

#define TYPE_JENSEN_S3 "jensen-s3-928"
OBJECT_DECLARE_SIMPLE_TYPE(JensenS3State, JENSEN_S3)

typedef struct JensenS3Port {
    MemoryRegion mr;
    JensenS3State *state;
    unsigned port;
} JensenS3Port;

struct JensenS3State {
    ISADevice parent_obj;
    VGACommonState vga;
    GraphicHwOps hw_ops;
    MemoryRegion ports, aperture;
    JensenS3Port accel[19];
    int (*vga_bpp)(VGACommonState *);
    void (*vga_params)(VGACommonState *, VGADisplayParams *);
    void (*vga_resolution)(VGACommonState *, int *, int *);
    uint32_t reg[17];
    uint16_t multi[16], multi_latch;
    uint8_t dac_command[4], cursor_ram[1024], cursor32_ram[256];
    uint8_t cursor_palette[12];
    uint32_t hwcursor_fg, hwcursor_bg;
    uint8_t hwcursor_color_index;
    int cursor_last_y, cursor_last_size;
    uint16_t dac_addr, cursor_x, cursor_y;
    uint8_t dac_component;
    uint32_t progress, transfer_data;
    unsigned transfer_bytes;
    int line_x, line_y, line_error;
    bool pending, unsupported;
};

static const uint16_t s3_ports[] = {
    0x42e8, 0x4ae8, 0x82e8, 0x86e8, 0x8ae8, 0x8ee8,
    0x92e8, 0x96e8, 0x9ae8, 0x9ee8, 0xa2e8, 0xa6e8,
    0xaae8, 0xaee8, 0xb6e8, 0xbae8, 0xb2e8,
};

enum {
    SUBSYS, ADVFUNC, CUR_Y, CUR_X, DEST_Y, DEST_X, ERROR_TERM,
    MAJOR, COMMAND, SHORT_STROKE, BACKGROUND, FOREGROUND,
    WRITE_MASK, READ_MASK, BACK_MIX, FORE_MIX, COLOR_COMPARE,
};

static bool s3_packed(JensenS3State *s)
{
    return s->vga.cr[0x31] & 8;
}

static unsigned s3_pitch(JensenS3State *s)
{
    unsigned extension = s->vga.cr[0x51] & 0x30;
    unsigned offset = s->vga.cr[0x13] |
        (extension ? extension << 4 : (s->vga.cr[0x43] & 4) << 6);
    return offset * 8;
}

static bool s3_cursor_external(JensenS3State *s)
{
    /* Same Bt485 routing as the Metheus 928 in 86Box. */
    return s->vga.cr[0x55] & 0x28;
}

/* Bt485 coordinates include a one-cursor-width/height bias. */
static unsigned s3_cursor_size(JensenS3State *s)
{
    return !s3_cursor_external(s) || (s->dac_command[3] & 4) ? 64 : 32;
}

static uint8_t *s3_cursor_ram(JensenS3State *s)
{
    return s->dac_command[3] & 4 ? s->cursor_ram : s->cursor32_ram;
}

static bool s3_cursor_enabled(JensenS3State *s)
{
    if (s3_cursor_external(s)) {
        return s->dac_command[2] & 3;
    }
    return s3_packed(s) && (s->vga.cr[0x45] & 1);
}

static int s3_cursor_top(JensenS3State *s)
{
    return s3_cursor_external(s) ? (int)s->cursor_y - s3_cursor_size(s) :
        ((s->vga.cr[0x48] & 7) << 8) | s->vga.cr[0x49];
}

static void s3_cursor_invalidate(VGACommonState *vga)
{
    JensenS3State *s = container_of(vga, JensenS3State, vga);
    int size = s3_cursor_size(s);
    int y = s3_cursor_top(s);

    /*
     * Redraw the framebuffer under both positions before applying the cursor.
     * This also keeps XOR pixels stable on repeated refreshes with clean VRAM.
     */
    vga_invalidate_scanlines(vga, MAX(0, s->cursor_last_y),
                             MAX(0, s->cursor_last_y + s->cursor_last_size));
    s->cursor_last_size = s3_cursor_enabled(s) ? size : 0;
    s->cursor_last_y = y;
    vga_invalidate_scanlines(vga, MAX(0, y),
                             MAX(0, y + s->cursor_last_size));
}

/* S3 DB02-C 11.3.13: interleaved 16-bit AND/XOR words in offscreen VRAM. */
static void s3_vram_cursor_line(JensenS3State *s, uint32_t *pixels, int y,
                                int width, int bpp)
{
    VGACommonState *vga = &s->vga;
    int row = y - s3_cursor_top(s) + (vga->cr[0x4f] & 63);
    int x = ((vga->cr[0x46] & 7) << 8) | vga->cr[0x47];
    int xoff = vga->cr[0x4e] & 63;
    unsigned step = (bpp == 15 || bpp == 16) && !(vga->cr[0x45] & 4) ? 2 : 1;
    unsigned base = (((vga->cr[0x4c] << 8) | vga->cr[0x4d]) & 0xfff) * 1024;
    uint32_t colors[2] = { s->hwcursor_bg, s->hwcursor_fg };
    int i;

    if (!s3_cursor_enabled(s) || y < s3_cursor_top(s) || row >= 64) {
        return;
    }
    /* 928 true-color cursor X is counted in two-byte units. */
    if (bpp == 15 || bpp == 16 || bpp == 32) {
        x >>= 1;
    }
    for (i = 0; i < 2; i++) {
        uint32_t color = colors[i];
        if (bpp == 8) {
            const uint8_t *rgb =
                &vga->palette[vga->cr[i ? 0x0e : 0x0f] * 3];
            colors[i] = vga->dac_8bit ? rgb_to_pixel32(rgb[0], rgb[1], rgb[2]) :
                rgb_to_pixel32(c6_to_8(rgb[0]), c6_to_8(rgb[1]),
                               c6_to_8(rgb[2]));
        } else if (bpp == 15 || bpp == 16) {
            unsigned red = (color >> (bpp == 15 ? 10 : 11)) & 31;
            unsigned green = (color >> 5) & (bpp == 15 ? 31 : 63);
            unsigned blue = color & 31;
            colors[i] = rgb_to_pixel32((red << 3) | (red >> 2),
                bpp == 15 ? (green << 3) | (green >> 2) :
                            (green << 2) | (green >> 4),
                (blue << 3) | (blue >> 2));
        }
    }
    for (i = 0; x + i < width && xoff + i * step < 64; i++) {
        unsigned col = xoff + i * step;
        unsigned addr = base + row * 16 + (col / 16) * 4;
        unsigned bit = 7 - (col & 7);
        unsigned and_bit, xor_bit;

        /* CR45.4 stores the masks at the right of 1280-pixel scanlines. */
        if (vga->cr[0x45] & 0x10) {
            if (vga->cr[0x3a] & 0x10) {
                addr = (addr & ~0xe00) | ((addr & 0x200) << 2) | 0x600;
            } else if ((vga->gr[5] & 0x60) == 0x20) {
                addr = (addr & ~0xf00) | ((addr & 0x300) << 2) | 0x300;
            }
        }
        addr += (col & 15) / 8;
        if (addr + 2 >= vga->vram_size) {
            continue;
        }
        and_bit = (vga->vram_ptr[addr] >> bit) & 1;
        xor_bit = (vga->vram_ptr[addr + 2] >> bit) & 1;
        if (vga->cr[0x55] & 0x10) {
            if (and_bit) {
                pixels[x + i] = colors[xor_bit];
            }
        } else if (!and_bit) {
            pixels[x + i] = colors[xor_bit];
        } else if (xor_bit) {
            pixels[x + i] ^= 0x00ffffff;
        }
    }
}

static void s3_cursor_draw_line(VGACommonState *vga, uint8_t *dest, int y)
{
    JensenS3State *s = container_of(vga, JensenS3State, vga);
    int size = s3_cursor_size(s);
    int x = (int)s->cursor_x - size;
    int row = y - ((int)s->cursor_y - size);
    unsigned pitch = size / 8;
    unsigned plane = pitch * size;
    unsigned mode = s->dac_command[2] & 3;
    uint8_t *ram = s3_cursor_ram(s);
    uint32_t colors[4], *pixels = (uint32_t *)dest;
    int first = MAX(0, -x);
    int width = surface_width(qemu_console_surface(vga->con));
    int last = MIN((int)size, width - x);
    int bpp = vga->get_bpp(vga);
    int i;

    /*
     * Allocated VGA surfaces truncate 5/6-bit channels, whereas shared
     * RGB555/RGB565 surfaces expand them in pixman. Preserve the latter's
     * colors when using a shadow surface for safe cursor composition.
     */
    if (bpp == 15 || bpp == 16) {
        for (i = 0; i < width; i++) {
            uint32_t pixel = pixels[i];
            pixels[i] = pixel | ((pixel >> 5) & 0x00070007) |
                ((pixel >> (bpp == 15 ? 5 : 6)) &
                 (bpp == 15 ? 0x00000700 : 0x00000300));
        }
    }

    if (!s3_cursor_external(s)) {
        s3_vram_cursor_line(s, pixels, y, width, bpp);
        return;
    }
    if (!s3_cursor_enabled(s) || row < 0 || row >= size) {
        return;
    }
    for (i = 1; i <= 3; i++) {
        const uint8_t *rgb = &s->cursor_palette[i * 3];
        colors[i] = vga->dac_8bit ? rgb_to_pixel32(rgb[0], rgb[1], rgb[2]) :
            rgb_to_pixel32(c6_to_8(rgb[0]), c6_to_8(rgb[1]), c6_to_8(rgb[2]));
    }
    /* MSB-first, two separate one-bit planes, as in 86Box's Bt48x DAC. */
    for (i = first; i < last; i++) {
        unsigned offset = row * pitch + i / 8;
        unsigned bit = 7 - (i & 7);
        unsigned value = ((ram[offset] >> bit) & 1) |
                         (((ram[offset + plane] >> bit) & 1) << 1);
        switch (mode) {
        case 1: /* Three colors: zero is transparent. */
            if (value) {
                pixels[x + i] = colors[value];
            }
            break;
        case 2: /* Windows: background, foreground, transparent, invert. */
            if (value < 2) {
                pixels[x + i] = colors[value + 1];
            } else if (value == 3) {
                pixels[x + i] ^= 0x00ffffff;
            }
            break;
        case 3: /* X Windows: plane 1 enables the pixel. */
            if (value & 2) {
                pixels[x + i] = colors[(value & 1) + 1];
            }
            break;
        }
    }
}

/* Enhanced scanout bypasses the VGA Graphics Controller shift mode. */
static bool s3_update_display(void *opaque)
{
    VGACommonState *vga = opaque;
    JensenS3State *s = container_of(vga, JensenS3State, vga);
    uint8_t mode = vga->gr[5];

    if (s3_packed(s) &&
        ((s->reg[ADVFUNC] & 1) || (vga->cr[0x40] & 8))) {
        vga->gr[5] = (mode & ~0x60) | 0x40;
    }
    /* The VGA renderer is synchronous under the BQL. Keep guest readback. */
    vga->hw_ops->gfx_update(vga);
    vga->gr[5] = mode;
    return true;
}

static int s3_bpp(VGACommonState *vga)
{
    JensenS3State *s = container_of(vga, JensenS3State, vga);
    if (!s3_packed(s)) {
        return s->vga_bpp(vga);
    }
    /* Bt485 24-bit RGB input occupies a 32-bit framebuffer word. */
    if (!(s->dac_command[2] & 0x20)) {
        return 8;
    }
    switch (s->dac_command[1] & 0x60) {
    case 0x00:
        return 32;
    case 0x20:
        return s->dac_command[1] & 8 ? 16 : 15;
    case 0x40:
        return 8;
    default:
        return 0; /* Four-bit VGA shift mode. */
    }
}

static void s3_params(VGACommonState *vga, VGADisplayParams *params)
{
    JensenS3State *s = container_of(vga, JensenS3State, vga);
    s->vga_params(vga, params);
    if (s3_packed(s)) {
        params->line_offset = s3_pitch(s);
        params->start_addr |= ((vga->cr[0x31] & 0x30) << 12) |
                              ((vga->cr[0x51] & 3) << 18);
        params->line_compare |= (vga->cr[0x5e] & 0x40) << 4;
        if ((s->reg[ADVFUNC] & 1) || (vga->cr[0x40] & 8)) {
            params->line_compare = 0x7fff;
        }
    }
}

static void s3_resolution(VGACommonState *vga, int *width, int *height)
{
    JensenS3State *s = container_of(vga, JensenS3State, vga);
    s->vga_resolution(vga, width, height);
    if (s3_packed(s)) {
        *width += (vga->cr[0x5d] & 2) << 10;
        *height += (vga->cr[0x5e] & 2) << 9;
        if (vga->cr[0x43] & 0x80) {
            *width *= 2;
        }
        /*
         * The Metheus 928 uses a Bt485 and ICS2494 clock generator.
         * In enhanced mode the DAC's input format determines how many
         * pixels it emits per CRTC clock: four for 8-bit multiplexing,
         * two for 15/16-bit multiplexing. The Bt485 clock doubler changes
         * the output frequency, not this pixel count. See 86Box's
         * S3_86C928/BT48X/ICS2494 timing calculation.
         */
        if (s->dac_command[2] & 0x20) {
            switch (s->dac_command[1] & 0x60) {
            case 0x40:
                *width *= 4;
                break;
            case 0x20:
                *width *= 2;
                break;
            }
        }
    }
}

/* The sixteen S3 mix functions, independent of the selected pixel source. */
static uint32_t s3_mix(unsigned mix, uint32_t src, uint32_t dst)
{
    switch (mix & 15) {
    case 0:
        return ~dst;
    case 1:
        return 0;
    case 2:
        return UINT32_MAX;
    case 3:
        return dst;
    case 4:
        return ~src;
    case 5:
        return src ^ dst;
    case 6:
        return ~(src ^ dst);
    case 7:
        return src;
    case 8:
        return ~(src & dst);
    case 9:
        return ~src | dst;
    case 10:
        return src | ~dst;
    case 11:
        return src | dst;
    case 12:
        return src & dst;
    case 13:
        return src & ~dst;
    case 14:
        return ~src & dst;
    case 15:
        return ~(src | dst);
    }
    g_assert_not_reached();
}

static unsigned s3_engine_pitch(JensenS3State *s)
{
    /* CR50 sets the engine stride independently of the display offset. */
    switch (s->vga.cr[0x50] & 0xc1) {
    case 0x00:
        return s->vga.cr[0x31] & 2 ? 2048 : 1024;
    case 0x01:
        return 1152;
    case 0x40:
        return 640;
    case 0x80:
        return 800;
    case 0x81:
        return 1600;
    case 0xc0:
        return 1280;
    default:
        return 1024;
    }
}

static unsigned s3_pixel_bytes(JensenS3State *s)
{
    return 1 + ((s->vga.cr[0x50] >> 4) & 3);
}

/* A pixel can straddle the end of installed VRAM. */
static uint32_t s3_load_pixel(JensenS3State *s, uint32_t addr)
{
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < s3_pixel_bytes(s); i++) {
        value |= (uint32_t)s->vga.vram_ptr[(addr + i) &
                 (s->vga.vram_size - 1)] << (i * 8);
    }
    return value;
}

static uint32_t s3_address(JensenS3State *s, int x, int y, bool source)
{
    unsigned base = (s->multi[14] >> (source ? 2 : 0)) & 3;
    return (base * MiB + (y * s3_engine_pitch(s) + x) * s3_pixel_bytes(s)) &
           (s->vga.vram_size - 1);
}

static void s3_pixel(JensenS3State *s, int x, int y, uint32_t source,
                     bool foreground, uint32_t cpu)
{
    unsigned mix = s->reg[foreground ? FORE_MIX : BACK_MIX];
    uint32_t addr;
    unsigned bytes = s3_pixel_bytes(s);
    uint32_t old, value, mask = s->reg[WRITE_MASK];
    unsigned i;
    bool inside = x >= s->multi[2] && x <= s->multi[4] &&
                  y >= s->multi[1] && y <= s->multi[3];
    if (!(s->reg[COMMAND] & 0x10) || inside == !!(s->multi[14] & 0x20)) {
        return;
    }
    switch ((mix >> 5) & 3) {
    case 0:
        source = s->reg[BACKGROUND];
        break;
    case 1:
        source = s->reg[FOREGROUND];
        break;
    case 2:
        source = cpu;
        break;
    case 3:
        break;
    }
    if (s->multi[14] & 0x100) {
        uint32_t planes = bytes == 4 ? UINT32_MAX : (1U << (bytes * 8)) - 1;
        bool equal = ((source ^ s->reg[COLOR_COMPARE]) & planes) == 0;
        if (equal != !!(s->multi[14] & 0x80)) {
            return;
        }
    }
    addr = s3_address(s, x, y, false);
    old = s3_load_pixel(s, addr);
    value = s3_mix(mix, source, old);
    value = (old & ~mask) | (value & mask);
    for (i = 0; i < bytes; i++) {
        uint32_t byte_addr = (addr + i) & (s->vga.vram_size - 1);
        s->vga.vram_ptr[byte_addr] = value >> (i * 8);
        memory_region_set_dirty(&s->vga.vram, byte_addr, 1);
    }
}

static int s3_signed14(uint32_t value)
{
    return (int)(value & 0x1fff) - (int)(value & 0x2000);
}

static const int8_t s3_line_dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
static const int8_t s3_line_dy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };

static void s3_line_step(JensenS3State *s, uint32_t cpu, bool foreground)
{
    unsigned command = s->reg[COMMAND];
    unsigned length = s->reg[MAJOR] & 0xfff;
    bool last = s->progress == length;
    int dx = command & 0x20 ? 1 : -1;
    int dy = command & 0x80 ? 1 : -1;

    if (!(last && (command & 4))) {
        s3_pixel(s, s->line_x, s->line_y, 0, foreground, cpu);
    }
    if (!last) {
        if (command & 8) {
            unsigned direction = (command >> 5) & 7;
            s->line_x += s3_line_dx[direction];
            s->line_y += s3_line_dy[direction];
        } else {
            /* The S3 error threshold is MAJ_AXIS_PCNT, as in 86Box. */
            bool diagonal = s->line_error >= (int)length;
            if (!(command & 0x40) || diagonal) {
                s->line_x += dx;
            }
            if ((command & 0x40) || diagonal) {
                s->line_y += dy;
            }
            s->line_error = (int16_t)(s->line_error +
                s3_signed14(s->reg[diagonal ? DEST_X : DEST_Y]));
        }
        s->line_x &= 0xfff;
        s->line_y &= 0xfff;
    }
    s->progress++;
    if (last) {
        s->reg[CUR_X] = s->line_x;
        s->reg[CUR_Y] = s->line_y;
        s->reg[ERROR_TERM] = s->line_error & 0x3fff;
        s->pending = false;
    }
}

static void s3_step(JensenS3State *s, uint32_t cpu, bool foreground)
{
    unsigned width = (s->reg[MAJOR] & 0xfff) + 1;
    int dx = s->reg[COMMAND] & 0x20 ? 1 : -1;
    int dy = s->reg[COMMAND] & 0x80 ? 1 : -1;
    int x = (s->reg[CUR_X] & 0xfff) + (s->progress % width) * dx;
    int y = (s->reg[CUR_Y] & 0xfff) + (s->progress / width) * dy;
    uint32_t source = 0;
    unsigned op = s->reg[COMMAND] >> 13;
    if (op == 1) {
        s3_line_step(s, cpu, foreground);
        return;
    }
    if (op == 6 || op == 7) {
        uint32_t addr;
        int dest_x = (s->reg[DEST_X] & 0xfff) + (s->progress % width) * dx;
        int dest_y = (s->reg[DEST_Y] & 0xfff) + (s->progress / width) * dy;
        if (op == 7) {
            x = (s->reg[CUR_X] & 0xfff) + (dest_x & 7);
            y = (s->reg[CUR_Y] & 0xfff) + (dest_y & 7);
        }
        addr = s3_address(s, x, y, true);
        source = s3_load_pixel(s, addr);
        x = dest_x;
        y = dest_y;
        if ((s->multi[10] & 0xc0) == 0xc0) {
            uint32_t mask = s->reg[READ_MASK];
            if (s3_pixel_bytes(s) < 4) {
                mask &= (1U << (s3_pixel_bytes(s) * 8)) - 1;
            }
            foreground = (source & mask) == mask;
            source = foreground;
        }
    }
    s3_pixel(s, x, y, source, foreground, cpu);
    s->progress++;
    if (s->progress == width * ((s->multi[0] & 0xfff) + 1)) {
        if (!(s->reg[COMMAND] & 0x100)) {
            unsigned height = (s->multi[0] & 0xfff) + 1;
            if (op == 2) {
                s->reg[CUR_Y] = (s->reg[CUR_Y] + height * dy) & 0xfff;
            } else if (op == 6 || op == 7) {
                s->reg[DEST_Y] = (s->reg[DEST_Y] + height * dy) & 0xfff;
            }
        }
        s->pending = false;
    }
}

static void s3_command(JensenS3State *s)
{
    unsigned op = s->reg[COMMAND] >> 13;
    s->progress = 0;
    s->pending = false;
    s->unsupported = false;
    s->transfer_data = s->transfer_bytes = 0;
    s->line_x = s->reg[CUR_X] & 0xfff;
    s->line_y = s->reg[CUR_Y] & 0xfff;
    s->line_error = s3_signed14(s->reg[ERROR_TERM]);
    trace_jensen_s3_command(s->reg[COMMAND], s->reg[CUR_X], s->reg[CUR_Y],
                            s->reg[MAJOR] + 1, s->multi[0] + 1);
    trace_jensen_s3_state(s->reg[COMMAND], s->reg[CUR_X], s->reg[CUR_Y],
                         s->reg[DEST_X], s->reg[DEST_Y],
                         (s->reg[MAJOR] & 0xfff) + 1,
                         (s->multi[0] & 0xfff) + 1, s->multi[10],
                         s->reg[FORE_MIX] | (s->reg[BACK_MIX] << 16),
                         s->reg[WRITE_MASK] | (s->vga.cr[0x50] << 16));
    if (op == 0) {
        return;
    }
    if (op != 1 && op != 2 && op != 6 && op != 7) {
        s->unsupported = true;
        qemu_log_mask(LOG_UNIMP,
                      "Jensen S3: unsupported command/depth %04x/%02x\n",
                      s->reg[COMMAND], s->vga.cr[0x50]);
        return;
    }
    if ((s->reg[COMMAND] & 0x101) == 0x100 && op != 2) {
        s->unsupported = true;
        qemu_log_mask(LOG_UNIMP,
                      "Jensen S3: unsupported CPU read command %04x\n",
                      s->reg[COMMAND]);
        return;
    }
    s->pending = true;
    if (!(s->reg[COMMAND] & 0x100)) {
        while (s->pending) {
            s3_step(s, 0, true);
        }
    }
}

static void s3_transfer(JensenS3State *s, uint32_t value, unsigned size)
{
    unsigned i, bits = MIN(size * 8,
                           8U << ((s->reg[COMMAND] >> 9) & 3));
    unsigned bytes = s3_pixel_bytes(s);
    bool mono = ((s->multi[10] & 0xc0) == 0x80 ||
                (s->reg[COMMAND] & 2)) &&
                !((s->reg[FORE_MIX] & 0x60) == 0x40 &&
                  (s->reg[BACK_MIX] & 0x60) == 0x40);
    unsigned width = (s->reg[MAJOR] & 0xfff) + 1;
    unsigned row = s->progress / width;
    if (!s->pending || (s->reg[COMMAND] & 0x101) != 0x101) {
        return;
    }
    if ((s->reg[COMMAND] & 0x1000) && bits >= 16 &&
        (mono || bytes == 1)) {
        value = ((value & 0xff00ff00) >> 8) | ((value & 0x00ff00ff) << 8);
    }
    for (i = 0; i < (mono ? bits : bits / 8) && s->pending; i++) {
        if (mono) {
            s3_step(s, 0, !!(value & (1U << (bits - 1 - i))));
        } else {
            unsigned offset = bytes == 1 ? bits - 8 - i * 8 : i * 8;
            s->transfer_data |= ((value >> offset) & 0xff) <<
                                (s->transfer_bytes * 8);
            if (++s->transfer_bytes < bytes) {
                continue;
            }
            s3_step(s, s->transfer_data, true);
            s->transfer_data = s->transfer_bytes = 0;
        }
        /* CPU rectangle rows are padded to the transfer width. */
        if ((s->reg[COMMAND] >> 13) != 1 && s->progress / width != row) {
            break;
        }
    }
}

static uint32_t s3_receive(JensenS3State *s, unsigned size)
{
    unsigned bits = MIN(size * 8, 8U << ((s->reg[COMMAND] >> 9) & 3));
    unsigned bytes = s3_pixel_bytes(s);
    unsigned width = (s->reg[MAJOR] & 0xfff) + 1;
    unsigned row = s->progress / width;
    bool mono = (s->multi[10] & 0xc0) == 0xc0;
    uint32_t value = 0;
    unsigned i;

    if (!s->pending || (s->reg[COMMAND] & 0x101) != 0x100) {
        return 0;
    }
    for (i = 0; i < (mono ? bits : bits / 8); i++) {
        if (!s->transfer_bytes && !s->pending) {
            break;
        }
        if (!s->transfer_bytes) {
            int dx = s->reg[COMMAND] & 0x20 ? 1 : -1;
            int dy = s->reg[COMMAND] & 0x80 ? 1 : -1;
            int x = (s->reg[CUR_X] + (s->progress % width) * dx) & 0xfff;
            int y = (s->reg[CUR_Y] + (s->progress / width) * dy) & 0xfff;
            s->transfer_data = s3_load_pixel(s, s3_address(s, x, y, false));
            s->transfer_bytes = bytes;
        }
        if (mono) {
            uint32_t mask = s->reg[READ_MASK];
            if (bytes < 4) {
                mask &= (1U << (bytes * 8)) - 1;
            }
            value |= (uint32_t)((s->transfer_data & mask) == mask) <<
                     (bits - 1 - i);
            s->transfer_bytes = 0;
        } else {
            unsigned offset = bytes == 1 ? bits - 8 - i * 8 : i * 8;
            value |= (s->transfer_data & 0xff) << offset;
            s->transfer_data >>= 8;
            s->transfer_bytes--;
        }
        if (!s->transfer_bytes) {
            s->progress++;
            if (s->progress == width * ((s->multi[0] & 0xfff) + 1)) {
                s->pending = false;
            }
            if (s->progress / width != row) {
                break;
            }
        }
    }
    if ((s->reg[COMMAND] & 0x1000) && bits >= 16 && (mono || bytes == 1)) {
        value = ((value & 0xff00ff00) >> 8) | ((value & 0x00ff00ff) << 8);
    }
    return value;
}

static bool s3_color_register(unsigned index)
{
    return index == FOREGROUND || index == BACKGROUND ||
           index == WRITE_MASK || index == READ_MASK || index == COLOR_COMPARE;
}

static uint64_t s3_accel_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenS3State *s = opaque;
    unsigned port = addr, i;
    uint32_t value = 0;
    if (!(s->vga.cr[0x40] & 1)) {
        return MAKE_64BIT_MASK(0, size * 8);
    }
    if ((port & ~3) == 0xe2e8) {
        value = s3_receive(s, size);
        trace_jensen_s3_io(port, value, size, false);
        return value;
    } else if ((port & ~3) == 0xbee8) {
        static const uint8_t selectors[7] = { 0, 1, 2, 3, 4, 10, 14 };
        unsigned index = s->multi[15] & 7;
        value = index < 7 ? s->multi[selectors[index]] : s->reg[COMMAND];
        if (size >= 2 || (port & 1)) {
            s->multi[15] = (index + 1) & 7;
        }
    } else {
        for (i = 0; i < ARRAY_SIZE(s3_ports); i++) {
            if ((port & ~3) == s3_ports[i]) {
                value = i == COMMAND ?
                    ((s->pending || s->unsupported) ? 0x200 : 0) |
                    ((s->pending && !(s->reg[COMMAND] & 1)) ? 0x100 : 0) :
                    s->reg[i];
                if (s3_color_register(i)) {
                    if (size <= 2 && s3_pixel_bytes(s) == 4 &&
                        (s->multi[14] & 0x10)) {
                        value >>= 16;
                    }
                    if (size >= 2 || (port & 1)) {
                        s->multi[14] ^= 0x10;
                    }
                }
                break;
            }
        }
    }
    trace_jensen_s3_io(port, value, size, false);
    return (value >> ((port & 3) * 8)) & MAKE_64BIT_MASK(0, size * 8);
}

/* Radial short vectors used by the NT driver for focus outlines. */
static void s3_short_stroke(JensenS3State *s, uint16_t word)
{
    static const int8_t dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int8_t dy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    unsigned part, i;
    int x = s->reg[CUR_X] & 0xfff;
    int y = s->reg[CUR_Y] & 0xfff;

    if ((s->reg[COMMAND] >> 13) != 0 || !(s->reg[COMMAND] & 8) ||
        (s->reg[COMMAND] & 0x100)) {
        s->unsupported = true;
        qemu_log_mask(LOG_UNIMP, "Jensen S3: unsupported short vector %04x\n",
                      s->reg[COMMAND]);
        return;
    }
    for (part = 0; part < 2; part++) {
        unsigned shift = (s->reg[COMMAND] & 0x1000) ? part * 8 : (1 - part) * 8;
        uint8_t vector = word >> shift;
        unsigned dir = vector >> 5;
        for (i = 0; i <= (vector & 15); i++) {
            if (vector & 0x10) {
                s3_pixel(s, x, y, 0, true, 0);
            }
            if (i != (vector & 15)) {
                x = (x + dx[dir]) & 0xfff;
                y = (y + dy[dir]) & 0xfff;
            }
        }
    }
    s->reg[CUR_X] = x;
    s->reg[CUR_Y] = y;
}

static void s3_accel_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    JensenS3State *s = opaque;
    unsigned port = addr, i, shift = (port & 3) * 8;
    uint16_t word;
    trace_jensen_s3_io(port, value, size, true);
    if (!(s->vga.cr[0x40] & 1)) {
        return;
    }
    if ((port & ~3) == 0xe2e8) {
        s3_transfer(s, value, size);
        return;
    }
    if ((port & ~3) == 0xbee8) {
        /* Indexed multifunction control is committed on the high byte. */
        word = (s->multi_latch &
                ~(MAKE_64BIT_MASK(0, size * 8) << shift)) |
               (value << shift);
        s->multi_latch = word;
        if (size >= 2 || shift == 8) {
            s->multi[word >> 12] = word & 0xfff;
        }
        return;
    }
    for (i = 0; i < ARRAY_SIZE(s3_ports); i++) {
        if ((port & ~3) == s3_ports[i]) {
            if (s3_color_register(i)) {
                if (s3_pixel_bytes(s) == 4 && size <= 2 &&
                    (s->multi[14] & 0x10)) {
                    shift += 16;
                }
                s->reg[i] = (s->reg[i] &
                             ~(MAKE_64BIT_MASK(0, size * 8) << shift)) |
                            (value << shift);
                if (size == 2 || (port & 1)) {
                    s->multi[14] ^= 0x10;
                }
            } else {
                s->reg[i] = (s->reg[i] &
                             ~(MAKE_64BIT_MASK(0, size * 8) << shift)) |
                            (value << shift);
                s->reg[i] &= 0xffff;
            }
            if (i == COMMAND && (size >= 2 || shift == 8)) {
                s3_command(s);
            } else if (i == SHORT_STROKE && (size >= 2 || shift == 8)) {
                s3_short_stroke(s, s->reg[i]);
            } else if (i == SUBSYS && s->reg[i] & 0x8000) {
                s->pending = s->unsupported = false;
            }
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP,
                  "Jensen S3: unsupported accelerator port %04x\n", port);
}

/* Each small port region adds its absolute base to the callback address. */
static uint64_t s3_engine_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenS3Port *p = opaque;
    return s3_accel_read(p->state, p->port + addr, size);
}

static void s3_engine_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    JensenS3Port *p = opaque;
    s3_accel_write(p->state, p->port + addr, value, size);
}

static const MemoryRegionOps s3_accel_ops = {
    .read = s3_engine_read,
    .write = s3_engine_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static unsigned s3_dac_index(JensenS3State *s, unsigned port)
{
    unsigned select = s->vga.cr[0x55] & 3;
    if (!select) {
        select = (s->vga.cr[0x43] & 2) >> 1;
    }
    return (port & 3) | (select << 2);
}

static uint8_t s3_dac_read(JensenS3State *s, unsigned port)
{
    unsigned index = s3_dac_index(s, port);
    uint8_t value;

    switch (index) {
    case 4:
    case 7:
        return s->dac_addr;
    case 5:
        value = s->cursor_palette[((s->dac_addr - 1) & 3) * 3 +
                                  s->dac_component++];
        if (s->dac_component == 3) {
            s->dac_component = 0;
            s->dac_addr = (s->dac_addr + 1) & 1023;
        }
        return s->vga.dac_8bit ? value : value & 63;
    case 6:
        return s->dac_command[0];
    case 8:
        return s->dac_command[1];
    case 9:
        return s->dac_command[2];
    case 10:
        return (s->dac_command[0] & 0x80) && (s->dac_addr & 0x3f) == 1 ?
                   (s->dac_command[3] & ~3) | (s->dac_addr >> 8) :
                   0x60; /* Bt485 status */
    case 11:
        value = s3_cursor_ram(s)[(s->dac_addr - 1) &
                                (s3_cursor_size(s) == 64 ? 1023 : 255)];
        s->dac_addr = (s->dac_addr + 1) & 1023;
        return value;
    case 12:
        return s->cursor_x;
    case 13:
        return s->cursor_x >> 8;
    case 14:
        return s->cursor_y;
    case 15:
        return s->cursor_y >> 8;
    default:
        return vga_ioport_read(&s->vga, port);
    }
}

static void s3_dac_write(JensenS3State *s, unsigned port, uint8_t value)
{
    unsigned index = s3_dac_index(s, port);
    trace_jensen_s3_dac(index, value);
    switch (index) {
    case 0:
    case 3:
    case 4:
    case 7:
        s->dac_addr = value | ((s->dac_command[3] & 3) << 8);
        s->dac_component = 0;
        /* Read counters point one byte beyond the next returned entry. */
        if (index == 3 || index == 7) {
            s->dac_addr = (s->dac_addr + 1) & 1023;
        }
        if (index < 4) {
            vga_ioport_write(&s->vga, port, value);
        }
        break;
    case 5:
        s->cursor_palette[(s->dac_addr & 3) * 3 + s->dac_component++] = value;
        if (s->dac_component == 3) {
            s->dac_component = 0;
            s->dac_addr = (s->dac_addr + 1) & 255;
        }
        break;
    case 6:
        s->dac_command[0] = value;
        s->vga.dac_8bit = !!(value & 2);
        break;
    case 8:
        s->dac_command[1] = value;
        break;
    case 9:
        s->dac_command[2] = value;
        break;
    case 10:
        if ((s->dac_command[0] & 0x80) && (s->dac_addr & 0x3f) == 1) {
            s->dac_command[3] = value;
            s->dac_addr = (s->dac_addr & 255) | ((value & 3) << 8);
        }
        break;
    case 11:
        s3_cursor_ram(s)[s->dac_addr &
                         (s3_cursor_size(s) == 64 ? 1023 : 255)] = value;
        s->dac_addr = (s->dac_addr + 1) & 1023;
        break;
    case 12:
        s->cursor_x = (s->cursor_x & 0xf00) | value;
        break;
    case 13:
        s->cursor_x = (s->cursor_x & 0xff) | ((value & 15) << 8);
        break;
    case 14:
        s->cursor_y = (s->cursor_y & 0xf00) | value;
        break;
    case 15:
        s->cursor_y = (s->cursor_y & 0xff) | ((value & 15) << 8);
        break;
    default:
        vga_ioport_write(&s->vga, port, value);
        break;
    }
    s->vga.full_update_gfx = true;
}

static uint64_t s3_vga_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenS3State *s = opaque;
    VGACommonState *vga = &s->vga;
    unsigned port = addr + 0x3b0;
    uint8_t value;
    if (port == 0x3c6 || port == 0x3c7 || port == 0x3c8 || port == 0x3c9) {
        value = s3_dac_read(s, port);
    } else if ((port == 0x3b5 || port == 0x3d5) && vga->cr_index >= 0x30) {
        value = vga->cr[vga->cr_index];
        if (vga->cr_index == 0x45) {
            s->hwcursor_color_index = 0;
        }
        if (vga->cr_index == 0x30 &&
            (vga->cr[0x38] & 0xcc) != 0x48 &&
            (vga->cr[0x39] & 0xe0) != 0xa0) {
            value = 0xff;
        }
        trace_jensen_s3_cr(vga->cr_index, value, false);
    } else {
        value = vga_ioport_read(vga, port);
    }
    return value;
}

static void s3_vga_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    JensenS3State *s = opaque;
    VGACommonState *vga = &s->vga;
    unsigned port = addr + 0x3b0;
    unsigned index = vga->cr_index;
    trace_jensen_s3_vga_io(port, value);
    if (port >= 0x3c6 && port <= 0x3c9) {
        s3_dac_write(s, port, value);
    } else if (port == 0x3b4 || port == 0x3d4) {
        vga->cr_index = value & 0x7f;
    } else if ((port == 0x3b5 || port == 0x3d5) && index >= 0x20) {
        trace_jensen_s3_cr(index, value, true);
        if (index == 0x30 || index == 0x37 ||
            (index == 0x36 && vga->cr[0x39] != 0xa5) ||
            (index < 0x40 && index != 0x36 && index != 0x38 && index != 0x39 &&
             (vga->cr[0x38] & 0xcc) != 0x48) ||
            (index >= 0x40 && (vga->cr[0x39] & 0xe0) != 0xa0)) {
            return;
        }
        vga->cr[index] = value;
        if (index == 0x4a || index == 0x4b) {
            unsigned shift = s->hwcursor_color_index * 8;
            uint32_t *color = index == 0x4a ? &s->hwcursor_fg : &s->hwcursor_bg;
            if (shift < 24) {
                *color = (*color & ~(0xffu << shift)) |
                         ((value & 0xff) << shift);
            }
            s->hwcursor_color_index = (s->hwcursor_color_index + 1) & 3;
        }
        if (index == 0x50 && (value & 0x30) == 0x30) {
            s->multi[14] &= ~0x10;
        }
        vga->full_update_gfx = true;
    } else {
        vga_ioport_write(vga, port, value);
    }
}

static const MemoryRegionOps s3_vga_ops = {
    .read = s3_vga_read,
    .write = s3_vga_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

static unsigned s3_bank_offset(JensenS3State *s, unsigned bank)
{
    bool chain4 = s->vga.sr[4] & 8;
    /* Unpopulated banks float high; do not alias them onto installed VRAM. */
    return bank << (chain4 ? 16 : 14);
}

/* Adapt the S3 banked 128 KiB window to the VGA core's banked 64 KiB map. */
static hwaddr s3_vga_address(JensenS3State *s, hwaddr addr, unsigned bank)
{
    VGACommonState *vga = &s->vga;

    vga->bank_offset = s3_bank_offset(s, bank);
    if (!(vga->gr[6] & 0x0c)) {
        /* S3 banks both halves of map 0; QEMU only banks map 1. */
        vga->bank_offset += addr;
        vga->gr[6] |= 4;
        return 0;
    }
    return addr;
}

static bool s3_mmio(JensenS3State *s, hwaddr addr)
{
    return addr < 0x10000 &&
           ((s->vga.cr[0x53] & 0x10) || (s->reg[ADVFUNC] & 0x20));
}

static unsigned s3_mmio_port(unsigned addr)
{
    switch (addr & ~3) {
    case 0x8504:
        return 0x42e8 + (addr & 3);
    case 0x850c:
        return 0x4ae8 + (addr & 3);
    default:
        return addr;
    }
}

static uint64_t s3_memory_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenS3State *s = opaque;
    unsigned bank = (s->vga.cr[0x35] & 15) | ((s->vga.cr[0x51] & 12) << 2);
    uint32_t value = 0;
    unsigned i;

    /*
     * The old 928 MMIO window overlays A0000-AFFFF. Its lower half is
     * PIX_TRANS, independent of the VGA bank.
     */
    if (s3_mmio(s, addr)) {
        if (addr < 0x8000) {
            if (size == 4 && (s->reg[COMMAND] & 0x600) == 0x200) {
                value = s3_accel_read(s, 0xe2e8, 2);
                value |= s3_accel_read(s, 0xe2e8, 2) << 16;
                return value;
            }
            return s3_accel_read(s, 0xe2e8, size);
        }
        if (addr >= 0x83b0 && addr < 0x83e0) {
            for (i = 0; i < size; i++) {
                value |= s3_vga_read(s, ((addr + i) & 0x3ff) - 0x3b0, 1) <<
                         (i * 8);
            }
            return value;
        }
        return s3_accel_read(s, s3_mmio_port(addr), size);
    }
    for (i = 0; i < size; i++) {
        uint32_t byte;
        if (s3_packed(s)) {
            uint32_t offset = bank * 0x10000 + addr + i;
            byte = addr + i < 0x10000 && offset < s->vga.vram_size ?
                   s->vga.vram_ptr[offset] : 0xff;
            trace_jensen_s3_memory(offset, byte, false);
        } else {
            uint8_t map = s->vga.gr[6];
            hwaddr offset = s3_vga_address(s, addr + i, bank);
            byte = vga_mem_readb(&s->vga, offset);
            s->vga.gr[6] = map;
        }
        value |= byte << (i * 8);
    }
    return value;
}

static void s3_memory_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    JensenS3State *s = opaque;
    unsigned bank = (s->vga.cr[0x35] & 15) | ((s->vga.cr[0x51] & 12) << 2);
    unsigned i;

    if (s3_mmio(s, addr)) {
        if (addr < 0x8000) {
            if (size == 4 && (s->reg[COMMAND] & 0x600) == 0x200) {
                s3_accel_write(s, 0xe2e8, value & 0xffff, 2);
                s3_accel_write(s, 0xe2e8, value >> 16, 2);
            } else {
                s3_accel_write(s, 0xe2e8, value, size);
            }
        } else if (addr >= 0x83b0 && addr < 0x83e0) {
            for (i = 0; i < size; i++) {
                s3_vga_write(s, ((addr + i) & 0x3ff) - 0x3b0,
                              value >> (i * 8), 1);
            }
        } else {
            s3_accel_write(s, s3_mmio_port(addr), value, size);
        }
        return;
    }
    for (i = 0; i < size; i++) {
        if (s3_packed(s)) {
            uint32_t offset = bank * 0x10000 + addr + i;
            trace_jensen_s3_memory(offset, (value >> (i * 8)) & 0xff, true);
            if (addr + i < 0x10000 && offset < s->vga.vram_size) {
                s->vga.vram_ptr[offset] = value >> (i * 8);
                memory_region_set_dirty(&s->vga.vram, offset, 1);
            }
        } else {
            uint8_t map = s->vga.gr[6];
            hwaddr offset = s3_vga_address(s, addr + i, bank);
            vga_mem_writeb(&s->vga, offset, value >> (i * 8));
            s->vga.gr[6] = map;
        }
    }
}

static const MemoryRegionOps s3_memory_ops = {
    .read = s3_memory_read,
    .write = s3_memory_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void s3_reset(DeviceState *dev)
{
    JensenS3State *s = JENSEN_S3(dev);
    vga_common_reset(&s->vga);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->multi, 0, sizeof(s->multi));
    s->multi_latch = 0;
    memset(s->dac_command, 0, sizeof(s->dac_command));
    memset(s->cursor_ram, 0, sizeof(s->cursor_ram));
    memset(s->cursor32_ram, 0, sizeof(s->cursor32_ram));
    s->hwcursor_fg = s->hwcursor_bg = s->hwcursor_color_index = 0;
    s->cursor_last_y = s->cursor_last_size = 0;
    memset(s->cursor_palette, 0, sizeof(s->cursor_palette));
    s->dac_addr = s->cursor_x = s->cursor_y = s->dac_component = 0;
    s->progress = 0;
    s->transfer_data = s->transfer_bytes = 0;
    s->pending = s->unsupported = false;
    s->vga.cr[0x30] = 0x91; /* 86C928D */
    s->vga.cr[0x36] = 0x8f; /* ISA, 2 MiB, 8-bit ROM */
    s->vga.cr[0x37] = 0x02;
    s->reg[WRITE_MASK] = s->reg[READ_MASK] = 0xff;
    s->multi[3] = s->multi[4] = 0xfff;
}

static void s3_realize(DeviceState *dev, Error **errp)
{
    JensenS3State *s = JENSEN_S3(dev);
    ISADevice *isa = ISA_DEVICE(dev);
    VGACommonState *vga = &s->vga;
    unsigned i;
    vga->vram_size_mb = 2;
    if (!vga_common_init(vga, OBJECT(dev), errp)) {
        return;
    }
    /* Keep all banked accesses in s3_memory_ops: the generic VGA chain-4
     * alias has higher priority and does not follow S3 CR35/CR51 banks.
     * Leaving legacy_address_space NULL disables only that optimization.
     */
    s->vga_bpp = vga->get_bpp;
    s->vga_params = vga->get_params;
    s->vga_resolution = vga->get_resolution;
    vga->get_bpp = s3_bpp;
    vga->get_params = s3_params;
    vga->get_resolution = s3_resolution;
    vga->cursor_invalidate = s3_cursor_invalidate;
    vga->cursor_draw_line = s3_cursor_draw_line;
    /* Cursor composition must never write into a shared guest VRAM surface. */
    vga->force_shadow = true;
    memory_region_init_io(&s->ports, OBJECT(dev), &s3_vga_ops, s,
                          "jensen-s3-vga", 0x30);
    memory_region_add_subregion(isa_address_space_io(isa), 0x3b0, &s->ports);
    memory_region_init_io(&s->aperture, OBJECT(dev), &s3_memory_ops, s,
                          "jensen-s3-memory", 0x20000);
    memory_region_add_subregion_overlap(isa_address_space(isa), 0xa0000,
                                        &s->aperture, 1);
    /* The last two regions decode multifunction control and pixel transfer. */
    for (i = 0; i < ARRAY_SIZE(s->accel); i++) {
        JensenS3Port *p = &s->accel[i];
        p->state = s;
        p->port = i < ARRAY_SIZE(s3_ports) ? s3_ports[i] :
                  i == ARRAY_SIZE(s3_ports) ? 0xbee8 : 0xe2e8;
        memory_region_init_io(&p->mr, OBJECT(dev), &s3_accel_ops, p,
                              "jensen-s3-engine", 4);
        memory_region_add_subregion(isa_address_space_io(isa), p->port, &p->mr);
    }
    s->hw_ops = *vga->hw_ops;
    s->hw_ops.gfx_update = s3_update_display;
    vga->con = qemu_graphic_console_create(dev, 0, &s->hw_ops, vga);
}

static const VMStateDescription s3_vmstate = {
    .name = TYPE_JENSEN_S3,
    .unmigratable = true,
};

static void s3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = s3_realize;
    dc->vmsd = &s3_vmstate;
    device_class_set_legacy_reset(dc, s3_reset);
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s3_info = {
    .name = TYPE_JENSEN_S3,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(JensenS3State),
    .class_init = s3_class_init,
};

static void s3_register_types(void)
{
    type_register_static(&s3_info);
}

type_init(s3_register_types)
