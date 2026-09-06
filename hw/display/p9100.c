/*
 * Weitek Power 9100 graphics controller
 *
 * Copyright (c) 2026 Ørjan Malde
 *
 * Modelled on the "Power 9100 Graphics Controller" preliminary data book
 * (November 1993) and the "Power 9100 Programmer's Manual" (July 1994).
 *
 * This models the native-mode side of the chip: the linear frame buffer, the
 * CRTC, the VRAM controller, the RAMDAC interface and the drawing engine. It
 * is enough for firmware and drivers that drive the chip natively - the NT
 * ARC bootloader, OS/2 PowerPC and Solaris PPC all do.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "qom/object.h"
#include "trace.h"

#define P9100_BAR_SIZE          (16 * MiB)
#define P9100_FB_BASE           (8 * MiB)
#define P9100_FB_WINDOW         (8 * MiB)
#define P9100_REG_WINDOW        0x80000
#define P9100_REG_MASK          0x7fff

#define P9100_ACC_SWAP_HALF     (1u << 18)
#define P9100_ACC_SWAP_BYTE     (1u << 17)
#define P9100_ACC_SWAP_BIT      (1u << 16)
#define P9100_SWAP_SHIFT        16
#define P9100_SWAP_MASK         0x7
#define P9100_SWAP_BITS         0x1
#define P9100_SWAP_BYTE         0x2
#define P9100_SWAP_HALF         0x4

#define P9100_SYSCONFIG         0x004
#define P9100_INTERRUPT         0x008
#define P9100_INTERRUPT_EN      0x00c
#define P9100_ALT_WRITE_BANK    0x010
#define P9100_ALT_READ_BANK     0x014

#define P9100_HRZC              0x104   /* read only */
#define P9100_HRZT              0x108
#define P9100_HRZSR             0x10c
#define P9100_HRZBR             0x110
#define P9100_HRZBF             0x114
#define P9100_PREHRZC           0x118
#define P9100_VRTC              0x11c   /* read only */
#define P9100_VRTT              0x120
#define P9100_VRTSR             0x124
#define P9100_VRTBR             0x128
#define P9100_VRTBF             0x12c
#define P9100_PREVRTC           0x130
#define P9100_SRADDR            0x134
#define P9100_SRTCTL            0x138
#define P9100_SRADDR_INC        0x13c
#define P9100_SRTCTL2           0x140
#define P9100_VIDEO_FIRST       0x100
#define P9100_VIDEO_LAST        0x17f

#define P9100_MEM_CONFIG        0x184
#define P9100_RFPERIOD          0x188
#define P9100_RFCOUNT           0x18c
#define P9100_RFMAX             0x190
#define P9100_RLCUR             0x194
#define P9100_PU_CONFIG         0x198   /* read only */
#define P9100_VRAM_FIRST        0x180
#define P9100_VRAM_LAST         0x1ff

#define P9100_DAC_FIRST         0x200
#define P9100_DAC_LAST          0x23f
#define P9100_DAC_RS(a)         (((a) >> 2) & 0xf)
#define P9100_DAC_LANE(a)       (((((a) & ~3u) | 2u) - (a)) & 3)

#define P9100_STATUS            0x2000
#define P9100_BLIT_CMD          0x2004
#define P9100_QUAD_CMD          0x2008
#define P9100_PIXEL8_CMD        0x200c

#define P9100_PIXEL1_FIRST      0x2080
#define P9100_PIXEL1_LAST       0x20fc
#define P9100_PIXEL1_NPIX(a)    (((((a) - P9100_PIXEL1_FIRST) >> 2) & 0x1f) + 1)
#define P9100_PE_FIRST          0x2000
#define P9100_PE_LAST           0x21ff

#define P9100_DE_FIRST          0x2200
#define P9100_DE_LAST           0x23ff
#define DE_COLOR0               0
#define DE_COLOR1               1
#define DE_PMASK                2
#define DE_DRAW_MODE            3
#define DE_PAT_ORIGIN_X         4
#define DE_PAT_ORIGIN_Y         5
#define DE_RASTER               6
#define DE_PIXEL8               7
#define DE_P_W_MIN              8
#define DE_P_W_MAX              9
#define DE_COLOR2               14
#define DE_COLOR3               15
#define DE_PATTERN0             32
#define DE_B_W_MIN              40
#define DE_B_W_MAX              41

#define RASTER_MINTERMS(v)      ((v) & 0xff)
#define RASTER_SOLID_DISABLE    (1 << 13)
#define RASTER_PATTERN_DEPTH    (1 << 14)
#define RASTER_PIXEL1_TRANS     (1 << 15)
#define RASTER_DRAW_MODE        (1 << 16)
/* TODO: bit 17, transparent pattern enable? */

#define P9100_COORD_FIRST       0x3000
#define P9100_COORD_LAST        0x31ff
#define P9100_LOADCOORD_FIRST   0x3200
#define P9100_LOADCOORD_LAST    0x33ff
#define COORD_REG(a)            (((a) >> 6) & 0x3)
#define COORD_VTYPE(a)          (((a) >> 6) & 0x7)
#define COORD_SEL(a)            (((a) >> 3) & 0x3)
#define COORD_SEL_X             0x1
#define COORD_SEL_Y             0x2
#define COORD_SEL_XY            0x3

#define P9100_COORD_WIN_REL     0x20
#define PE_W_OFF_XY             ((0x2190 - P9100_PE_FIRST) / 4)

#define VTYPE_POINT             0
#define VTYPE_LINE              1
#define VTYPE_TRIANGLE          2
#define VTYPE_QUAD              3
#define VTYPE_RECT              4

#define SYSCONFIG_SHIFT3(v)     (((v) >> 29) & 0x3)
#define SYSCONFIG_PIXEL_SIZE(v) (((v) >> 26) & 0x7)
#define SYSCONFIG_SHIFT0(v)     (((v) >> 20) & 0x7)
#define SYSCONFIG_SHIFT1(v)     (((v) >> 17) & 0x7)
#define SYSCONFIG_SHIFT2(v)     (((v) >> 14) & 0x7)
#define SYSCONFIG_SWAP(v)       (((v) >> 11) & 0x7)
#define SYSCONFIG_ID_MASK       0x7

#define P9100_INT_DE_IDLE       (1 << 0)
#define P9100_INT_PICKED        (1 << 2)
#define P9100_INT_VBLANKED      (1 << 4)
#define P9100_INT_MASTER        (1 << 6)

#define PU_CONFIG_BUS_PCI       (0x1U << 30)
#define PU_CONFIG_MODESELECT    (1U << 26)     /* 1 = VGA, 0 = native */
#define PU_CONFIG_VGA_ABSENT    (1U << 25)
#define PU_CONFIG_DAC_RGB525    (0x8U << 12)
#define PU_CONFIG_CLOCK_50MHZ   (0x1U << 5)
#define PU_CONFIG_MEM_DEPTH     (1U << 4)
#define PU_CONFIG_SAM_HALF      (1U << 3)
#define PU_CONFIG_ADD_IN_BOARD  (1U << 0)

#define PU_CONFIG_DEFAULT       (PU_CONFIG_BUS_PCI | PU_CONFIG_DAC_RGB525 | \
                                 PU_CONFIG_CLOCK_50MHZ | PU_CONFIG_ADD_IN_BOARD)

#define P9100_CFG64             0x40
#define P9100_CFG65             0x41
#define P9100_CFG66             0x42

#define CFG65_MODESELECT        (1 << 1)
#define CFG65_NA_ENABLE         (1 << 2)
#define CFG65_NA_SELECT         (1 << 3)

