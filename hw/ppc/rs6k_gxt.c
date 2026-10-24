/*
 * IBM POWER GXT150L graphics adapter for the RS/6000 7006
 *
 * The adapter is bus unit 4 on the 601 bus. Its configuration registers
 * appear at 0xff200000 while BUC 4 is selected and its feature ROM at
 * 0xffa00000. AIX writes the base of its 256 MB window to configuration
 * register 0x18, the BUID to 0x08 and the interrupt priority and level to
 * 0x1000 and 0x1004; the adapter interrupts as source (BUID << 4) | level.
 * In the window, 2 MB of 8-bit video memory 2048 pixels wide sit at
 * 0x04000000, the rasterizer at 0x0a000000 and span fills at 0x0d000000.
 *
 * Rasterizer: reading offset 0 gives the free FIFO entries in the high half
 * and 1 during vertical blank; writing 0-0x3c queues command words. 0xd0
 * reads 0x80000 while the FIFO is empty, with the same vertical blank bit.
 * 0x40 is the mode: 1-bit image source in bit 0, transparency in bit 3, the
 * raster op in 0x4c enabled by bit 4, pattern fill in bit 24. 0x44 and
 * 0x48 hold the foreground and background colours, 0x50 the plane mask. Four
 * clip rectangles sit at 0x5c-0x78 as min, max pairs (x << 16 | y,
 * inclusive), each with two bits in 0x7c: 3 draws only inside, 2 never.
 * 0x100-0x11c hold a 16x16 pattern. Reading 0xb8 returns and clears the
 * pending interrupts: vertical retrace 0x10000, fetch done 0x20000, idle
 * 0x40000. Writing it sets their enables in bits 0-2 and clears the pending
 * ones given in bits 4-6. 0xc0 and 0xc8 hold the low and high byte of a
 * RAMDAC register index, 0xc4 and 0xcc access it with auto increment. The
 * RAMDAC holds the CRTC timing and the cursor below 0x600 and colours from
 * there, one entry per index taking red, green and blue in turn: the
 * palette at 0x600 and the cursor colours at 0xa10.
 *
 * Command words have bit 31 set, the opcode in bits 24-31 and a count in
 * the low half; 0xffff makes the argument list run until 0xd4 is written.
 * 0x80 points, 0x82 polyline and 0x84 segments (vertices x << 16 | y from
 * the origin at 0x80), 0x86 rectangles (x << 16 | y, h << 16 | w), 0x87
 * triangles, 0x8d images (x << 16 | y, h << 16 | w, pixels skipped per row
 * left << 16 | right, then count blocks of rows side by side) and 0xa0
 * copies (source, destination, h << 16 | w).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "hw/ppc/rs6k_gxt.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(GxtState, RS6K_GXT)

#define GXT_BUC_ID      0x34900040
#define GXT_ROM_SIZE    (64 * KiB)
#define GXT_WIN_SIZE    (256 * MiB)
#define GXT_VRAM        0x04000000
#define GXT_VRAM_SIZE   (2 * MiB)
#define GXT_PITCH       2048
#define GXT_LINES       (GXT_VRAM_SIZE / GXT_PITCH)
#define GXT_REGS        0x0a000000
#define GXT_SPAN        0x0d000000
#define GXT_SPAN_SIZE   (8 * MiB)
#define GXT_REGS_SIZE   0x1000
#define GXT_DAC_SIZE    0x1000
#define GXT_CFG_WORDS   8

/* configuration registers */
#define CFG_BUID        0x0008
#define CFG_WINDOW      0x0018
#define CFG_PRIO        0x1000
#define CFG_LEVEL       0x1004

/* rasterizer registers */
#define REG_FIFO        0x00
#define REG_FIFO_END    0x40
#define REG_MODE        0x40
#define REG_FG          0x44
#define REG_BG          0x48
#define REG_ROP         0x4c
#define REG_PLANES      0x50
#define REG_CLIP        0x5c
#define REG_CLIP_ON     0x7c
#define REG_ORIGIN      0x80
#define REG_PATTERN     0x100
#define REG_INT         0xb8
#define REG_DAC_LO      0xc0
#define REG_DAC_DATA    0xc4
#define REG_DAC_HI      0xc8
#define REG_DAC_PAL     0xcc
#define REG_STATUS      0xd0
#define REG_END         0xd4

#define MODE_TRANSPARENT 0x00000008
#define MODE_ROP        0x00000010
#define MODE_1BIT       0x00000001
#define MODE_STIPPLE    0x01000000
#define CLIP_RECTS      4
#define CLIP_INSIDE     0x1
#define CLIP_ENABLE     0x2

#define STAT_FREE       0x02000000
#define STAT_VBLANK     0x1
#define STATUS_EMPTY    0x00080000

#define INT_VBLANK      0x1
#define INT_FETCH       0x2
#define INT_IDLE        0x4
#define INT_ALL         0x7

#define COUNT_OPEN      0xffff
#define CMD_POINTS      0x80
#define CMD_POLYLINE    0x82
#define CMD_SEGMENTS    0x84
#define CMD_FILL        0x86
#define CMD_TRIANGLE    0x87
#define CMD_EXPAND      0x8d
#define CMD_COPY        0xa0

/* RAMDAC registers */
#define DAC_CTRL        0x02
#define DAC_SENSE       0x05
#define SENSE_1280      0xaa
#define DAC_HDISP       0x15
#define DAC_VDISP       0x1f
#define DAC_CURSOR_ON   0x30
#define DAC_HOT_X       0x32
#define DAC_HOT_Y       0x33
#define DAC_CURSOR_X    0x34
#define DAC_CURSOR_Y    0x36
#define DAC_CURSOR_IMG  0x100
#define DAC_PALETTE     0x600
#define DAC_CURSOR_COL  0xa10
#define DAC_COLOURS     (GXT_DAC_SIZE - DAC_PALETTE)
#define CURSOR_DIM      64
#define CTRL_VIDEO      0x01

#define VBLANK_NS       (NANOSECONDS_PER_SECOND / 60)

struct GxtState {
    SysBusDevice parent_obj;

    char *romfile;
    uint8_t rom[GXT_ROM_SIZE];
    uint32_t cfg[2][GXT_CFG_WORDS];
    MemoryRegion window;
    MemoryRegion vram;
    MemoryRegion regs_io;
    MemoryRegion span_io;
    bool mapped;
    uint8_t *vram_ptr;
    QemuConsole *con;
    bool full_update;
    int cursor_y;

    uint32_t regs[GXT_REGS_SIZE / 4];
    uint16_t dac_index;
    uint8_t dac[DAC_PALETTE];
    uint8_t palette[DAC_COLOURS][3];
    unsigned pal_comp;
    bool vblank;
    uint8_t int_enable;
    uint8_t int_pending;
    QEMUTimer *vblank_timer;
    Rs6kGxtPresent present;
    void *present_opaque;

    uint32_t cmd;
    uint32_t last;
    unsigned count;
    unsigned nargs;
    uint32_t arg[4];
    uint32_t bit, bits;
    int x, y;

    DECLARE_BITMAP(seen_op, 256);
    DECLARE_BITMAP(seen_reg, GXT_REGS_SIZE / 4);
    DECLARE_BITMAP(seen_mem, GXT_WIN_SIZE / MiB);
};

static uint32_t *cfg_word(GxtState *s, hwaddr addr)
{
    unsigned bank = addr >= 0x1000;
    unsigned i = (addr & 0xfff) >> 2;

    return i < GXT_CFG_WORDS ? &s->cfg[bank][i] : NULL;
}

static uint32_t cfg_get(GxtState *s, hwaddr addr)
{
    return *cfg_word(s, addr);
}

static void gxt_update_irq(GxtState *s)
{
    uint32_t source = (cfg_get(s, CFG_BUID) >> 16 & 0x1ff) << 4 |
                      (cfg_get(s, CFG_LEVEL) & 0xf);

    if (s->present) {
        s->present(s->present_opaque, source, cfg_get(s, CFG_PRIO) & 0xff,
                   s->int_pending & s->int_enable);
    }
}

void rs6k_gxt_set_presenter(DeviceState *dev, Rs6kGxtPresent fn, void *opaque)
{
    GxtState *s = RS6K_GXT(dev);

    s->present = fn;
    s->present_opaque = opaque;
}

uint32_t rs6k_gxt_cfg_read(DeviceState *dev, hwaddr addr)
{
    GxtState *s = RS6K_GXT(dev);
    uint32_t *w = cfg_word(s, addr);
    uint32_t val = w ? *w : ~0U;

    trace_rs6k_gxt_cfg_read(addr, val);
    return val;
}

void rs6k_gxt_cfg_write(DeviceState *dev, hwaddr addr, uint32_t val)
{
    GxtState *s = RS6K_GXT(dev);
    uint32_t *w = cfg_word(s, addr);

    trace_rs6k_gxt_cfg_write(addr, val);
    if (!w || addr == 0) {
        return;
    }
    *w = val;
    if (addr != CFG_WINDOW) {
        return;
    }
    if (s->mapped) {
        memory_region_del_subregion(get_system_memory(), &s->window);
    }
    memory_region_add_subregion_overlap(get_system_memory(), val & 0xf0000000,
                                        &s->window, 2);
    s->mapped = true;
}