#define RGB525_PAL_WR_ADDR      0x0
#define RGB525_PAL_DATA         0x1
#define RGB525_PIXEL_MASK       0x2
#define RGB525_PAL_RD_ADDR      0x3
#define RGB525_INDEX_LOW        0x4
#define RGB525_INDEX_HIGH       0x5
#define RGB525_INDEX_DATA       0x6
#define RGB525_INDEX_CONTROL    0x7

#define RGB525_IDX_REVISION     0x00
#define RGB525_IDX_ID           0x01

/* Hardware cursor */
#define RGB525_IDX_CURSOR_CTL   0x30
#define RGB525_IDX_CURSOR_XL    0x31
#define RGB525_IDX_CURSOR_XH    0x32
#define RGB525_IDX_CURSOR_YL    0x33
#define RGB525_IDX_CURSOR_YH    0x34
#define RGB525_IDX_CURSOR_HOTX  0x35
#define RGB525_IDX_CURSOR_HOTY  0x36
#define RGB525_IDX_CURSOR_COL   0x40
#define RGB525_CURSOR_ARRAY     0x100
#define RGB525_CURSOR_MAX       64
#define RGB525_CURSOR_BYTES     (RGB525_CURSOR_MAX * RGB525_CURSOR_MAX / 4)
#define RGB525_INDEX_LAST       (RGB525_CURSOR_ARRAY + RGB525_CURSOR_BYTES)

#define RGB525_CURSOR_DIM(ctl)  (((ctl) & 0x4) ? RGB525_CURSOR_MAX : 32)
#define RGB525_CURSOR_PART(ctl) (((ctl) >> 6) & 0x3)

#define RGB525_CURSOR_MODE(ctl) ((ctl) & 0x3)
#define RGB525_CURSOR_X11       0x3
#define RGB525_CURSOR_LTOR      0x20

#define TYPE_P9100 "p9100"
OBJECT_DECLARE_SIMPLE_TYPE(P9100State, P9100)

struct P9100State {
    PCIDevice parent_obj;

    QemuConsole *con;

    MemoryRegion bar;
    MemoryRegion fb[P9100_FB_WINDOW / (1 * MiB)];
    MemoryRegion regs;
    MemoryRegion vram;

    uint32_t vram_size;
    uint32_t pu_config;
    uint8_t silicon_rev;

    /* system control group */
    uint32_t sysconfig;
    uint32_t interrupt;
    uint32_t interrupt_en;
    uint32_t alt_read_bank;
    uint32_t alt_write_bank;

    /* video control group, indexed by (offset - 0x100) / 4 */
    uint32_t video[32];
    /* VRAM control group, indexed by (offset - 0x180) / 4 */
    uint32_t vram_ctl[32];
    /* parameter engine, indexed by (offset - 0x2000) / 4 */
    uint32_t pe[128];
    /* drawing engine, indexed by (offset - 0x2200) / 4 */
    uint32_t de[128];
    /* device coordinate registers */
    int32_t x[4], y[4];
    unsigned load_idx;

    /* IBM RGB525 RAMDAC */
    uint8_t dac_index_lo;
    uint8_t dac_index_hi;
    uint8_t dac_indexed[256];
    uint8_t dac_cursor[RGB525_CURSOR_BYTES];
    bool dac_index_inc;
    bool cursor_dirty;
    bool cursor_drawn;
    int cursor_drawn_x, cursor_drawn_y, cursor_drawn_dim;
    uint8_t dac_pal_addr;
    uint8_t dac_pal_hold[2];
    int32_t last_vx, last_vy;
    uint8_t dac_pal_seq;
    uint8_t dac_pixel_mask;
    uint8_t r[256], g[256], b[256];
    uint32_t width, height, depth, pitch;
    bool full_update;
};

static void p9100_update_irq(P9100State *s)
{
    uint32_t mask = P9100_INT_DE_IDLE | P9100_INT_PICKED | P9100_INT_VBLANKED;
    uint32_t active = s->interrupt & s->interrupt_en & mask;

    pci_set_irq(PCI_DEVICE(s), (s->interrupt_en & P9100_INT_MASTER) && active);
}

static const uint32_t shift0_bytes[8] = { 0, 0, 0, 128, 256, 512, 1024, 2048 };
static const uint32_t shift1_bytes[8] = { 0, 0, 64, 128, 256, 512, 1024, 0 };
static const uint32_t shift2_bytes[8] = { 0, 32, 64, 128, 256, 512, 0, 0 };
static const uint32_t shift3_bytes[4] = { 0, 1024, 2048, 4096 };

static uint32_t p9100_pitch(P9100State *s)
{
    uint32_t v = s->sysconfig;

    return shift0_bytes[SYSCONFIG_SHIFT0(v)]
         + shift1_bytes[SYSCONFIG_SHIFT1(v)]
         + shift2_bytes[SYSCONFIG_SHIFT2(v)]
         + shift3_bytes[SYSCONFIG_SHIFT3(v)];
}

static uint32_t p9100_depth(P9100State *s)
{
    switch (SYSCONFIG_PIXEL_SIZE(s->sysconfig)) {
    case 0x2:
        return 8;
    case 0x3:
        return 16;
    case 0x7:
        return 24;
    case 0x5:
        return 32;
    default:
        return 0;
    }
}

static uint32_t p9100_height(P9100State *s)
{
    uint32_t vrtt = s->video[(P9100_VRTT - P9100_VIDEO_FIRST) / 4] & 0xfff;
    uint32_t vrtbr = s->video[(P9100_VRTBR - P9100_VIDEO_FIRST) / 4] & 0xfff;
    uint32_t vrtbf = s->video[(P9100_VRTBF - P9100_VIDEO_FIRST) / 4] & 0xfff;

    if (vrtbf > vrtbr) {
        return vrtbf - vrtbr;
    }
    if (vrtt > vrtbr) {
        return vrtt - vrtbr + vrtbf;
    }
    return 0;
}

static void p9100_update_mode(P9100State *s)
{
    uint32_t depth = p9100_depth(s);
    uint32_t pitch = p9100_pitch(s);
    uint32_t height = p9100_height(s);
    uint32_t width;

    if (!depth || !pitch || !height) {
        return;
    }
    width = pitch / (depth / 8);

    if (width < 64 || width > 2560 || height < 32 || height > 2048) {
        qemu_log_mask(LOG_GUEST_ERROR, "p9100: implausible mode %ux%u-%u "
                      "(pitch %u)\n", width, height, depth, pitch);
        return;
    }
    if ((uint64_t)pitch * height > s->vram_size) {
        qemu_log_mask(LOG_GUEST_ERROR, "p9100: mode %ux%u-%u needs %" PRIu64
                      " bytes, only %u available\n", width, height, depth,
                      (uint64_t)pitch * height, s->vram_size);
        return;
    }

    if (width == s->width && height == s->height && depth == s->depth &&
        pitch == s->pitch) {
        return;
    }

    trace_p9100_mode(width, height, depth, pitch);
    s->width = width;
    s->height = height;
    s->depth = depth;
    s->pitch = pitch;
    qemu_console_resize(s->con, width, height);
    s->full_update = true;
}

static inline uint32_t p9100_rgb(P9100State *s, uint8_t idx)
{
    return (s->r[idx] << 16) | (s->g[idx] << 8) | s->b[idx];
}