uint8_t rs6k_gxt_rom_read(DeviceState *dev, hwaddr addr)
{
    GxtState *s = RS6K_GXT(dev);

    return addr < GXT_ROM_SIZE ? s->rom[addr] : 0xff;
}

static unsigned gxt_width(GxtState *s)
{
    return (s->dac[DAC_HDISP] | (s->dac[DAC_HDISP + 1] << 8)) + 1;
}

static unsigned gxt_height(GxtState *s)
{
    return (s->dac[DAC_VDISP] | (s->dac[DAC_VDISP + 1] << 8)) + 1;
}

/* The index registers keep all 16 bits, of which the RAMDAC decodes 12 */
static unsigned dac_addr(GxtState *s)
{
    return s->dac_index % GXT_DAC_SIZE;
}

static bool dac_is_palette(GxtState *s)
{
    return dac_addr(s) >= DAC_PALETTE;
}

/* Palette entries take red, green and blue in turn, like a VGA DAC */
static void dac_write(GxtState *s, uint8_t val)
{
    trace_rs6k_gxt_dac_write(s->dac_index, val);
    s->full_update = true;
    if (!dac_is_palette(s)) {
        s->dac[dac_addr(s)] = val;
        s->dac_index++;
        return;
    }
    s->palette[dac_addr(s) - DAC_PALETTE][s->pal_comp] = val;
    if (++s->pal_comp == 3) {
        s->pal_comp = 0;
        s->dac_index++;
    }
}

/*
 * The RAMDAC is a byte-wide device: reads return the byte on every lane.
 * The monitor ID pins read 0xaa whatever the syncs do: a 1280x1024 60 Hz
 * display. Open pins would read 0xff, which the diagnostics reject.
 */
static uint8_t dac_read(GxtState *s)
{
    unsigned i = dac_addr(s);
    uint8_t val;

    if (!dac_is_palette(s)) {
        s->dac_index++;
        return i == DAC_SENSE ? SENSE_1280 : s->dac[i];
    }
    val = s->palette[i - DAC_PALETTE][s->pal_comp];
    if (++s->pal_comp == 3) {
        s->pal_comp = 0;
        s->dac_index++;
    }
    return val;
}

/* Vertical blank shows on every other status read */
static uint32_t gxt_retrace(GxtState *s)
{
    s->vblank = !s->vblank;
    return s->vblank ? STAT_VBLANK : 0;
}

static void gxt_vblank(void *opaque)
{
    GxtState *s = opaque;

    s->int_pending |= INT_VBLANK;
    gxt_update_irq(s);
    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBLANK_NS);
}

/* The rasterizer never stays busy, so an enabled idle interrupt is due */
static void gxt_int_write(GxtState *s, uint32_t val)
{
    s->int_pending &= ~(val >> 4 & INT_ALL);
    s->int_enable = val & INT_ALL;
    if (s->int_enable & INT_IDLE) {
        s->int_pending |= INT_IDLE;
    }
    gxt_update_irq(s);
}

static uint32_t gxt_int_read(GxtState *s)
{
    uint32_t val = (uint32_t)s->int_pending << 16 | s->int_enable;

    s->int_pending = 0;
    gxt_update_irq(s);
    return val;
}

static void gxt_dirty(GxtState *s, int y, int h)
{
    y = MAX(y, 0);
    h = MIN(y + h, GXT_LINES) - y;
    if (h > 0) {
        memory_region_set_dirty(&s->vram, (hwaddr)y * GXT_PITCH,
                                (hwaddr)h * GXT_PITCH);
    }
}

static uint32_t reg(GxtState *s, hwaddr addr)
{
    return s->regs[addr >> 2];
}

static int coord_x(uint32_t v)
{
    return (int16_t)(v >> 16);
}

static int coord_y(uint32_t v)
{
    return (int16_t)v;
}

/*
 * Four clip rectangles (min, max pairs), each with a 2-bit field in
 * REG_CLIP_ON: the pixel must be in one of the enabled inside rectangles,
 * if any, and in none of the enabled outside ones.
 */