static void p9100_draw_line(P9100State *s, uint32_t *dst, const uint8_t *src,
                            uint32_t width)
{
    uint32_t x;

    switch (s->depth) {
    case 8:
        for (x = 0; x < width; x++) {
            *dst++ = p9100_rgb(s, src[x] & s->dac_pixel_mask);
        }
        break;
    case 16:
        /*
         * 5:5:5, least significant byte first.  Read out of video memory with
         * NT running at 65536 colors the boot splash comes out exactly right
         * this way and as rainbow noise under either 5:6:5 or the other byte
         * order.
         */
        /* TODO: decode the RGB525 16bpp format register and support 5:6:5 */
        for (x = 0; x < width; x++) {
            uint32_t v = src[2 * x] | (src[2 * x + 1] << 8);
            uint32_t r = ((v >> 10) & 0x1f) << 3;
            uint32_t g = ((v >> 5) & 0x1f) << 3;
            uint32_t b = (v & 0x1f) << 3;
            *dst++ = (r << 16) | (g << 8) | b;
        }
        break;
    case 24:
        /* Blue, green then red, following the 16bpp byte order. */
        for (x = 0; x < width; x++) {
            *dst++ = (src[3 * x + 2] << 16) | (src[3 * x + 1] << 8)
                   | src[3 * x];
        }
        break;
    case 32:
        /* As 24bpp with a pad byte on top. */
        for (x = 0; x < width; x++) {
            *dst++ = (src[4 * x + 2] << 16) | (src[4 * x + 1] << 8)
                   | src[4 * x];
        }
        break;
    default:
        g_assert_not_reached();
    }
}