static bool gxt_clipped(GxtState *s, int x, int y)
{
    uint32_t on = reg(s, REG_CLIP_ON);
    bool inside_any = false, in = false;

    if (x < 0 || y < 0 || x >= GXT_PITCH || y >= GXT_LINES) {
        return true;
    }
    for (int i = 0; i < CLIP_RECTS; i++, on >>= 2) {
        uint32_t lo = reg(s, REG_CLIP + 8 * i);
        uint32_t hi = reg(s, REG_CLIP + 8 * i + 4);
        bool hit = x >= coord_x(lo) && y >= coord_y(lo) &&
                   x <= coord_x(hi) && y <= coord_y(hi);

        if (!(on & CLIP_ENABLE)) {
            continue;
        }
        if (!(on & CLIP_INSIDE)) {
            if (hit) {
                return true;
            }
            continue;
        }
        inside_any = true;
        in |= hit;
    }
    return inside_any && !in;
}

/* The X11 raster ops (GXclear to GXset) */
static uint8_t gxt_rop(uint8_t alu, uint8_t src, uint8_t dst)
{
    switch (alu & 0xf) {
    case 0x0: return 0;
    case 0x1: return src & dst;
    case 0x2: return src & ~dst;
    case 0x4: return ~src & dst;
    case 0x5: return dst;
    case 0x6: return src ^ dst;
    case 0x7: return src | dst;
    case 0x8: return ~(src | dst);
    case 0x9: return ~src ^ dst;
    case 0xa: return ~dst;
    case 0xb: return src | ~dst;
    case 0xc: return ~src;
    case 0xd: return ~src | dst;
    case 0xe: return ~(src & dst);
    case 0xf: return 0xff;
    }
    return src;
}

static void gxt_pset(GxtState *s, int x, int y, uint8_t c)
{
    uint8_t mask = reg(s, REG_PLANES);
    uint8_t *p;

    if (gxt_clipped(s, x, y)) {
        return;
    }
    p = s->vram_ptr + y * GXT_PITCH + x;
    if (reg(s, REG_MODE) & MODE_ROP) {
        c = gxt_rop(reg(s, REG_ROP), c, *p);
    }
    *p = (*p & ~mask) | (c & mask);
}

/* The 16x16 pattern: row r is the high or low half of word r / 2 */
static bool gxt_pattern(GxtState *s, int x, int y)
{
    uint32_t w = s->regs[(REG_PATTERN >> 2) + (y & 15) / 2];
    uint16_t row = y & 1 ? w : w >> 16;

    return row & (0x8000 >> (x & 15));
}

static void gxt_fill(GxtState *s, uint32_t pos, uint32_t size)
{
    int x = coord_x(pos), y = coord_y(pos);
    int x1 = MIN(x + (int)(size & 0xffff), GXT_PITCH);
    int y1 = MIN(y + (int)(size >> 16), GXT_LINES);
    uint8_t fg = reg(s, REG_FG);
    bool stipple = reg(s, REG_MODE) & MODE_STIPPLE;

    for (int j = MAX(y, 0); j < y1; j++) {
        for (int i = MAX(x, 0); i < x1; i++) {
            if (!stipple || gxt_pattern(s, i, j)) {
                gxt_pset(s, i, j, fg);
            }
        }
    }
    gxt_dirty(s, y, y1 - y);
}

static void gxt_copy(GxtState *s, uint32_t src, uint32_t dst, uint32_t size)
{
    int sx = coord_x(src), sy = coord_y(src);
    int dx = coord_x(dst), dy = coord_y(dst);
    int w = MIN(size & 0xffff, GXT_PITCH), h = MIN(size >> 16, GXT_LINES);
    int ystep = dy > sy ? -1 : 1;
    int xstep = dx > sx ? -1 : 1;

    for (int j = 0, y = ystep > 0 ? 0 : h - 1; j < h; j++, y += ystep) {
        for (int i = 0, x = xstep > 0 ? 0 : w - 1; i < w; i++, x += xstep) {
            if (sx + x < 0 || sy + y < 0 || sx + x >= GXT_PITCH ||
                sy + y >= GXT_LINES) {
                continue;
            }
            gxt_pset(s, dx + x, dy + y,
                     s->vram_ptr[(sy + y) * GXT_PITCH + sx + x]);
        }
    }
    gxt_dirty(s, dy, h);
}

/* Lines far outside the video memory are not walked pixel by pixel */
static void gxt_line(GxtState *s, int x0, int y0, int x1, int y1)
{
    if (MAX(x0, x1) < 0 || MAX(y0, y1) < 0 || MIN(x0, x1) >= GXT_PITCH ||
        MIN(y0, y1) >= GXT_LINES) {
        return;
    }
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    uint8_t fg = reg(s, REG_FG);

    gxt_dirty(s, MIN(y0, y1), abs(y1 - y0) + 1);
    for (;;) {
        int e2 = 2 * err;

        gxt_pset(s, x0, y0, fg);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Line commands take their vertices relative to the origin register */
static void gxt_vertex(GxtState *s, uint32_t v, int *x, int *y)
{
    *x = coord_x(v) + coord_x(reg(s, REG_ORIGIN));
    *y = coord_y(v) + coord_y(reg(s, REG_ORIGIN));
}

static void gxt_triangle(GxtState *s)
{
    int x[3], y[3], ymin, ymax;
    uint8_t fg = reg(s, REG_FG);

    for (int i = 0; i < 3; i++) {
        gxt_vertex(s, s->arg[i], &x[i], &y[i]);
    }
    ymin = MAX(MIN(y[0], MIN(y[1], y[2])), 0);
    ymax = MIN(MAX(y[0], MAX(y[1], y[2])), GXT_LINES - 1);
    for (int j = ymin; j <= ymax; j++) {
        int xl = INT_MAX, xr = INT_MIN;

        for (int e = 0; e < 3; e++) {
            int ax = x[e], ay = y[e], bx = x[(e + 1) % 3], by = y[(e + 1) % 3];
            int xi;

            if ((j < ay && j < by) || (j > ay && j > by)) {
                continue;
            }
            xi = ay == by ? MIN(ax, bx) : ax + (bx - ax) * (j - ay) / (by - ay);
            xl = MIN(xl, ay == by ? MIN(ax, bx) : xi);
            xr = MAX(xr, ay == by ? MAX(ax, bx) : xi);
        }
        for (int i = MAX(xl, 0); i <= MIN(xr, GXT_PITCH - 1); i++) {
            gxt_pset(s, i, j, fg);
        }
    }
    gxt_dirty(s, ymin, ymax - ymin + 1);
}

/*
 * Image: count blocks of h rows of w pixels side by side, each block
 * starting on a word. Pixels are bits drawn in the foreground and
 * background colours with MODE_1BIT, else bytes. The left and right skip
 * in the third argument are pixels of each row of a block not drawn; the
 * drawn ones of the blocks follow each other from x plus the left skip.
 */
static unsigned expand_bpp(GxtState *s)
{
    return reg(s, REG_MODE) & MODE_1BIT ? 1 : 8;
}

static void gxt_image(GxtState *s, uint32_t data)
{
    int x = coord_x(s->arg[0]), y = coord_y(s->arg[0]);
    unsigned w = s->arg[1] & 0xffff, h = s->arg[1] >> 16;
    unsigned lskip = s->arg[2] >> 16, rskip = s->arg[2] & 0xffff;
    unsigned vis = w - MIN(lskip + rskip, w), bpp = expand_bpp(s);
    unsigned block = s->bit / s->bits, bit = s->bit % s->bits;
    bool opaque = !(reg(s, REG_MODE) & MODE_TRANSPARENT);
    uint8_t fg = reg(s, REG_FG), bg = reg(s, REG_BG);

    for (int i = 32 - bpp; i >= 0 && bit < w * h * bpp; i -= bpp, bit += bpp) {
        unsigned pix = bit / bpp, col = pix % w;
        uint8_t v = data >> i & ((1 << bpp) - 1);

        if (col < lskip || col >= w - rskip) {
            continue;
        }
        if (bpp == 1 && !v && !opaque) {
            continue;
        }
        gxt_pset(s, x + block * vis + col, y + pix / w,
                 bpp == 8 ? v : v ? fg : bg);
    }
    s->bit = block * s->bits + (bit < w * h * bpp ? bit : s->bits);
    if (s->bit >= s->bits * s->count) {
        gxt_dirty(s, y, h);
        s->cmd = 0;
    }
}

/* One more argument group done: the command ends after count of them */
static void gxt_next(GxtState *s)
{
    if (s->count != COUNT_OPEN && --s->count == 0) {
        s->cmd = 0;
    }
}

static void gxt_arg(GxtState *s, uint32_t val)
{
    uint8_t op = s->cmd >> 24;
    int x, y;

    s->arg[s->nargs++ % 4] = val;
    switch (op) {
    case CMD_FILL:
        if (s->nargs == 2) {
            gxt_fill(s, s->arg[0], s->arg[1]);
            s->nargs = 0;
            gxt_next(s);
        }
        return;
    case CMD_COPY:
        if (s->nargs == 3) {
            gxt_copy(s, s->arg[0], s->arg[1], s->arg[2]);
            s->nargs = 0;
            gxt_next(s);
        }
        return;
    case CMD_TRIANGLE:
        if (s->nargs == 3) {
            gxt_triangle(s);
            s->nargs = 0;
            gxt_next(s);
        }
        return;
    case CMD_POINTS:
        gxt_vertex(s, val, &x, &y);
        gxt_pset(s, x, y, reg(s, REG_FG));
        gxt_dirty(s, y, 1);
        break;
    case CMD_POLYLINE:
        gxt_vertex(s, val, &x, &y);
        if (s->nargs > 1) {
            gxt_line(s, s->x, s->y, x, y);
        }
        s->x = x;
        s->y = y;
        break;
    case CMD_SEGMENTS:
        gxt_vertex(s, val, &x, &y);
        if (s->nargs % 2 == 0) {
            gxt_line(s, s->x, s->y, x, y);
        }
        s->x = x;
        s->y = y;
        if (s->nargs == 2 * s->count) {
            s->cmd = 0;
        }
        return;
    case CMD_EXPAND:
        if (s->nargs == 3) {
            s->bits = (s->arg[1] & 0xffff) * (s->arg[1] >> 16) * expand_bpp(s);
            s->bits = ROUND_UP(s->bits, 32);
            s->bit = 0;
            s->cmd = s->bits && s->count ? s->cmd : 0;
        }
        return;
    }
    if (s->nargs == s->count) {
        s->cmd = 0;
    }
}

static bool cmd_known(uint8_t op)
{
    switch (op) {
    case CMD_POINTS:
    case CMD_POLYLINE:
    case CMD_SEGMENTS:
    case CMD_FILL:
    case CMD_TRIANGLE:
    case CMD_EXPAND:
    case CMD_COPY:
        return true;
    }
    return false;
}

static void gxt_fifo(GxtState *s, uint32_t val)
{
    trace_rs6k_gxt_fifo(val);
    if (!s->cmd) {
        if (!cmd_known(val >> 24)) {
            if (!test_and_set_bit(val >> 24, s->seen_op)) {
                qemu_log_mask(LOG_UNIMP, "gxt150l: command 0x%08x after "
                              "0x%08x 0x%08x 0x%08x 0x%08x mode 0x%08x\n",
                              val, s->last, s->arg[0], s->arg[1], s->arg[2],
                              reg(s, REG_MODE));
            }
            return;
        }
        s->cmd = val;
        s->last = val;
        s->count = val & 0xffff ?: 1;
        s->nargs = 0;
        return;
    }
    if (s->cmd >> 24 == CMD_EXPAND && s->nargs == 3) {
        gxt_image(s, val);
        return;
    }
    gxt_arg(s, val);
}

static uint64_t gxt_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    GxtState *s = opaque;
    uint64_t val;

    if (addr < REG_FIFO_END) {
        addr = REG_FIFO;
    }
    switch (addr) {
    case REG_FIFO:
        val = STAT_FREE | gxt_retrace(s);
        break;
    case REG_STATUS:
        val = STATUS_EMPTY | gxt_retrace(s);
        break;
    case REG_INT:
        val = gxt_int_read(s);
        break;
    case REG_DAC_LO:
        val = (s->dac_index & 0xff) * 0x01010101U;
        break;
    case REG_DAC_HI:
        val = (s->dac_index >> 8) * 0x01010101U;
        break;
    case REG_DAC_DATA:
    case REG_DAC_PAL:
        val = dac_read(s) * 0x01010101U;
        break;
    default:
        val = s->regs[addr >> 2];
    }
    trace_rs6k_gxt_read(GXT_REGS + addr, size, val);
    return val;
}

static void gxt_reg_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    GxtState *s = opaque;

    trace_rs6k_gxt_write(GXT_REGS + addr, size, val);
    if (addr < REG_FIFO_END) {
        gxt_fifo(s, val);
        return;
    }
    switch (addr) {
    case REG_INT:
        gxt_int_write(s, val);
        return;
    case REG_END:
        s->cmd = 0;
        return;
    case REG_DAC_LO:
        s->dac_index = (s->dac_index & 0xff00) | val >> 24;
        s->pal_comp = 0;
        return;
    case REG_DAC_HI:
        s->dac_index = (s->dac_index & 0xff) | (val >> 24) << 8;
        s->pal_comp = 0;
        return;
    case REG_DAC_DATA:
    case REG_DAC_PAL:
        dac_write(s, val >> 24);
        return;
    }
    if (!test_and_set_bit(addr >> 2, s->seen_reg)) {
        qemu_log_mask(LOG_UNIMP, "gxt150l: register 0x%03" HWADDR_PRIx
                      " = 0x%08" PRIx64 "\n", addr, val);
    }
    s->regs[addr >> 2] = val;
}