static void p9100_cursor_overlay(P9100State *s, DisplaySurface *surface)
{
    const uint8_t *idx = s->dac_indexed;
    const uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t *data = (uint32_t *)surface_data(surface);
    uint8_t ctl = idx[RGB525_IDX_CURSOR_CTL];
    int dim = RGB525_CURSOR_DIM(ctl);
    bool x11 = RGB525_CURSOR_MODE(ctl) == RGB525_CURSOR_X11;
    bool ltor = (ctl & RGB525_CURSOR_LTOR) != 0;
    unsigned part = dim == 32 ? RGB525_CURSOR_PART(ctl) * (32 * 32 / 4) : 0;
    int x, y, cx, cy;
    int y0 = INT_MAX, y1 = INT_MIN;

    if (s->cursor_drawn) {
        int old0 = MAX(s->cursor_drawn_y, 0);
        int old1 = MIN(s->cursor_drawn_y + s->cursor_drawn_dim, (int)s->height);

        for (cy = old0; cy < old1; cy++) {
            p9100_draw_line(s, data + (size_t)cy * s->width,
                            vram + (size_t)cy * s->pitch, s->width);
        }
        if (old1 > old0) {
            y0 = old0;
            y1 = old1;
        }
        s->cursor_drawn = false;
    }

    if (ctl & 0x3) {
        x = ((idx[RGB525_IDX_CURSOR_XH] << 8) | idx[RGB525_IDX_CURSOR_XL]) -
            idx[RGB525_IDX_CURSOR_HOTX];
        y = ((idx[RGB525_IDX_CURSOR_YH] << 8) | idx[RGB525_IDX_CURSOR_YL]) -
            idx[RGB525_IDX_CURSOR_HOTY];

        for (cy = 0; cy < dim; cy++) {
            if (y + cy < 0 || y + cy >= (int)s->height) {
                continue;
            }
            for (cx = 0; cx < dim; cx++) {
                unsigned byte = part + cy * (dim / 4) + cx / 4;
                unsigned pair = ltor ? 3 - (cx % 4) : cx % 4;
                unsigned sel = (s->dac_cursor[byte] >> (2 * pair)) & 3;
                const uint8_t *rgb;
                unsigned slot;

                if (x + cx < 0 || x + cx >= (int)s->width) {
                    continue;
                }
                if (x11) {
                    /*
                     * Plane 1 is the mask and plane 0 chooses the colour, so
                     * 00 and 01 are transparent, 10 is color 1 and 11 is
                     * color 2.  Solaris draws the pointer's border in 10 and
                     * its body in 11, against colour 1 white and colour 2
                     * black - the familiar white edged black arrow.
                     */
                    if (!(sel & 2)) {
                        continue;
                    }
                    slot = sel & 1;
                } else {
                    if (sel == 2) {
                        continue;
                    }
                    slot = sel == 3 ? 2 : sel;  /* 00, 01, 11 -> colour 1..3 */
                }
                rgb = idx + RGB525_IDX_CURSOR_COL + slot * 3;
                data[(size_t)(y + cy) * s->width + x + cx] =
                    (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
            }
        }

        y0 = MIN(y0, MAX(y, 0));
        y1 = MAX(y1, MIN(y + dim, (int)s->height));
        s->cursor_drawn = true;
        s->cursor_drawn_x = x;
        s->cursor_drawn_y = y;
        s->cursor_drawn_dim = dim;
    }

    if (y1 > y0) {
        qemu_console_update(s->con, 0, y0, s->width, y1 - y0);
    }
}

static bool p9100_update_display(void *opaque)
{
    P9100State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap = NULL;
    const uint8_t *vram;
    uint32_t *data;
    int y, y_start = -1;

    /* Latch a vertical blank for anyone polling or waiting on an interrupt. */
    s->interrupt |= P9100_INT_VBLANKED;
    p9100_update_irq(s);

    if (!s->width || surface_bits_per_pixel(surface) != 32) {
        return true;
    }
    vram = memory_region_get_ram_ptr(&s->vram);
    data = (uint32_t *)surface_data(surface);

    if (!s->full_update) {
        snap = memory_region_snapshot_and_clear_dirty(&s->vram, 0, s->vram_size,
                                                      DIRTY_MEMORY_VGA);
    }

    for (y = 0; y < s->height; y++) {
        ram_addr_t page = (ram_addr_t)y * s->pitch;
        bool update = s->full_update;

        if (!update) {
            update = memory_region_snapshot_get_dirty(&s->vram, snap, page,
                                                      s->pitch);
        }
        if (update) {
            if (y_start < 0) {
                y_start = y;
            }
            p9100_draw_line(s, data + (size_t)y * s->width, vram + page,
                            s->width);
        } else if (y_start >= 0) {
            qemu_console_update(s->con, 0, y_start, s->width, y - y_start);
            y_start = -1;
        }
    }
    if (y_start >= 0) {
        qemu_console_update(s->con, 0, y_start, s->width, y - y_start);
    }

    s->full_update = false;
    g_free(snap);

    p9100_cursor_overlay(s, surface);
    return true;
}

static void p9100_invalidate_display(void *opaque)
{
    P9100State *s = opaque;

    s->full_update = true;
    memory_region_set_dirty(&s->vram, 0, s->vram_size);
}

static const GraphicHwOps p9100_gfx_ops = {
    .gfx_update = p9100_update_display,
    .invalidate = p9100_invalidate_display,
};

static unsigned p9100_dac_index(P9100State *s)
{
    return (s->dac_index_hi << 8) | s->dac_index_lo;
}

static void p9100_dac_index_step(P9100State *s)
{
    if (!s->dac_index_inc) {
        return;
    }
    if (++s->dac_index_lo == 0) {
        s->dac_index_hi++;
    }
}

static uint8_t p9100_dac_read(P9100State *s, unsigned rs)
{
    uint8_t val = 0;

    switch (rs) {
    case RGB525_PAL_DATA:
        switch (s->dac_pal_seq) {
        case 0:
            val = s->r[s->dac_pal_addr];
            break;
        case 1:
            val = s->g[s->dac_pal_addr];
            break;
        default:
            val = s->b[s->dac_pal_addr];
            break;
        }
        if (++s->dac_pal_seq == 3) {
            s->dac_pal_seq = 0;
            s->dac_pal_addr++;
        }
        break;
    case RGB525_PIXEL_MASK:
        val = s->dac_pixel_mask;
        break;
    case RGB525_PAL_WR_ADDR:
    case RGB525_PAL_RD_ADDR:
        val = s->dac_pal_addr;
        break;
    case RGB525_INDEX_LOW:
        val = s->dac_index_lo;
        break;
    case RGB525_INDEX_HIGH:
        val = s->dac_index_hi;
        break;
    case RGB525_INDEX_DATA: {
        unsigned idx = p9100_dac_index(s);

        if (idx == RGB525_IDX_ID) {
            val = 0x02;
        } else if (idx == RGB525_IDX_REVISION) {
            val = 0x10;
        } else if (idx < ARRAY_SIZE(s->dac_indexed)) {
            val = s->dac_indexed[idx];
        } else if (idx >= RGB525_CURSOR_ARRAY && idx < RGB525_INDEX_LAST) {
            val = s->dac_cursor[idx - RGB525_CURSOR_ARRAY];
        } else {
            val = 0;
        }
        p9100_dac_index_step(s);
        break;
    }
    default:
        break;
    }
    trace_p9100_dac_read(rs, val);
    return val;
}

static void p9100_dac_write(P9100State *s, unsigned rs, uint8_t val)
{
    trace_p9100_dac_write(rs, val);

    switch (rs) {
    case RGB525_PAL_WR_ADDR:
        s->dac_pal_addr = val;
        s->dac_pal_seq = 0;
        break;
    case RGB525_PAL_RD_ADDR:
        s->dac_pal_addr = val;
        s->dac_pal_seq = 0;
        break;
    case RGB525_PAL_DATA:
        switch (s->dac_pal_seq) {
        case 0:
            s->dac_pal_hold[0] = val;
            break;
        case 1:
            s->dac_pal_hold[1] = val;
            break;
        default:
            s->r[s->dac_pal_addr] = s->dac_pal_hold[0];
            s->g[s->dac_pal_addr] = s->dac_pal_hold[1];
            s->b[s->dac_pal_addr] = val;
            break;
        }
        if (++s->dac_pal_seq == 3) {
            s->dac_pal_seq = 0;
            s->dac_pal_addr++;
            s->full_update = true;
        }
        break;
    case RGB525_PIXEL_MASK:
        s->dac_pixel_mask = val;
        s->full_update = true;
        break;
    case RGB525_INDEX_LOW:
        s->dac_index_lo = val;
        break;
    case RGB525_INDEX_HIGH:
        s->dac_index_hi = val;
        break;
    case RGB525_INDEX_DATA: {
        unsigned idx = p9100_dac_index(s);

        if (idx < ARRAY_SIZE(s->dac_indexed)) {
            s->dac_indexed[idx] = val;
            if (idx >= RGB525_IDX_CURSOR_CTL &&
                idx < RGB525_IDX_CURSOR_COL + 9) {
                s->cursor_dirty = true;
            }
        } else if (idx >= RGB525_CURSOR_ARRAY && idx < RGB525_INDEX_LAST) {
            s->dac_cursor[idx - RGB525_CURSOR_ARRAY] = val;
            s->cursor_dirty = true;
        }
        p9100_dac_index_step(s);
        break;
    }
    case RGB525_INDEX_CONTROL:
        s->dac_index_inc = val & 1;
        break;
    default:
        break;
    }
}

static uint8_t p9100_rop(uint8_t op, uint8_t p, uint8_t src, uint8_t dst)
{
    uint8_t r = 0;

    if (op & 0x01) {
        r |= ~p & ~src & ~dst;
    }
    if (op & 0x02) {
        r |= ~p & ~src & dst;
    }
    if (op & 0x04) {
        r |= ~p & src & ~dst;
    }
    if (op & 0x08) {
        r |= ~p & src & dst;
    }
    if (op & 0x10) {
        r |= p & ~src & ~dst;
    }
    if (op & 0x20) {
        r |= p & ~src & dst;
    }
    if (op & 0x40) {
        r |= p & src & ~dst;
    }
    if (op & 0x80) {
        r |= p & src & dst;
    }
    return r;
}

static const uint8_t de_pattern_colour[4] = {
    DE_COLOR0, DE_COLOR1, DE_COLOR2, DE_COLOR3
};

/* Cheating: swap colors */
static unsigned p9100_colour_lane(P9100State *s, unsigned lane)
{
    if (SYSCONFIG_SWAP(s->sysconfig) & P9100_SWAP_BYTE) {
        lane = (s->depth / 8) - 1 - lane;
    }
    return lane;
}

static uint8_t p9100_pattern_byte(P9100State *s, int x, int y, unsigned lane)
{
    uint32_t raster = s->de[DE_RASTER];
    uint32_t colour;

    if (raster & RASTER_SOLID_DISABLE) {
        unsigned px = (x - (int)s->de[DE_PAT_ORIGIN_X]) & 7;
        unsigned py = (y - (int)s->de[DE_PAT_ORIGIN_Y]) & 7;
        uint32_t w = s->de[DE_PATTERN0 + (py >> 1)];
        unsigned base = (py & 1) ? 0 : 16;
        unsigned p0 = (w >> base) & 0xff;
        unsigned p1 = (w >> (base + 8)) & 0xff;
        unsigned sel = (((p1 >> px) & 1) << 1) | ((p0 >> px) & 1);

        if (!(raster & RASTER_PATTERN_DEPTH)) {
            sel &= 1;
        }
        colour = s->de[de_pattern_colour[sel]];
    } else {
        colour = s->de[DE_COLOR0];
    }
    return (colour >> (8 * (p9100_colour_lane(s, lane) & 3))) & 0xff;
}

/* TODO: decode draw_mode's plane mask enable, used by NT? */
static bool p9100_accel_ready(P9100State *s, unsigned *bypp)
{
    if (!s->pitch || !s->depth) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "p9100: drawing command issued before a mode was set\n");
        return false;
    }
    *bypp = s->depth / 8;
    return true;
}

static int p9100_max_y(P9100State *s)
{
    return s->vram_size / s->pitch;
}

static int p9100_max_x(P9100State *s, unsigned bypp)
{
    return s->pitch / bypp;
}

typedef struct P9100Clip {
    int xmin, ymin, xmax, ymax;
} P9100Clip;

static void p9100_plot(P9100State *s, uint8_t *vram, unsigned bypp,
                       const P9100Clip *clip, int x, int y)
{
    uint8_t op = RASTER_MINTERMS(s->de[DE_RASTER]);
    size_t off;
    unsigned lane;

    if (x < clip->xmin || x >= clip->xmax ||
        y < clip->ymin || y >= clip->ymax) {
        return;
    }
    off = (size_t)y * s->pitch + (size_t)x * bypp;

    for (lane = 0; lane < bypp; lane++) {
        uint8_t p = p9100_pattern_byte(s, x, y, lane);
        uint8_t d = vram[off + lane];

        vram[off + lane] = p9100_rop(op, p, 0, d);
    }
}

static void p9100_fill_span(P9100State *s, uint8_t *vram, unsigned bypp,
                            const P9100Clip *clip, int y, int x0, int x1)
{
    int x;

    for (x = x0; x < x1; x++) {
        p9100_plot(s, vram, bypp, clip, x, y);
    }
}

static void p9100_line(P9100State *s, uint8_t *vram, unsigned bypp,
                       const P9100Clip *clip, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        p9100_plot(s, vram, bypp, clip, x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        if (2 * err >= dy) {
            err += dy;
            x0 += sx;
        } else {
            err += dx;
            y0 += sy;
        }
    }
}

static void p9100_quad(P9100State *s)
{
    unsigned bypp;
    uint8_t *vram;
    P9100Clip clip;
    int ymin, ymax, lo_y, hi_y, y, i;

    if (!p9100_accel_ready(s, &bypp)) {
        return;
    }
    vram = memory_region_get_ram_ptr(&s->vram);

    ymin = ymax = s->y[0];
    for (i = 1; i < 4; i++) {
        ymin = MIN(ymin, s->y[i]);
        ymax = MAX(ymax, s->y[i]);
    }

    clip.xmin = MAX(0, (int16_t)(s->de[DE_P_W_MIN] >> 16));
    clip.ymin = MAX(0, (int16_t)(s->de[DE_P_W_MIN] & 0xffff));
    clip.xmax = MIN(p9100_max_x(s, bypp),
                    (int16_t)(s->de[DE_P_W_MAX] >> 16) + 1);
    clip.ymax = MIN(p9100_max_y(s),
                    (int16_t)(s->de[DE_P_W_MAX] & 0xffff) + 1);

    lo_y = MAX(ymin, clip.ymin);
    hi_y = MIN(ymax, clip.ymax);

    trace_p9100_quad(s->x[0], s->y[0], s->x[1], s->y[1], s->x[2], s->y[2],
                     s->x[3], s->y[3], RASTER_MINTERMS(s->de[DE_RASTER]));

    for (y = lo_y; y < hi_y; y++) {
        int xs[4], n = 0, lo, hi;

        for (i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            int ya = s->y[i], yb = s->y[j], xa = s->x[i], xb = s->x[j];

            if (ya == yb) {
                continue;
            }
            if (ya > yb) {
                int t = ya; ya = yb; yb = t;
                t = xa; xa = xb; xb = t;
            }
            if (y < ya || y >= yb) {
                continue;
            }
            xs[n++] = xa + (xb - xa) * (y - ya) / (yb - ya);
        }
        if (n < 2) {
            continue;
        }
        lo = hi = xs[0];
        for (i = 1; i < n; i++) {
            lo = MIN(lo, xs[i]);
            hi = MAX(hi, xs[i]);
        }
        lo = MAX(lo, clip.xmin);
        hi = MIN(hi, clip.xmax);
        if (hi > lo) {
            p9100_fill_span(s, vram, bypp, &clip, y, lo, hi);
        }
    }

    if (s->de[DE_RASTER] & RASTER_DRAW_MODE) {
        for (i = 0; i < 4; i++) {
            int j = (i + 1) & 3;

            p9100_line(s, vram, bypp, &clip, s->x[i], s->y[i],
                       s->x[j], s->y[j]);
        }
        hi_y = MIN(ymax + 1, clip.ymax);
    }

    if (hi_y > lo_y) {
        memory_region_set_dirty(&s->vram, (ram_addr_t)lo_y * s->pitch,
                                (ram_addr_t)(hi_y - lo_y) * s->pitch);
    }
    s->interrupt |= P9100_INT_DE_IDLE;
    p9100_update_irq(s);
}

static void p9100_blit(P9100State *s)
{
    uint32_t raster = s->de[DE_RASTER];
    uint8_t op = RASTER_MINTERMS(raster);
    unsigned bypp;
    uint8_t *vram;
    int sx = MIN(s->x[0], s->x[1]), sy = MIN(s->y[0], s->y[1]);
    int dx = MIN(s->x[2], s->x[3]), dy = MIN(s->y[2], s->y[3]);
    int w = MIN(abs(s->x[1] - s->x[0]), abs(s->x[3] - s->x[2]));
    int h = MIN(abs(s->y[1] - s->y[0]), abs(s->y[3] - s->y[2]));
    int xmin, ymin, xmax, ymax;
    int row, step;

    if (!p9100_accel_ready(s, &bypp)) {
        return;
    }
    vram = memory_region_get_ram_ptr(&s->vram);

    xmin = MAX(0, (int16_t)(s->de[DE_P_W_MIN] >> 16));
    ymin = MAX(0, (int16_t)(s->de[DE_P_W_MIN] & 0xffff));
    xmax = MIN(p9100_max_x(s, bypp), (int16_t)(s->de[DE_P_W_MAX] >> 16) + 1);
    ymax = MIN(p9100_max_y(s), (int16_t)(s->de[DE_P_W_MAX] & 0xffff) + 1);

    if (dx < xmin) {
        w -= xmin - dx;
        sx += xmin - dx;
        dx = xmin;
    }
    if (dy < ymin) {
        h -= ymin - dy;
        sy += ymin - dy;
        dy = ymin;
    }
    w = MIN(w, xmax - dx);
    h = MIN(h, ymax - dy);

    w = MIN(w, p9100_max_x(s, bypp) - MAX(sx, dx));
    h = MIN(h, p9100_max_y(s) - MAX(sy, dy));
    if (w <= 0 || h <= 0 || sx < 0 || sy < 0) {
        return;
    }

    trace_p9100_blit(sx, sy, dx, dy, w, h, op);

    if (dy > sy) {
        row = h - 1;
        step = -1;
    } else {
        row = 0;
        step = 1;
    }

    for (; row >= 0 && row < h; row += step) {
        size_t soff = (size_t)(sy + row) * s->pitch + (size_t)sx * bypp;
        size_t doff = (size_t)(dy + row) * s->pitch + (size_t)dx * bypp;
        size_t len = (size_t)w * bypp;
        int back = dx > sx;
        size_t i;

        for (i = 0; i < len; i++) {
            size_t k = back ? len - 1 - i : i;
            unsigned lane = k % bypp;
            uint8_t p = p9100_pattern_byte(s, dx + (int)(k / bypp), dy + row,
                                           lane);
            uint8_t src = vram[soff + k];
            uint8_t d = vram[doff + k];

            vram[doff + k] = p9100_rop(op, p, src, d);
        }
    }

    memory_region_set_dirty(&s->vram, (ram_addr_t)dy * s->pitch,
                            (ram_addr_t)h * s->pitch);
    s->interrupt |= P9100_INT_DE_IDLE;
    p9100_update_irq(s);
}