/* The diagnostics reach the RAMDAC registers a byte at a time too */
static const MemoryRegionOps gxt_reg_ops = {
    .read = gxt_reg_read,
    .write = gxt_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Span fills: a word written at (y << 13) | (x << 2) fills the width in its
 * low half from (x, y) as the 0x86 command would.
 */
static uint64_t gxt_span_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void gxt_span_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    GxtState *s = opaque;
    uint32_t pos = (addr >> 2 & 0x7ff) << 16 | addr >> 13;

    trace_rs6k_gxt_mem_write(GXT_SPAN + addr, size, val);
    gxt_fill(s, pos, 1 << 16 | (val & 0xffff));
}

static const MemoryRegionOps gxt_span_ops = {
    .read = gxt_span_read,
    .write = gxt_span_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* The rest of the window, logged once per megabyte */
static uint64_t gxt_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    GxtState *s = opaque;

    trace_rs6k_gxt_mem_read(addr, size);
    if (!test_and_set_bit(addr / MiB, s->seen_mem)) {
        qemu_log_mask(LOG_UNIMP, "gxt150l: read%u 0x%07" HWADDR_PRIx "\n",
                      size * 8, addr);
    }
    return 0;
}

static void gxt_unknown_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    GxtState *s = opaque;

    trace_rs6k_gxt_mem_write(addr, size, val);
    if (!test_and_set_bit(addr / MiB, s->seen_mem)) {
        qemu_log_mask(LOG_UNIMP, "gxt150l: write%u 0x%07" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", size * 8, addr, val);
    }
}