static void p9100_pixel8(P9100State *s, uint32_t val)
{
    uint8_t op = RASTER_MINTERMS(s->de[DE_RASTER]);
    int left = s->x[0], right = s->x[2];
    unsigned bypp;
    uint8_t *vram;
    int i, dirty_lo = -1, dirty_hi = -1;
    int bx0, by0, bx1, by1;
    bool copy;

    if (!p9100_accel_ready(s, &bypp)) {
        return;
    }
    if (right <= left) {
        qemu_log_mask(LOG_GUEST_ERROR, "p9100: pixel8 with empty span "
                      "[%d,%d)\n", left, right);
        return;
    }
    vram = memory_region_get_ram_ptr(&s->vram);

    if (s->x[1] < left || s->x[1] >= right) {
        s->x[1] = left;
    }

    copy = op == 0xcc && s->de[DE_PMASK] == 0xffffffff;

    bx0 = MAX(0, (int16_t)(s->de[DE_B_W_MIN] >> 16));
    by0 = MAX(0, (int16_t)(s->de[DE_B_W_MIN] & 0xffff));
    bx1 = MIN((int)s->pitch, (int16_t)(s->de[DE_B_W_MAX] >> 16) + 1);
    by1 = MIN(p9100_max_y(s), (int16_t)(s->de[DE_B_W_MAX] & 0xffff) + 1);

    for (i = 0; i < 4; i++) {
        int x = s->x[1] + i;
        size_t off;
        unsigned lane;
        uint8_t src, p, d;

        if (x >= right) {
            s->de[DE_PIXEL8] = val << (8 * i);
            break;
        }
        if (s->y[1] < by0 || s->y[1] >= by1 || x < bx0 || x >= bx1) {
            continue;
        }
        off = (size_t)s->y[1] * s->pitch + x;
        src = (val >> (8 * (3 - i))) & 0xff;
        if (copy) {
            vram[off] = src;
        } else {
            lane = x % bypp;
            p = p9100_pattern_byte(s, x / bypp, s->y[1], lane);
            d = vram[off];
            vram[off] = p9100_rop(op, p, src, d);
        }
        if (dirty_lo < 0) {
            dirty_lo = off;
        }
        dirty_hi = off;
    }

    if (dirty_lo >= 0) {
        memory_region_set_dirty(&s->vram, dirty_lo, dirty_hi - dirty_lo + 1);
    }

    s->x[1] += 4;
    if (s->x[1] >= right) {
        s->x[1] = left;
        s->y[1] += s->y[3];
    }
}

static void p9100_pixel1(P9100State *s, unsigned npix, uint32_t val)
{
    uint32_t raster = s->de[DE_RASTER];
    uint8_t op = RASTER_MINTERMS(raster);
    bool transparent = raster & RASTER_PIXEL1_TRANS;
    int left = s->x[0], right = s->x[2];
    int dirty_lo = -1, dirty_hi = -1;
    unsigned bypp, i;
    uint8_t *vram;

    if (!p9100_accel_ready(s, &bypp)) {
        return;
    }
    if (right <= left) {
        qemu_log_mask(LOG_GUEST_ERROR, "p9100: pixel1 with empty span "
                      "[%d,%d)\n", left, right);
        return;
    }
    vram = memory_region_get_ram_ptr(&s->vram);

    if (s->x[1] < left || s->x[1] > right) {
        s->x[1] = left;
    }

    for (i = 0; i < npix; i++) {
        unsigned bit = (val >> (31 - i)) & 1;
        uint32_t colour;
        size_t off;
        unsigned lane;

        if (s->x[1] >= right) {
            s->x[1] = left;
            s->y[1] += s->y[3];
        }
        if (bit == 0 && transparent) {
            s->x[1]++;
            continue;
        }
        colour = s->de[bit ? DE_COLOR1 : DE_COLOR0];

        if (s->y[1] >= 0 && s->y[1] < p9100_max_y(s) && s->x[1] >= 0 &&
            (s->x[1] + 1) * (int)bypp <= (int)s->pitch) {
            off = (size_t)s->y[1] * s->pitch + (size_t)s->x[1] * bypp;

            for (lane = 0; lane < bypp; lane++) {
                uint8_t src = (colour >> (8 * (p9100_colour_lane(s, lane)
                                                & 3))) & 0xff;
                uint8_t p = p9100_pattern_byte(s, s->x[1], s->y[1], lane);
                uint8_t d = vram[off + lane];

                vram[off + lane] = p9100_rop(op, p, src, d);
            }
            if (dirty_lo < 0) {
                dirty_lo = off;
            }
            dirty_hi = off + bypp - 1;
        }
        s->x[1]++;
    }

    if (dirty_lo >= 0) {
        memory_region_set_dirty(&s->vram, dirty_lo, dirty_hi - dirty_lo + 1);
    }
}

static void p9100_window_origin(P9100State *s, int32_t *ox, int32_t *oy)
{
    *ox = (int16_t)(s->pe[PE_W_OFF_XY] >> 16);
    *oy = (int16_t)(s->pe[PE_W_OFF_XY] & 0xffff);
}

static void p9100_coord_write(P9100State *s, unsigned reg, unsigned sel,
                              int32_t ox, int32_t oy, uint32_t val)
{
    switch (sel) {
    case COORD_SEL_X:
        s->x[reg] = (int32_t)val + ox;
        break;
    case COORD_SEL_Y:
        s->y[reg] = (int32_t)val + oy;
        break;
    case COORD_SEL_XY:
        s->x[reg] = (int16_t)(val >> 16) + ox;
        s->y[reg] = (int16_t)(val & 0xffff) + oy;
        break;
    default:
        break;
    }
}

static uint32_t p9100_coord_read(P9100State *s, unsigned reg, unsigned sel)
{
    switch (sel) {
    case COORD_SEL_X:
        return s->x[reg];
    case COORD_SEL_Y:
        return s->y[reg];
    case COORD_SEL_XY:
        return ((uint32_t)(s->x[reg] & 0xffff) << 16) | (s->y[reg] & 0xffff);
    default:
        return 0;
    }
}

static void p9100_loadcoord_write(P9100State *s, unsigned vtype, unsigned sel,
                                  bool vertex_rel, uint32_t val)
{
    int32_t ox, oy;
    static const unsigned needed[8] = { 1, 2, 3, 4, 2, 1, 1, 1 };
    unsigned n = needed[vtype];
    unsigned i;

    if (s->load_idx >= n) {
        s->load_idx = 0;
    }

    if (vertex_rel) {
        ox = s->last_vx;
        oy = s->last_vy;
    } else {
        p9100_window_origin(s, &ox, &oy);
    }
    p9100_coord_write(s, s->load_idx, sel, ox, oy, val);

    if (sel == COORD_SEL_X) {
        return;
    }
    s->last_vx = s->x[s->load_idx];
    s->last_vy = s->y[s->load_idx];
    s->load_idx++;

    if (s->load_idx < n) {
        return;
    }
    s->load_idx = 0;

    switch (vtype) {
    case VTYPE_RECT:
        s->x[3] = s->x[0];
        s->y[3] = s->y[1];
        s->x[2] = s->x[1];
        s->y[2] = s->y[1];
        s->y[1] = s->y[0];
        break;
    case VTYPE_TRIANGLE:
        s->x[3] = s->x[2];
        s->y[3] = s->y[2];
        break;
    case VTYPE_LINE:
        s->x[2] = s->x[1];
        s->y[2] = s->y[1];
        s->x[3] = s->x[0];
        s->y[3] = s->y[0];
        break;
    case VTYPE_POINT:
        for (i = 1; i < 4; i++) {
            s->x[i] = s->x[0];
            s->y[i] = s->y[0];
        }
        break;
    default:
        break;
    }
}

static uint32_t p9100_apply_wc(uint32_t old, uint32_t val, uint32_t bits)
{
    uint32_t bit;

    for (bit = 0; bits >> bit; bit += 2) {
        if (!(bits & (1U << bit))) {
            continue;
        }
        if (val & (1U << (bit + 1))) {
            old = (old & ~(1U << bit)) | (val & (1U << bit));
        }
    }
    return old;
}

static uint32_t p9100_swap_data(hwaddr addr, uint32_t val)
{
    if (addr & P9100_ACC_SWAP_HALF) {
        val = (val << 16) | (val >> 16);
    }
    if (addr & P9100_ACC_SWAP_BYTE) {
        val = ((val & 0x00ff00ff) << 8) | ((val >> 8) & 0x00ff00ff);
    }
    if (addr & P9100_ACC_SWAP_BIT) {
        val = ((val & 0x55555555) << 1) | ((val >> 1) & 0x55555555);
        val = ((val & 0x33333333) << 2) | ((val >> 2) & 0x33333333);
        val = ((val & 0x0f0f0f0f) << 4) | ((val >> 4) & 0x0f0f0f0f);
    }
    return val;
}

static uint64_t p9100_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    P9100State *s = opaque;
    unsigned reg = addr & P9100_REG_MASK;
    uint64_t val = 0;

    switch (reg) {
    case P9100_SYSCONFIG:
        val = (s->sysconfig & ~SYSCONFIG_ID_MASK) | s->silicon_rev;
        break;
    case P9100_INTERRUPT:
        val = s->interrupt | P9100_INT_DE_IDLE;
        break;
    case P9100_INTERRUPT_EN:
        val = s->interrupt_en;
        break;
    case P9100_ALT_READ_BANK:
        val = s->alt_read_bank;
        break;
    case P9100_ALT_WRITE_BANK:
        val = s->alt_write_bank;
        break;
    case P9100_HRZC:
        s->video[(P9100_HRZC - P9100_VIDEO_FIRST) / 4]++;
        val = s->video[(P9100_HRZC - P9100_VIDEO_FIRST) / 4] & 0xfff;
        break;
    case P9100_VRTC:
        s->video[(P9100_VRTC - P9100_VIDEO_FIRST) / 4]++;
        val = s->video[(P9100_VRTC - P9100_VIDEO_FIRST) / 4] & 0xfff;
        break;
    case P9100_VIDEO_FIRST ... P9100_HRZC - 1:
    case P9100_HRZT ... P9100_VRTC - 1:
    case P9100_VRTT ... P9100_VIDEO_LAST:
        val = s->video[(reg - P9100_VIDEO_FIRST) / 4];
        break;
    case P9100_PU_CONFIG:
        val = s->pu_config;
        break;
    case P9100_VRAM_FIRST ... P9100_PU_CONFIG - 1:
    case P9100_PU_CONFIG + 4 ... P9100_VRAM_LAST:
        val = s->vram_ctl[(reg - P9100_VRAM_FIRST) / 4];
        break;
    case P9100_DAC_FIRST ... P9100_DAC_LAST:
        val = p9100_dac_read(s, P9100_DAC_RS(reg))
              << (8 * P9100_DAC_LANE(reg));
        break;
    case P9100_QUAD_CMD:
        p9100_quad(s);
        val = 0;
        break;
    case P9100_BLIT_CMD:
        p9100_blit(s);
        val = 0;
        break;
    case P9100_STATUS:
        val = 0;                        /* drawing engine always idle */
        break;
    case P9100_PIXEL8_CMD ... P9100_PE_LAST:
        val = s->pe[(reg - P9100_PE_FIRST) / 4];
        break;
    case P9100_DE_FIRST ... P9100_DE_LAST:
        val = s->de[(reg - P9100_DE_FIRST) / 4];
        break;
    case P9100_COORD_FIRST ... P9100_COORD_LAST:
        val = p9100_coord_read(s, COORD_REG(reg), COORD_SEL(reg));
        break;
    case P9100_LOADCOORD_FIRST ... P9100_LOADCOORD_LAST:
        val = 0;                        /* write only */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "p9100: read from unimplemented register "
                      "0x%03x (window offset 0x%" HWADDR_PRIx ")\n", reg, addr);
        break;
    }

    val = p9100_swap_data(addr, val);
    trace_p9100_reg_read(reg, val, size);
    return val;
}

static void p9100_reg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    P9100State *s = opaque;
    unsigned reg = addr & P9100_REG_MASK;

    val = p9100_swap_data(addr, val);
    trace_p9100_reg_write(reg, val, size);

    switch (reg) {
    case P9100_SYSCONFIG:
        s->sysconfig = val;
        if (SYSCONFIG_SWAP(val)) {
            /* TODO: implement this rather than compensating for it by cheating */
            qemu_log_mask(LOG_UNIMP, "p9100: frame buffer swap control %u "
                          "not implemented\n",
                          (unsigned)SYSCONFIG_SWAP(val));
        }
        p9100_update_mode(s);
        s->full_update = true;
        break;
    case P9100_INTERRUPT:
        s->interrupt = p9100_apply_wc(s->interrupt, val,
                                      P9100_INT_DE_IDLE | P9100_INT_PICKED |
                                      P9100_INT_VBLANKED);
        p9100_update_irq(s);
        break;
    case P9100_INTERRUPT_EN:
        s->interrupt_en = p9100_apply_wc(s->interrupt_en, val,
                                         P9100_INT_DE_IDLE | P9100_INT_PICKED |
                                         P9100_INT_VBLANKED | P9100_INT_MASTER);
        p9100_update_irq(s);
        break;
    case P9100_ALT_READ_BANK:
        s->alt_read_bank = val;
        break;
    case P9100_ALT_WRITE_BANK:
        s->alt_write_bank = val;
        break;
    case P9100_HRZC:
    case P9100_VRTC:
        break;                          /* read only */
    /* TODO: implement the screen repaint address, for panning and double buffering */
    case P9100_SRADDR:
        if (val) {
            qemu_log_mask(LOG_UNIMP, "p9100: non-zero screen repaint address "
                          "0x%" PRIx64 " ignored\n", val);
        }
        s->video[(reg - P9100_VIDEO_FIRST) / 4] = val;
        break;
    case P9100_VIDEO_FIRST ... P9100_HRZC - 1:
    case P9100_HRZT ... P9100_VRTC - 1:
    case P9100_VRTT ... P9100_SRADDR - 1:
    case P9100_SRTCTL ... P9100_VIDEO_LAST:
        s->video[(reg - P9100_VIDEO_FIRST) / 4] = val;
        p9100_update_mode(s);
        s->full_update = true;
        break;
    case P9100_PU_CONFIG:
        break;                          /* read only */
    case P9100_VRAM_FIRST ... P9100_PU_CONFIG - 1:
    case P9100_PU_CONFIG + 4 ... P9100_VRAM_LAST:
        s->vram_ctl[(reg - P9100_VRAM_FIRST) / 4] = val;
        break;
    case P9100_DAC_FIRST ... P9100_DAC_LAST:
        p9100_dac_write(s, P9100_DAC_RS(reg),
                        (val >> (8 * P9100_DAC_LANE(reg))) & 0xff);
        break;
    case P9100_QUAD_CMD:
        p9100_quad(s);
        break;
    case P9100_BLIT_CMD:
        p9100_blit(s);
        break;
    case P9100_STATUS:
        break;                          /* read only */
    case 0x4000:
        /*
         * Seen only on OS/2 PPC edition, might not be correct?
		 */
        p9100_pixel8(s, val);
        break;
    case P9100_PIXEL8_CMD:
        p9100_pixel8(s, val);
        break;
    case P9100_PIXEL1_FIRST ... P9100_PIXEL1_LAST:
        p9100_pixel1(s, P9100_PIXEL1_NPIX(reg), val);
        break;
    case P9100_PIXEL8_CMD + 4 ... P9100_PIXEL1_FIRST - 1:
    case P9100_PIXEL1_LAST + 4 ... P9100_PE_LAST:
        s->pe[(reg - P9100_PE_FIRST) / 4] = val;
        break;
    case P9100_DE_FIRST ... P9100_DE_LAST:
        s->de[(reg - P9100_DE_FIRST) / 4] = val;
        break;
    case P9100_COORD_FIRST ... P9100_COORD_LAST:
        {
            int32_t ox = 0, oy = 0;

            if (reg & P9100_COORD_WIN_REL) {
                p9100_window_origin(s, &ox, &oy);
            }
            p9100_coord_write(s, COORD_REG(reg), COORD_SEL(reg), ox, oy, val);
        }
        break;
    case P9100_LOADCOORD_FIRST ... P9100_LOADCOORD_LAST:
        p9100_loadcoord_write(s, COORD_VTYPE(reg), COORD_SEL(reg),
                              (reg & P9100_COORD_WIN_REL) != 0, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "p9100: write 0x%" PRIx64 " to unimplemented "
                      "register 0x%03x (window offset 0x%" HWADDR_PRIx ")\n",
                      val, reg, addr);
        break;
    }
}