static const MemoryRegionOps gxt_unknown_ops = {
    .read = gxt_unknown_read,
    .write = gxt_unknown_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

static int dac_word(GxtState *s, unsigned i)
{
    return s->dac[i] | s->dac[i + 1] << 8;
}

/*
 * The hardware cursor: 64x64 pixels of 2 bits, leftmost in the high bits,
 * at its position less the hot spot. Values 1-3 take the colours at 0xa10,
 * 0xa11 and 0xa12; 0 is transparent.
 */
static int cursor_top(GxtState *s)
{
    if (!(s->dac[DAC_CURSOR_ON] & 1)) {
        return -CURSOR_DIM;
    }
    return dac_word(s, DAC_CURSOR_Y) - s->dac[DAC_HOT_Y];
}

static void gxt_draw_cursor(GxtState *s, DisplaySurface *surface)
{
    int x0 = dac_word(s, DAC_CURSOR_X) - s->dac[DAC_HOT_X];
    int y0 = cursor_top(s);
    int w = surface_width(surface), h = surface_height(surface);
    const uint8_t (*colour)[3] = &s->palette[DAC_CURSOR_COL - DAC_PALETTE - 1];

    for (int cy = 0; cy < CURSOR_DIM; cy++) {
        uint32_t *dst = (uint32_t *)surface_data(surface) + (y0 + cy) * w;

        if (y0 + cy < 0 || y0 + cy >= h) {
            continue;
        }
        for (int cx = 0; cx < CURSOR_DIM; cx++) {
            uint8_t b = s->dac[DAC_CURSOR_IMG + cy * 16 + cx / 4];
            unsigned v = (b >> (6 - 2 * (cx % 4))) & 3;
            const uint8_t *rgb = colour[v];

            if (v && x0 + cx >= 0 && x0 + cx < w) {
                dst[x0 + cx] = rgb[0] << 16 | rgb[1] << 8 | rgb[2];
            }
        }
    }
}

/* Until a mode is set and video turned on the monitor shows black */
static void gxt_blank(GxtState *s, DisplaySurface *surface)
{
    if (!s->full_update) {
        return;
    }
    memset(surface_data(surface), 0,
           surface_stride(surface) * surface_height(surface));
    qemu_console_update(s->con, 0, 0, surface_width(surface),
                        surface_height(surface));
    s->full_update = false;
}

static bool gxt_update_display(void *opaque)
{
    GxtState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    unsigned w = gxt_width(s), h = gxt_height(s);
    int cursor_y = cursor_top(s);
    DirtyBitmapSnapshot *snap;
    DECLARE_BITMAP(rows, GXT_LINES) = { 0 };

    if (!(s->dac[DAC_CTRL] & CTRL_VIDEO) || w < 64 || h < 64 ||
        w > GXT_PITCH || h > GXT_LINES) {
        gxt_blank(s, surface);
        return true;
    }
    if (surface_width(surface) != w || surface_height(surface) != h) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        s->full_update = true;
    }
    if (surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    /* redraw the rows under the cursor, and where it was */
    gxt_dirty(s, s->cursor_y, CURSOR_DIM);
    gxt_dirty(s, cursor_y, CURSOR_DIM);
    s->cursor_y = cursor_y;
    snap = memory_region_snapshot_and_clear_dirty(&s->vram, 0, GXT_VRAM_SIZE,
                                                  DIRTY_MEMORY_VGA);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *src = s->vram_ptr + y * GXT_PITCH;
        uint32_t *dst = (uint32_t *)surface_data(surface) + y * w;

        if (!s->full_update &&
            !memory_region_snapshot_get_dirty(&s->vram, snap, y * GXT_PITCH,
                                              w)) {
            continue;
        }
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *rgb = s->palette[src[x]];

            dst[x] = rgb[0] << 16 | rgb[1] << 8 | rgb[2];
        }
        set_bit(y, rows);
    }
    g_free(snap);
    s->full_update = false;
    gxt_draw_cursor(s, surface);
    for (unsigned y = find_first_bit(rows, h); y < h;) {
        unsigned end = find_next_zero_bit(rows, h, y);

        qemu_console_update(s->con, 0, y, w, end - y);
        y = find_next_bit(rows, h, end);
    }
    return true;
}