static const MemoryRegionOps p9100_reg_ops = {
    .read = p9100_reg_read,
    .write = p9100_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void p9100_reset_hold(Object *obj, ResetType type)
{
    P9100State *s = P9100(obj);

    s->sysconfig = 0;
    s->interrupt = 0;
    s->interrupt_en = 0;
    s->alt_read_bank = 0;
    s->alt_write_bank = 0;
    memset(s->video, 0, sizeof(s->video));
    memset(s->vram_ctl, 0, sizeof(s->vram_ctl));
    memset(s->pe, 0, sizeof(s->pe));
    memset(s->de, 0, sizeof(s->de));
    memset(s->x, 0, sizeof(s->x));
    memset(s->y, 0, sizeof(s->y));
    s->load_idx = 0;
    s->last_vx = 0;
    s->last_vy = 0;
    s->de[DE_PMASK] = 0xffffffff;
    s->de[DE_B_W_MAX] = 0x3fff3fff;
    s->vram_ctl[(P9100_PU_CONFIG - P9100_VRAM_FIRST) / 4] = s->pu_config;

    s->dac_index_lo = 0;
    s->dac_index_hi = 0;
    s->dac_pal_addr = 0;
    s->dac_pal_seq = 0;
    s->dac_pixel_mask = 0xff;
    memset(s->dac_indexed, 0, sizeof(s->dac_indexed));
    memset(s->dac_cursor, 0, sizeof(s->dac_cursor));
    s->dac_index_inc = false;
    s->cursor_dirty = true;
    s->cursor_drawn = false;

    s->width = s->height = s->depth = s->pitch = 0;
    s->full_update = true;
    pci_set_irq(PCI_DEVICE(s), 0);

    if (s->pu_config & PU_CONFIG_MODESELECT) {
        s->parent_obj.config[P9100_CFG65] |= CFG65_MODESELECT;
    } else {
        s->parent_obj.config[P9100_CFG65] &= ~CFG65_MODESELECT;
    }
}

static void p9100_realize(PCIDevice *dev, Error **errp)
{
    P9100State *s = P9100(dev);
    Object *obj = OBJECT(dev);
    unsigned i;

    if (s->vram_size < 1 * MiB || s->vram_size > P9100_FB_WINDOW ||
        s->vram_size & (s->vram_size - 1)) {
        error_setg(errp, "p9100: vram_size must be a power of two between "
                   "1 and %u MB", (unsigned)(P9100_FB_WINDOW / MiB));
        return;
    }

    s->con = qemu_graphic_console_create(DEVICE(dev), 0, &p9100_gfx_ops, s);

    if (!memory_region_init_ram(&s->vram, obj, "p9100.vram", s->vram_size,
                                errp)) {
        return;
    }

    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);

    memory_region_init(&s->bar, obj, "p9100.bar", P9100_BAR_SIZE);

    for (i = 0; i < P9100_FB_WINDOW / s->vram_size; i++) {
        g_autofree char *name = g_strdup_printf("p9100.fb[%u]", i);

        memory_region_init_alias(&s->fb[i], obj, name, &s->vram, 0,
                                 s->vram_size);
        memory_region_add_subregion(&s->bar,
                                    P9100_FB_BASE + (hwaddr)i * s->vram_size,
                                    &s->fb[i]);
    }

    memory_region_init_io(&s->regs, obj, &p9100_reg_ops, s, "p9100.regs",
                          P9100_REG_WINDOW);
    memory_region_add_subregion(&s->bar, 0, &s->regs);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    pci_set_byte(&dev->config[PCI_REVISION_ID], s->silicon_rev);

    if (s->pu_config & PU_CONFIG_VGA_ABSENT) {
        pci_config_set_class(dev->config, PCI_CLASS_DISPLAY_OTHER);
    }
    dev->config[PCI_INTERRUPT_PIN] = 1;
    dev->config[P9100_CFG64] = (s->pu_config >> 24) & 0xfc;
    dev->wmask[P9100_CFG65] = 0xff;
    dev->wmask[P9100_CFG66] = 0xff;
}

static void p9100_exit(PCIDevice *dev)
{
    P9100State *s = P9100(dev);

    qemu_graphic_console_close(s->con);
}

static const VMStateDescription vmstate_p9100 = {
    .name = "p9100",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, P9100State),
        VMSTATE_UINT32(sysconfig, P9100State),
        VMSTATE_UINT32(interrupt, P9100State),
        VMSTATE_UINT32(interrupt_en, P9100State),
        VMSTATE_UINT32(alt_read_bank, P9100State),
        VMSTATE_UINT32(alt_write_bank, P9100State),
        VMSTATE_UINT32_ARRAY(video, P9100State, 32),
        VMSTATE_UINT32_ARRAY(vram_ctl, P9100State, 32),
        VMSTATE_UINT32_ARRAY(pe, P9100State, 128),
        VMSTATE_UINT32_ARRAY(de, P9100State, 128),
        VMSTATE_INT32_ARRAY(x, P9100State, 4),
        VMSTATE_INT32_ARRAY(y, P9100State, 4),
        VMSTATE_UINT8(dac_index_lo, P9100State),
        VMSTATE_UINT8(dac_index_hi, P9100State),
        VMSTATE_UINT8_ARRAY(dac_indexed, P9100State, 256),
        VMSTATE_UINT8_ARRAY(dac_cursor, P9100State, RGB525_CURSOR_BYTES),
        VMSTATE_BOOL(dac_index_inc, P9100State),
        VMSTATE_UINT8(dac_pal_addr, P9100State),
        VMSTATE_UINT8(dac_pal_seq, P9100State),
        VMSTATE_UINT8(dac_pixel_mask, P9100State),
        VMSTATE_UINT8_ARRAY(r, P9100State, 256),
        VMSTATE_UINT8_ARRAY(g, P9100State, 256),
        VMSTATE_UINT8_ARRAY(b, P9100State, 256),
        VMSTATE_END_OF_LIST()
    }
};

static const Property p9100_properties[] = {
    DEFINE_PROP_UINT32("vram_size", P9100State, vram_size, 4 * MiB),
    DEFINE_PROP_UINT32("pu-config", P9100State, pu_config, PU_CONFIG_DEFAULT),
    DEFINE_PROP_UINT8("silicon-rev", P9100State, silicon_rev, 4),
};

static void p9100_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = p9100_realize;
    k->exit = p9100_exit;
    k->vendor_id = PCI_VENDOR_ID_WEITEK;
    k->device_id = PCI_DEVICE_ID_WEITEK_P9100;
    k->class_id = PCI_CLASS_DISPLAY_VGA;

    rc->phases.hold = p9100_reset_hold;

    dc->desc = "Weitek Power 9100";
    dc->vmsd = &vmstate_p9100;
    device_class_set_props(dc, p9100_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo p9100_types[] = {
    {
        .name          = TYPE_P9100,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(P9100State),
        .class_init    = p9100_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { }
        },
    },
};

DEFINE_TYPES(p9100_types)