static void gxt_invalidate_display(void *opaque)
{
    GxtState *s = opaque;

    s->full_update = true;
}

static const GraphicHwOps gxt_gfx_ops = {
    .gfx_update = gxt_update_display,
    .invalidate = gxt_invalidate_display,
};

static void gxt_reset(DeviceState *dev)
{
    GxtState *s = RS6K_GXT(dev);

    memset(s->cfg, 0, sizeof(s->cfg));
    s->cfg[0][0] = GXT_BUC_ID;
    if (s->mapped) {
        memory_region_del_subregion(get_system_memory(), &s->window);
        s->mapped = false;
    }
    s->int_enable = 0;
    s->int_pending = 0;
    s->cmd = 0;
    s->dac[DAC_CTRL] = 0;
    s->full_update = true;
    gxt_update_irq(s);
}

static void gxt_realize(DeviceState *dev, Error **errp)
{
    GxtState *s = RS6K_GXT(dev);
    g_autofree char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, s->romfile);

    if (!path || load_image_size(path, s->rom, GXT_ROM_SIZE) < 0) {
        error_setg(errp, "could not load GXT150L ROM '%s'", s->romfile);
        return;
    }
    memory_region_init_io(&s->window, OBJECT(dev), &gxt_unknown_ops, s,
                          "gxt150l.window", GXT_WIN_SIZE);
    if (!memory_region_init_ram(&s->vram, OBJECT(dev), "gxt150l.vram",
                                GXT_VRAM_SIZE, errp)) {
        return;
    }
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
    memory_region_add_subregion_overlap(&s->window, GXT_VRAM, &s->vram, 1);
    memory_region_init_io(&s->regs_io, OBJECT(dev), &gxt_reg_ops, s,
                          "gxt150l.regs", GXT_REGS_SIZE);
    memory_region_add_subregion_overlap(&s->window, GXT_REGS, &s->regs_io, 1);
    memory_region_init_io(&s->span_io, OBJECT(dev), &gxt_span_ops, s,
                          "gxt150l.span", GXT_SPAN_SIZE);
    memory_region_add_subregion_overlap(&s->window, GXT_SPAN, &s->span_io, 1);
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gxt_vblank, s);
    timer_mod(s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBLANK_NS);
    s->con = qemu_graphic_console_create(dev, 0, &gxt_gfx_ops, s);
    qemu_console_resize(s->con, 1280, 1024);
}

static const Property gxt_properties[] = {
    DEFINE_PROP_STRING("romfile", GxtState, romfile),
};

static void gxt_instance_init(Object *obj)
{
    GxtState *s = RS6K_GXT(obj);

    s->romfile = g_strdup("GXT150L_88G2832_U35.BIN");
}

static void gxt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "IBM POWER GXT150L graphics adapter";
    dc->realize = gxt_realize;
    device_class_set_legacy_reset(dc, gxt_reset);
    device_class_set_props(dc, gxt_properties);
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo gxt_info = {
    .name = TYPE_RS6K_GXT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GxtState),
    .instance_init = gxt_instance_init,
    .class_init = gxt_class_init,
};

static void gxt_register_types(void)
{
    type_register_static(&gxt_info);
}

type_init(gxt_register_types)
