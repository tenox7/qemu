/*
 * S3 Trio64V+ (86C765) PCI VGA with the 8514-style graphics engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "system/reset.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "vga_regs.h"
#include "vga_int.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_S3_TRIO "s3-trio64"
OBJECT_DECLARE_SIMPLE_TYPE(S3TrioState, S3_TRIO)

#define S3_BAR_SIZE     (64 * MiB)
#define S3_APERTURE     (16 * MiB)

/* enhanced command registers, by I/O port */
enum {
    GE_SUBSYS = 0x42e8,
    GE_ADVFUNC = 0x4ae8,
    GE_CUR_Y = 0x82e8,
    GE_CUR_X = 0x86e8,
    GE_DESTY = 0x8ae8,
    GE_DESTX = 0x8ee8,
    GE_ERR = 0x92e8,
    GE_MAJ = 0x96e8,
    GE_CMD = 0x9ae8,
    GE_SSV = 0x9ee8,
    GE_BKGD_COLOR = 0xa2e8,
    GE_FRGD_COLOR = 0xa6e8,
    GE_WRT_MASK = 0xaae8,
    GE_RD_MASK = 0xaee8,
    GE_COLOR_CMP = 0xb2e8,
    GE_BKGD_MIX = 0xb6e8,
    GE_FRGD_MIX = 0xbae8,
    GE_MULTIFUNC = 0xbee8,
    GE_PIX_TRANS = 0xe2e8,
};

static const uint16_t s3_ge_ports[] = {
    GE_SUBSYS, GE_ADVFUNC, GE_CUR_Y, GE_CUR_X, GE_DESTY, GE_DESTX, GE_ERR,
    GE_MAJ, GE_CMD, GE_SSV, GE_BKGD_COLOR, GE_FRGD_COLOR, GE_WRT_MASK,
    GE_RD_MASK, GE_COLOR_CMP, GE_BKGD_MIX, GE_FRGD_MIX, GE_MULTIFUNC,
    GE_PIX_TRANS,
};
#define S3_GE_PORTS ARRAY_SIZE(s3_ge_ports)

/* command register bits */
#define CMD_ACROSS      0x0002
#define CMD_LAST_OFF    0x0004
#define CMD_RADIAL      0x0008
#define CMD_DRAW        0x0010
#define CMD_XPOS        0x0020
#define CMD_YMAJ        0x0040
#define CMD_YPOS        0x0080
#define CMD_WAIT        0x0100
#define CMD_BUS(c)      (((c) >> 9) & 3)
#define CMD_SWAP        0x1000
#define CMD_TYPE(c)     ((c) >> 13)

typedef struct S3GEPort {
    struct S3TrioState *s;
    uint16_t port;
} S3GEPort;

typedef struct S3GE {
    uint16_t cur_x, cur_y, desty, destx, err, maj, cmd;
    uint32_t bkgd_color, frgd_color, wrt_mask, rd_mask, color_cmp;
    uint16_t bkgd_mix, frgd_mix;
    uint16_t mf[16];
    uint16_t subsys_cntl, subsys_stat, advfunc;
    /* command waiting for data on PIX_TRANS */
    bool wait;
    int x, y, sx, sy, w, h;
    uint32_t pend;
    int pend_bytes;
    /* dirty span of the current command */
    uint32_t dirty_lo, dirty_hi;
} S3GE;

struct S3TrioState {
    PCIDevice parent_obj;
    VGACommonState vga;
    MemoryRegion bar, lfb, lfb_be, mmio, mmio_be, vga_io, lowmem;
    MemoryRegion vga_io_mono, vga_io_color;
    MemoryRegion ge_io[S3_GE_PORTS];
    S3GEPort ge_port[S3_GE_PORTS];
    uint8_t sr_index;
    uint32_t hwc_fg, hwc_bg;
    uint8_t hwc_stack;
    int hwc_last_y, hwc_last_h;
    S3GE ge;
};

/* ---------------------------------------------------------------- helpers */

static inline unsigned s3_ge_bpp(S3TrioState *s)
{
    switch ((s->vga.cr[0x50] >> 4) & 3) {
    case 1:
        return 2;
    case 3:
        return 4;
    default:
        return 1;
    }
}

static unsigned s3_ge_pitch(S3TrioState *s)
{
    uint8_t cr50 = s->vga.cr[0x50];

    switch (((cr50 & 1) << 2) | (cr50 >> 6)) {
    case 1:
        return 640;
    case 2:
        return 800;
    case 3:
        return 1280;
    case 4:
        return 1152;
    case 6:
        return 1600;
    default:
        return (s->vga.cr[0x31] & 2) ? 2048 : 1024;
    }
}

static inline uint32_t s3_bank(S3TrioState *s)
{
    uint8_t *cr = s->vga.cr;

    if (!(cr[0x31] & 1)) {
        return 0;
    }
    if (cr[0x6a] & 0x3f) {
        return cr[0x6a] & 0x3f;
    }
    return (cr[0x35] & 0x0f) | ((cr[0x51] & 0x0c) << 2);
}

static inline uint32_t s3_vram_mask(S3TrioState *s)
{
    return s->vga.vram_size - 1;
}

/* ---------------------------------------------------------------- engine */

static uint32_t s3_px_read(S3TrioState *s, uint32_t addr)
{
    uint8_t *p = s->vga.vram_ptr + (addr & s3_vram_mask(s));

    switch (s3_ge_bpp(s)) {
    case 2:
        return lduw_le_p(p);
    case 4:
        return ldl_le_p(p);
    default:
        return *p;
    }
}

static void s3_px_write(S3TrioState *s, uint32_t addr, uint32_t val)
{
    S3GE *ge = &s->ge;
    unsigned bpp = s3_ge_bpp(s);
    uint8_t *p;

    addr &= s3_vram_mask(s);
    p = s->vga.vram_ptr + addr;
    switch (bpp) {
    case 2:
        stw_le_p(p, val);
        break;
    case 4:
        stl_le_p(p, val);
        break;
    default:
        *p = val;
    }
    ge->dirty_lo = MIN(ge->dirty_lo, addr);
    ge->dirty_hi = MAX(ge->dirty_hi, addr + bpp);
}

static void s3_ge_flush_dirty(S3TrioState *s)
{
    S3GE *ge = &s->ge;

    if (ge->dirty_hi <= ge->dirty_lo) {
        return;
    }
    memory_region_set_dirty(&s->vga.vram, ge->dirty_lo,
                            ge->dirty_hi - ge->dirty_lo);
    ge->dirty_lo = UINT32_MAX;
    ge->dirty_hi = 0;
}

static uint32_t s3_base(S3TrioState *s, bool src)
{
    S3GE *ge = &s->ge;
    unsigned misc2 = (ge->mf[0xd] >> (src ? 4 : 0)) & 7;

    if (misc2) {
        return misc2 * MiB;
    }
    return ((ge->mf[0xe] >> (src ? 2 : 0)) & 3) * MiB;
}

static inline uint32_t s3_px_addr(S3TrioState *s, uint32_t base, int x, int y)
{
    return base + ((y & 0xfff) * s3_ge_pitch(s) + (x & 0xfff)) * s3_ge_bpp(s);
}

static uint32_t s3_rop(unsigned mix, uint32_t src, uint32_t dst)
{
    switch (mix & 0xf) {
    case 0x0:
        return ~dst;
    case 0x1:
        return 0;
    case 0x2:
        return ~0;
    case 0x3:
        return dst;
    case 0x4:
        return ~src;
    case 0x5:
        return src ^ dst;
    case 0x6:
        return ~(src ^ dst);
    case 0x7:
        return src;
    case 0x8:
        return ~(src & dst);
    case 0x9:
        return ~src | dst;
    case 0xa:
        return src | ~dst;
    case 0xb:
        return src | dst;
    case 0xc:
        return src & dst;
    case 0xd:
        return src & ~dst;
    case 0xe:
        return ~src & dst;
    default:
        return ~(src | dst);
    }
}

static bool s3_clipped(S3TrioState *s, int x, int y)
{
    S3GE *ge = &s->ge;
    bool in;

    x &= 0xfff;
    y &= 0xfff;
    in = x >= (ge->mf[2] & 0xfff) && x <= (ge->mf[4] & 0xfff) &&
         y >= (ge->mf[1] & 0xfff) && y <= (ge->mf[3] & 0xfff);
    return (ge->mf[0xe] & 0x20) ? in : !in;
}

/* pick fg or bg mix: source color, then mix with the destination pixel */
static void s3_put(S3TrioState *s, int x, int y, bool fg, uint32_t cpu,
                   uint32_t mem)
{
    S3GE *ge = &s->ge;
    uint16_t mix = fg ? ge->frgd_mix : ge->bkgd_mix;
    uint32_t addr, src, dst, res;

    if (!(ge->cmd & CMD_DRAW) || s3_clipped(s, x, y)) {
        return;
    }
    switch ((mix >> 5) & 3) {
    case 0:
        src = ge->bkgd_color;
        break;
    case 1:
        src = ge->frgd_color;
        break;
    case 2:
        src = cpu;
        break;
    default:
        src = mem;
    }
    if (ge->mf[0xe] & 0x100) {
        bool eq = src == ge->color_cmp;

        if ((ge->mf[0xe] & 0x80) ? !eq : eq) {
            return;
        }
    }
    addr = s3_px_addr(s, s3_base(s, false), x, y);
    dst = s3_px_read(s, addr);
    res = s3_rop(mix, src, dst);
    s3_px_write(s, addr, (res & ge->wrt_mask) | (dst & ~ge->wrt_mask));
}

/* mix select from display memory: all read-mask planes set */
static inline bool s3_mem_fg(S3TrioState *s, uint32_t mem)
{
    uint32_t m = s->ge.rd_mask;

    if (s3_ge_bpp(s) == 1) {
        m &= 0xff;
    } else if (s3_ge_bpp(s) == 2) {
        m &= 0xffff;
    }
    return (mem & m) == m;
}

static inline unsigned s3_pix_sel(S3TrioState *s)
{
    return (s->ge.mf[0xa] >> 6) & 3;
}

static bool s3_mono_data(S3TrioState *s)
{
    S3GE *ge = &s->ge;

    if ((ge->frgd_mix & 0x60) == 0x40 && (ge->bkgd_mix & 0x60) == 0x40) {
        return false;
    }
    return s3_pix_sel(s) == 2 || (ge->cmd & CMD_ACROSS);
}

static void s3_step(int dir, int *x, int *y)
{
    static const int dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int dy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };

    *x += dx[dir];
    *y += dy[dir];
}

static void s3_ge_done(S3TrioState *s)
{
    s->ge.wait = false;
    s3_ge_flush_dirty(s);
}

/* rectangle / image transfer: one pixel at the current position */
static void s3_rect_pixel(S3TrioState *s, bool fg, uint32_t cpu)
{
    S3GE *ge = &s->ge;
    uint32_t mem = 0;

    if (s3_pix_sel(s) == 3 || ((ge->frgd_mix | ge->bkgd_mix) & 0x60) == 0x60) {
        mem = s3_px_read(s, s3_px_addr(s, s3_base(s, false), ge->x, ge->y));
        if (s3_pix_sel(s) == 3) {
            fg = s3_mem_fg(s, mem);
        }
    }
    s3_put(s, ge->x, ge->y, fg, cpu, mem);
}

/* advance a rectangle walk; returns true at the end of a line */
static bool s3_rect_next(S3TrioState *s)
{
    S3GE *ge = &s->ge;

    ge->x += (ge->cmd & CMD_XPOS) ? 1 : -1;
    if (--ge->sx > 0) {
        return false;
    }
    ge->x = ge->cur_x & 0xfff;
    ge->sx = ge->w;
    ge->y += (ge->cmd & CMD_YPOS) ? 1 : -1;
    if (--ge->sy == 0) {
        ge->cur_y = ge->y & 0xfff;
        s3_ge_done(s);
    }
    return true;
}

static void s3_rect_start(S3TrioState *s)
{
    S3GE *ge = &s->ge;

    ge->x = ge->cur_x & 0xfff;
    ge->y = ge->cur_y & 0xfff;
    ge->w = ge->sx = (ge->maj & 0xfff) + 1;
    ge->h = ge->sy = (ge->mf[0] & 0xfff) + 1;
    if (ge->cmd & CMD_WAIT) {
        ge->wait = true;
        ge->pend_bytes = 0;
        return;
    }
    while (ge->sy) {
        s3_rect_pixel(s, true, 0);
        s3_rect_next(s);
    }
}

static void s3_blit(S3TrioState *s, bool pattern)
{
    S3GE *ge = &s->ge;
    int w = (ge->maj & 0xfff) + 1, h = (ge->mf[0] & 0xfff) + 1;
    int sx0 = ge->cur_x & 0xfff, sy0 = ge->cur_y & 0xfff;
    int dx = ge->destx & 0xfff, dy = ge->desty & 0xfff;
    int xs = (ge->cmd & CMD_XPOS) ? 1 : -1, ys = (ge->cmd & CMD_YPOS) ? 1 : -1;
    uint32_t sbase = s3_base(s, true);

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int x = dx + i * xs, y = dy + j * ys;
            int px = pattern ? sx0 + (x & 7) : sx0 + i * xs;
            int py = pattern ? sy0 + (y & 7) : sy0 + j * ys;
            uint32_t mem = s3_px_read(s, s3_px_addr(s, sbase, px, py));
            bool fg = s3_pix_sel(s) == 3 ? s3_mem_fg(s, mem) : true;

            s3_put(s, x, y, fg, 0, mem);
        }
    }
    ge->destx = (dx + w * xs) & 0xfff;
    ge->desty = (dy + h * ys) & 0xfff;
    ge->cur_y = (sy0 + h * ys) & 0xfff;
    s3_ge_done(s);
}

static void s3_line_pixel(S3TrioState *s, int x, int y, bool last)
{
    S3GE *ge = &s->ge;
    uint32_t mem = 0;
    bool fg = true;

    if (last && (ge->cmd & CMD_LAST_OFF)) {
        return;
    }
    if (s3_pix_sel(s) == 3) {
        mem = s3_px_read(s, s3_px_addr(s, s3_base(s, false), x, y));
        fg = s3_mem_fg(s, mem);
    }
    s3_put(s, x, y, fg, 0, mem);
}

static inline int sext14(uint16_t v)
{
    return (int16_t)(v << 2) >> 2;
}

static void s3_line(S3TrioState *s)
{
    S3GE *ge = &s->ge;
    int x = ge->cur_x & 0xfff, y = ge->cur_y & 0xfff;
    int len = ge->maj & 0xfff;
    int xs = (ge->cmd & CMD_XPOS) ? 1 : -1, ys = (ge->cmd & CMD_YPOS) ? 1 : -1;
    int err = sext14(ge->err), k1 = sext14(ge->desty), k2 = sext14(ge->destx);

    if (ge->cmd & CMD_RADIAL) {
        int dir = (ge->cmd >> 5) & 7;

        for (int i = 0; i <= len; i++) {
            s3_line_pixel(s, x, y, i == len);
            if (i < len) {
                s3_step(dir, &x, &y);
            }
        }
        goto done;
    }
    for (int i = 0; i <= len; i++) {
        s3_line_pixel(s, x, y, i == len);
        if (i == len) {
            break;
        }
        if (ge->cmd & CMD_YMAJ) {
            y += ys;
        } else {
            x += xs;
        }
        if (err >= 0) {
            if (ge->cmd & CMD_YMAJ) {
                x += xs;
            } else {
                y += ys;
            }
            err += k2;
        } else {
            err += k1;
        }
    }
    ge->err = err & 0x3fff;
done:
    ge->cur_x = x & 0xfff;
    ge->cur_y = y & 0xfff;
    s3_ge_done(s);
}

static void s3_short_stroke(S3TrioState *s, uint8_t v)
{
    S3GE *ge = &s->ge;
    int x = ge->cur_x & 0xfff, y = ge->cur_y & 0xfff;
    int len = v & 0xf, dir = (v >> 5) & 7;

    for (int i = 0; i <= len; i++) {
        if (v & 0x10) {
            s3_line_pixel(s, x, y, i == len);
        }
        if (i < len) {
            s3_step(dir, &x, &y);
        }
    }
    ge->cur_x = x & 0xfff;
    ge->cur_y = y & 0xfff;
    s3_ge_flush_dirty(s);
}

static void s3_cmd(S3TrioState *s, uint16_t cmd)
{
    S3GE *ge = &s->ge;

    trace_s3_cmd(cmd, ge->cur_x, ge->cur_y, ge->destx, ge->desty, ge->maj,
                 ge->mf[0], s3_ge_bpp(s), s3_ge_pitch(s));
    ge->cmd = cmd;
    ge->wait = false;
    switch (CMD_TYPE(cmd)) {
    case 1:
        s3_line(s);
        break;
    case 2:
        s3_rect_start(s);
        s3_ge_flush_dirty(s);
        break;
    case 6:
        s3_blit(s, false);
        break;
    case 7:
        s3_blit(s, true);
        break;
    default:
        break;
    }
}

static uint32_t s3_bswap(uint32_t data, int bytes)
{
    switch (bytes) {
    case 4:
        return bswap32(data);
    case 2:
        return bswap16(data);
    default:
        return data;
    }
}

/* one transfer unit of image data; extra data at the end of a line is lost */
static void s3_pix_unit(S3TrioState *s, uint32_t data, int bytes)
{
    S3GE *ge = &s->ge;
    unsigned bpp = s3_ge_bpp(s);

    if (s3_mono_data(s)) {
        if (ge->cmd & CMD_SWAP) {
            data = s3_bswap(data, bytes);
        }
        for (int i = bytes * 8 - 1; i >= 0 && ge->wait; i--) {
            s3_rect_pixel(s, (data >> i) & 1, 0);
            if (!s3_rect_next(s)) {
                continue;
            }
            if (CMD_BUS(ge->cmd) != 3) {
                break;
            }
            i &= ~7;
        }
        return;
    }
    if (bpp == 1 && !(ge->cmd & CMD_SWAP)) {
        /* high byte first */
        data = s3_bswap(data, bytes);
    }
    for (int i = 0; i + bpp <= bytes && ge->wait; i += bpp) {
        s3_rect_pixel(s, true, extract32(data, i * 8, bpp * 8));
        if (s3_rect_next(s)) {
            break;
        }
    }
}

static void s3_pix_trans(S3TrioState *s, uint32_t val, int bytes)
{
    S3GE *ge = &s->ge;
    int unit = 1 << CMD_BUS(ge->cmd);

    if (!ge->wait || CMD_TYPE(ge->cmd) != 2) {
        return;
    }
    if (unit > 4) {
        unit = 4;
    }
    /* 32 bpp color through 16-bit writes: gather a whole pixel */
    if (!s3_mono_data(s) && s3_ge_bpp(s) == 4 && bytes == 2) {
        ge->pend |= val << (ge->pend_bytes * 8);
        ge->pend_bytes += 2;
        if (ge->pend_bytes == 4) {
            s3_pix_unit(s, ge->pend, 4);
            ge->pend = 0;
            ge->pend_bytes = 0;
        }
        goto out;
    }
    while (bytes > 0 && ge->wait) {
        int n = MIN(unit, bytes);

        s3_pix_unit(s, val & (n == 4 ? UINT32_MAX : (1u << (n * 8)) - 1), n);
        val = n == 4 ? 0 : val >> (n * 8);
        bytes -= n;
    }
out:
    s3_ge_flush_dirty(s);
}

/* ---------------------------------------------------------- GE registers */

static uint32_t *s3_color_reg(S3GE *ge, uint32_t reg)
{
    switch (reg) {
    case GE_BKGD_COLOR:
        return &ge->bkgd_color;
    case GE_FRGD_COLOR:
        return &ge->frgd_color;
    case GE_WRT_MASK:
        return &ge->wrt_mask;
    case GE_RD_MASK:
        return &ge->rd_mask;
    case GE_COLOR_CMP:
        return &ge->color_cmp;
    default:
        return NULL;
    }
}

static void s3_ge_write16(S3TrioState *s, uint32_t reg, uint16_t val)
{
    S3GE *ge = &s->ge;
    uint32_t *c = s3_color_reg(ge, reg);

    if (c) {
        if (s3_ge_bpp(s) == 4 && !(ge->mf[0xe] & 0x200)) {
            if (ge->mf[0xe] & 0x10) {
                *c = (*c & 0xffff) | ((uint32_t)val << 16);
            } else {
                *c = (*c & 0xffff0000) | val;
            }
            ge->mf[0xe] ^= 0x10;
            return;
        }
        *c = val;
        return;
    }
    switch (reg) {
    case GE_SUBSYS:
        ge->subsys_cntl = val;
        ge->subsys_stat &= ~(val & 0xf);
        if ((val >> 14) == 2) {
            ge->wait = false;
        }
        break;
    case GE_ADVFUNC:
        ge->advfunc = val;
        break;
    case GE_CUR_Y:
        ge->cur_y = val;
        break;
    case GE_CUR_X:
        ge->cur_x = val;
        break;
    case GE_DESTY:
        ge->desty = val;
        break;
    case GE_DESTX:
        ge->destx = val;
        break;
    case GE_ERR:
        ge->err = val;
        break;
    case GE_MAJ:
        ge->maj = val;
        break;
    case GE_CMD:
        s3_cmd(s, val);
        break;
    case GE_SSV:
        if (ge->cmd & CMD_SWAP) {
            s3_short_stroke(s, val);
            s3_short_stroke(s, val >> 8);
        } else {
            s3_short_stroke(s, val >> 8);
            s3_short_stroke(s, val);
        }
        break;
    case GE_BKGD_MIX:
        ge->bkgd_mix = val;
        break;
    case GE_FRGD_MIX:
        ge->frgd_mix = val;
        break;
    case GE_MULTIFUNC:
        ge->mf[val >> 12] = val & 0xfff;
        break;
    case GE_PIX_TRANS:
    case GE_PIX_TRANS + 2:
        s3_pix_trans(s, val, 2);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "s3: write to GE reg 0x%x\n", reg);
    }
}

static uint16_t s3_ge_read16(S3TrioState *s, uint32_t reg)
{
    S3GE *ge = &s->ge;
    uint32_t *c = s3_color_reg(ge, reg);
    unsigned sel;

    if (c) {
        if (s3_ge_bpp(s) == 4 && (ge->mf[0xe] & 0x10)) {
            return *c >> 16;
        }
        return *c;
    }
    switch (reg) {
    case GE_SUBSYS:
        return ge->subsys_stat | 0x80;
    case GE_ADVFUNC:
        return ge->advfunc;
    case GE_CUR_Y:
        return ge->cur_y;
    case GE_CUR_X:
        return ge->cur_x;
    case GE_DESTY:
        return ge->desty;
    case GE_DESTX:
        return ge->destx;
    case GE_ERR:
        return ge->err;
    case GE_MAJ:
        return ge->maj;
    case GE_CMD:
        return ge->wait ? 0x0200 : 0x0400;
    case GE_BKGD_MIX:
        return ge->bkgd_mix;
    case GE_FRGD_MIX:
        return ge->frgd_mix;
    case GE_MULTIFUNC:
        sel = ge->mf[0xf] & 0xf;
        ge->mf[0xf] = (ge->mf[0xf] & ~0xf) | ((sel + 1) & 0xf);
        switch (sel) {
        case 0 ... 4:
            return ge->mf[sel] | (sel << 12);
        case 5:
            return ge->mf[0xa] | 0xa000;
        case 6:
            return ge->mf[0xe] | 0xe000;
        case 7:
            return ge->cmd & 0x1fff;
        case 8:
            return (ge->subsys_stat | 0x80) & 0x0fff;
        case 10:
            return ge->mf[0xd] | 0xd000;
        default:
            return 0;
        }
    default:
        return 0xffff;
    }
}

static void s3_ge_write(S3TrioState *s, uint32_t reg, uint32_t val,
                        unsigned size)
{
    uint32_t *c = s3_color_reg(&s->ge, reg & ~3);

    trace_s3_ge_write(reg, val, size);
    if (size == 4 && (reg & 3) == 0) {
        if (c) {
            *c = val;
            return;
        }
        if (reg == GE_PIX_TRANS) {
            s3_pix_trans(s, val, 4);
            return;
        }
        s3_ge_write16(s, reg, val);
        s3_ge_write16(s, reg + 2, val >> 16);
        return;
    }
    if (size == 2 && (reg & 3) == 2 && c) {
        *c = (*c & 0xffff) | (val << 16);
        return;
    }
    if (size == 1) {
        /* byte writes: merge into the 16-bit register */
        uint32_t r16 = reg & ~1;
        uint16_t cur = (r16 == GE_PIX_TRANS || r16 == GE_CMD) ? 0 :
                       s3_ge_read16(s, r16);

        if (r16 == GE_PIX_TRANS) {
            s3_pix_trans(s, val & 0xff, 1);
            return;
        }
        val = (reg & 1) ? (cur & 0xff) | (val << 8) : (cur & 0xff00) | val;
    }
    s3_ge_write16(s, reg & ~1, val);
}

static uint32_t s3_ge_read(S3TrioState *s, uint32_t reg, unsigned size)
{
    uint32_t v = s3_ge_read16(s, reg & ~1);

    if (size == 1) {
        v = (reg & 1) ? v >> 8 : v & 0xff;
    } else if (size == 4) {
        v |= (uint32_t)s3_ge_read16(s, reg + 2) << 16;
    }
    trace_s3_ge_read(reg, v, size);
    return v;
}

/* packed MMIO registers 8100-814A: pairs of 16-bit registers */
static void s3_packed_write(S3TrioState *s, uint32_t off, uint32_t val,
                            unsigned size)
{
    static const uint16_t lo[] = {
        [0x00] = GE_CUR_Y, [0x08] = GE_DESTY, [0x10] = GE_ERR,
        [0x18] = GE_CMD, [0x1c] = GE_SSV, [0x34] = GE_BKGD_MIX,
        [0x48] = GE_MULTIFUNC,
    };
    static const uint16_t hi[] = {
        [0x00] = GE_CUR_X, [0x08] = GE_DESTX, [0x34] = GE_FRGD_MIX,
        [0x48] = GE_MAJ,
    };
    S3GE *ge = &s->ge;
    unsigned o = off - 0x8100;

    switch (o) {
    case 0x20:
        ge->bkgd_color = val;
        return;
    case 0x24:
        ge->frgd_color = val;
        return;
    case 0x28:
        ge->wrt_mask = val;
        return;
    case 0x2c:
        ge->rd_mask = val;
        return;
    case 0x30:
        ge->color_cmp = val;
        return;
    case 0x38: /* scissors top, left */
        ge->mf[1] = val & 0xfff;
        ge->mf[2] = (val >> 16) & 0xfff;
        return;
    case 0x3c: /* scissors bottom, right */
        ge->mf[3] = val & 0xfff;
        ge->mf[4] = (val >> 16) & 0xfff;
        return;
    case 0x40: /* pix_cntl, mult_misc2 */
        ge->mf[0xa] = val & 0xfff;
        ge->mf[0xd] = (val >> 16) & 0xfff;
        return;
    case 0x44: /* mult_misc, read_sel */
        ge->mf[0xe] = val & 0xfff;
        ge->mf[0xf] = (val >> 16) & 0xfff;
        return;
    case 0x48: /* min axis, maj axis */
        ge->mf[0] = val & 0xfff;
        ge->maj = val >> 16;
        return;
    }
    if (o < ARRAY_SIZE(lo) && lo[o] && size == 4) {
        /* the command register is written last */
        if (o < ARRAY_SIZE(hi) && hi[o]) {
            s3_ge_write16(s, hi[o], val >> 16);
        }
        s3_ge_write16(s, lo[o], val);
        return;
    }
    if (o < ARRAY_SIZE(lo) && lo[o]) {
        s3_ge_write16(s, lo[o], val);
        return;
    }
    if (o >= 2 && o - 2 < ARRAY_SIZE(hi) && hi[o - 2]) {
        s3_ge_write16(s, hi[o - 2], val);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "s3: packed MMIO write 0x%x\n", off);
}

/* ---------------------------------------------------------- VGA registers */

static bool s3_cr_unlocked(VGACommonState *vga, uint8_t idx)
{
    if (idx < 0x30 || idx == 0x38 || idx == 0x39) {
        return true;
    }
    if (idx < 0x40) {
        return (vga->cr[0x38] & 0xcc) == 0x48;
    }
    return (vga->cr[0x39] & 0xe0) == 0xa0;
}

static uint8_t s3_cr_read(S3TrioState *s, uint8_t idx)
{
    VGACommonState *vga = &s->vga;
    PCIDevice *d = PCI_DEVICE(s);
    uint32_t bar = pci_get_long(d->config + PCI_BASE_ADDRESS_0);

    if (!s3_cr_unlocked(vga, idx)) {
        return 0;
    }
    switch (idx) {
    case 0x2d:
        return 0x88;
    case 0x2e:
        return 0x11;
    case 0x2f:
        return 0x40;
    case 0x30:
        return 0xe1;
    case 0x45:
        s->hwc_stack = 0;
        return vga->cr[idx];
    case 0x59:
        return bar >> 24;
    case 0x5a:
        return bar >> 16;
    case 0x66:
        return (vga->cr[idx] & ~1) | (s->ge.advfunc & 1);
    default:
        return vga->cr[idx];
    }
}

static void s3_cr_write(S3TrioState *s, uint8_t idx, uint8_t val)
{
    VGACommonState *vga = &s->vga;

    if (!s3_cr_unlocked(vga, idx)) {
        return;
    }
    switch (idx) {
    case 0x2d ... 0x30:
        return;
    case 0x36:
    case 0x37:
    case 0x68:
    case 0x6f:
        if (vga->cr[0x39] != 0xa5) {
            return;
        }
        if (idx == 0x36) {
            /* memory size straps stay at the installed size */
            val = (val & 0x1f) | (vga->cr[0x36] & 0xe0);
        }
        break;
    case 0x4a:
    case 0x4b: {
        uint32_t *c = idx == 0x4a ? &s->hwc_fg : &s->hwc_bg;
        int sh = (s->hwc_stack % 3) * 8;

        *c = (*c & ~(0xffu << sh)) | ((uint32_t)val << sh);
        s->hwc_stack++;
        break;
    }
    case 0x66:
        s->ge.advfunc = (s->ge.advfunc & ~1) | (val & 1);
        if (val & 2) {
            s->ge.wait = false;
        }
        break;
    }
    vga->cr[idx] = val;
    if (idx == 0x45) {
        vga->force_shadow = val & 1;
    }
}

static void s3_update_bank(S3TrioState *s)
{
    bool chain4 = s->vga.sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M;

    /* a bank is 64K of display memory, 16K addresses per plane */
    s->vga.bank_offset = s3_bank(s) * (chain4 ? 64 : 16) * KiB;
}

static uint8_t s3_vga_index(S3TrioState *s, uint32_t port)
{
    switch (port) {
    case 0x3c5:
        return s->sr_index;
    case 0x3b5:
    case 0x3d5:
        return s->vga.cr_index;
    case 0x3cf:
        return s->vga.gr_index;
    default:
        return 0;
    }
}

static uint32_t s3_vga_rd(S3TrioState *s, uint32_t port)
{
    VGACommonState *vga = &s->vga;

    if (vga_ioport_invalid(vga, port)) {
        return 0xff;
    }
    switch (port) {
    case 0x3c4:
        return s->sr_index;
    case 0x3c5:
        if (s->sr_index == 0x17) {
            /* clock synthesizer test counter: BIOSes wait for it to move */
            return vga->sr[0x17]++;
        }
        if (s->sr_index >= 8) {
            return vga->sr[s->sr_index];
        }
        break;
    case 0x3b5:
    case 0x3d5:
        if (vga->cr_index >= 0x2d) {
            return s3_cr_read(s, vga->cr_index);
        }
        break;
    }
    return vga_ioport_read(vga, port);
}

static uint64_t s3_vga_io_read(void *opaque, hwaddr addr, unsigned size)
{
    S3TrioState *s = opaque;
    uint32_t port = addr + 0x3b0;
    uint32_t val = s3_vga_rd(s, port);

    trace_s3_vga_read(port, s3_vga_index(s, port), val);
    return val;
}

static void s3_vga_io_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    S3TrioState *s = opaque;
    VGACommonState *vga = &s->vga;
    uint32_t port = addr + 0x3b0;

    trace_s3_vga_write(port, s3_vga_index(s, port), val);
    if (vga_ioport_invalid(vga, port)) {
        return;
    }
    switch (port) {
    case 0x3c4:
        s->sr_index = val;
        break;
    case 0x3c5:
        if (s->sr_index < 8) {
            break;
        }
        if (s->sr_index == 8 || (vga->sr[8] & 0x0f) == 0x06) {
            vga->sr[s->sr_index] = val;
        }
        return;
    case 0x3b5:
    case 0x3d5:
        if (vga->cr_index < 0x2d) {
            break;
        }
        s3_cr_write(s, vga->cr_index, val);
        return;
    }
    vga_ioport_write(vga, port, val);
}

static const MemoryRegionOps s3_vga_io_ops = {
    .read = s3_vga_io_read,
    .write = s3_vga_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* ------------------------------------------------------------------- MMIO */

static uint64_t s3_mmio_reg_read(S3TrioState *s, uint32_t off, unsigned size)
{
    PCIDevice *d = PCI_DEVICE(s);

    if (off >= 0x8000 && off < 0x8100) {
        return pci_default_read_config(d, off & 0xff, size);
    }
    if (off >= 0x83b0 && off < 0x83e0) {
        uint32_t v = 0;

        for (unsigned i = 0; i < size; i++) {
            v |= s3_vga_io_read(s, off - 0x83b0 + i, 1) << (i * 8);
        }
        return v;
    }
    if (off == 0x8504) {
        return s3_ge_read(s, GE_SUBSYS, size);
    }
    if (off == 0x850c) {
        return s3_ge_read(s, GE_ADVFUNC, size);
    }
    if (off >= 0x82e8 && off < 0xf000) {
        return s3_ge_read(s, off, size);
    }
    return 0;
}

static void s3_mmio_reg_write(S3TrioState *s, uint32_t off, uint32_t val,
                              unsigned size)
{
    if (off < 0x8000) {
        s3_pix_trans(s, val, size);
        return;
    }
    if (off >= 0x8100 && off < 0x8150) {
        s3_packed_write(s, off, val, size);
        return;
    }
    if (off >= 0x83b0 && off < 0x83e0) {
        for (unsigned i = 0; i < size; i++) {
            s3_vga_io_write(s, off - 0x83b0 + i, (val >> (i * 8)) & 0xff, 1);
        }
        return;
    }
    if (off == 0x8504) {
        s3_ge_write(s, GE_SUBSYS, val, size);
        return;
    }
    if (off == 0x850c) {
        s3_ge_write(s, GE_ADVFUNC, val, size);
        return;
    }
    if (off >= 0x82e8 && off < 0xf000) {
        s3_ge_write(s, off, val, size);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "s3: MMIO write 0x%x\n", off);
}

static uint64_t s3_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    return s3_mmio_reg_read(opaque, addr & 0xffff, size);
}

static void s3_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    s3_mmio_reg_write(opaque, addr & 0xffff, val, size);
}

static const MemoryRegionOps s3_mmio_ops = {
    .read = s3_mmio_read,
    .write = s3_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps s3_mmio_be_ops = {
    .read = s3_mmio_read,
    .write = s3_mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* big endian linear aperture, byte lanes swapped per CR53 */
static inline hwaddr s3_be_lane(S3TrioState *s, hwaddr addr)
{
    switch ((s->vga.cr[0x53] >> 1) & 3) {
    case 1:
        return addr ^ 1;
    case 2:
        return addr ^ 3;
    default:
        return addr;
    }
}

static uint64_t s3_lfb_be_read(void *opaque, hwaddr addr, unsigned size)
{
    S3TrioState *s = opaque;
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr a = s3_be_lane(s, addr + i) & s3_vram_mask(s);

        v = (v << 8) | s->vga.vram_ptr[a];
    }
    return v;
}

static void s3_lfb_be_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    S3TrioState *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        hwaddr a = s3_be_lane(s, addr + i) & s3_vram_mask(s);

        s->vga.vram_ptr[a] = val >> ((size - 1 - i) * 8);
        memory_region_set_dirty(&s->vga.vram, a, 1);
    }
}

static const MemoryRegionOps s3_lfb_be_ops = {
    .read = s3_lfb_be_read,
    .write = s3_lfb_be_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* ------------------------------------------------- legacy 0xa0000 window */

static bool s3_old_mmio(S3TrioState *s, hwaddr addr, bool *regs)
{
    uint8_t cr53 = s->vga.cr[0x53];

    if (!(cr53 & 0x10)) {
        return false;
    }
    if (cr53 & 0x20) {
        *regs = addr >= 0x18000;
        return *regs;
    }
    *regs = addr >= 0x8000;
    return addr < 0x10000;
}

static uint64_t s3_lowmem_read(void *opaque, hwaddr addr, unsigned size)
{
    S3TrioState *s = opaque;
    VGACommonState *vga = &s->vga;
    bool regs;
    uint64_t v = 0;

    if (s3_old_mmio(s, addr, &regs)) {
        return regs ? s3_mmio_reg_read(s, addr & 0xffff, size) : 0;
    }
    if (vga->cr[0x31] & 0x08) {
        uint32_t a = s3_bank(s) * 64 * KiB + (addr & 0xffff);

        if (addr >= 0x10000) {
            return 0;
        }
        for (unsigned i = 0; i < size; i++) {
            v |= (uint64_t)vga->vram_ptr[(a + i) & s3_vram_mask(s)] << (i * 8);
        }
        return v;
    }
    s3_update_bank(s);
    for (unsigned i = 0; i < size; i++) {
        v |= (uint64_t)vga_mem_readb(vga, addr + i) << (i * 8);
    }
    return v;
}

static void s3_lowmem_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    S3TrioState *s = opaque;
    VGACommonState *vga = &s->vga;
    bool regs;

    if (s3_old_mmio(s, addr, &regs)) {
        if (regs) {
            s3_mmio_reg_write(s, addr & 0xffff, val, size);
        } else {
            s3_pix_trans(s, val, size);
        }
        return;
    }
    if (vga->cr[0x31] & 0x08) {
        uint32_t a = s3_bank(s) * 64 * KiB + (addr & 0xffff);

        if (addr >= 0x10000) {
            return;
        }
        for (unsigned i = 0; i < size; i++) {
            vga->vram_ptr[(a + i) & s3_vram_mask(s)] = val >> (i * 8);
        }
        memory_region_set_dirty(&vga->vram, a & s3_vram_mask(s), size);
        return;
    }
    s3_update_bank(s);
    for (unsigned i = 0; i < size; i++) {
        vga_mem_writeb(vga, addr + i, (val >> (i * 8)) & 0xff);
    }
}

static const MemoryRegionOps s3_lowmem_ops = {
    .read = s3_lowmem_read,
    .write = s3_lowmem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* -------------------------------------------------- enhanced I/O ports */

static uint64_t s3_ge_io_read(void *opaque, hwaddr addr, unsigned size)
{
    S3GEPort *p = opaque;

    return s3_ge_read(p->s, p->port + addr, size);
}

static void s3_ge_io_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    S3GEPort *p = opaque;

    s3_ge_write(p->s, p->port + addr, val, size);
}

static const MemoryRegionOps s3_ge_io_ops = {
    .read = s3_ge_io_read,
    .write = s3_ge_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* --------------------------------------------------------------- display */

/* packed pixel display: CR3A or the 8514 enhanced mode in ADVFUNC_CNTL */
static bool s3_enhanced(VGACommonState *vga)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);

    return (vga->cr[0x3a] & 0x10) || (s->ge.advfunc & 1);
}

static int s3_get_bpp(VGACommonState *vga)
{
    if (!s3_enhanced(vga)) {
        return 0;
    }
    switch (vga->cr[0x67] >> 4) {
    case 0x3:
        return 15;
    case 0x5:
        return 16;
    case 0x7:
        return 24;
    case 0xd:
        return 32;
    default:
        return 8;
    }
}

static void s3_get_params(VGACommonState *vga, VGADisplayParams *p)
{
    uint8_t *cr = vga->cr;
    uint32_t off = cr[0x13];

    if (cr[0x51] & 0x30) {
        off |= (cr[0x51] & 0x30) << 4;
    } else if (cr[0x43] & 0x04) {
        off |= 0x100;
    }
    p->line_offset = off << 3;
    p->start_addr = cr[0x0d] | (cr[0x0c] << 8);
    if (cr[0x69] & 0x0f) {
        p->start_addr |= (cr[0x69] & 0x0f) << 16;
    } else {
        p->start_addr |= ((cr[0x31] & 0x30) << 12) | ((cr[0x51] & 0x03) << 18);
    }
    p->line_compare = cr[0x18] | ((cr[0x07] & 0x10) << 4) |
                      ((cr[0x09] & 0x40) << 3) | ((cr[0x5e] & 0x40) << 4);
    p->hpel = vga->ar[VGA_ATC_PEL];
    p->hpel_split = vga->ar[VGA_ATC_MODE] & 0x20;
}

static void s3_get_resolution(VGACommonState *vga, int *pw, int *ph)
{
    uint8_t *cr = vga->cr;
    int w = (cr[0x01] + 1 + ((cr[0x5d] & 0x02) << 7)) * 8;
    int h = cr[0x12] | ((cr[0x07] & 0x02) << 7) | ((cr[0x07] & 0x40) << 3) |
            ((cr[0x5e] & 0x02) << 9);

    switch (s3_get_bpp(vga)) {
    case 15:
    case 16:
        w >>= 1;
        break;
    case 24:
        w /= 3;
        break;
    }
    if (cr[0x43] & 0x80) {
        w <<= 1;
    }
    *pw = w;
    *ph = h + 1;
}

static uint32_t s3_hwc_color(S3TrioState *s, uint32_t c)
{
    VGACommonState *vga = &s->vga;
    uint8_t *p;

    switch (s3_get_bpp(vga)) {
    case 15:
        return rgb_to_pixel32(((c >> 10) & 0x1f) << 3, ((c >> 5) & 0x1f) << 3,
                              (c & 0x1f) << 3);
    case 16:
        return rgb_to_pixel32(((c >> 11) & 0x1f) << 3, ((c >> 5) & 0x3f) << 2,
                              (c & 0x1f) << 3);
    case 24:
    case 32:
        return rgb_to_pixel32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    default:
        p = vga->palette + (c & 0xff) * 3;
        return rgb_to_pixel32(c6_to_8(p[0]), c6_to_8(p[1]), c6_to_8(p[2]));
    }
}

static bool s3_hwc_on(VGACommonState *vga)
{
    return (vga->cr[0x45] & 1) && s3_enhanced(vga);
}

static void s3_cursor_invalidate(VGACommonState *vga)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);
    int y = ((vga->cr[0x48] & 7) << 8) | vga->cr[0x49];
    int h = s3_hwc_on(vga) ? 64 - (vga->cr[0x4f] & 0x3f) : 0;

    if (y == s->hwc_last_y && h == s->hwc_last_h) {
        return;
    }
    vga_invalidate_scanlines(vga, s->hwc_last_y, s->hwc_last_y + s->hwc_last_h);
    vga_invalidate_scanlines(vga, y, y + h);
    s->hwc_last_y = y;
    s->hwc_last_h = h;
}

static void s3_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);
    uint8_t *cr = vga->cr;
    int x0 = ((cr[0x46] & 7) << 8) | cr[0x47];
    int y0 = ((cr[0x48] & 7) << 8) | cr[0x49];
    int xoff = cr[0x4e] & 0x3f, yoff = cr[0x4f] & 0x3f;
    int line = scr_y - y0 + yoff;
    uint32_t base, fg, bg, *px = (uint32_t *)d;
    bool x11 = cr[0x55] & 0x10;
    int width = vga->last_width;

    if (!s3_hwc_on(vga) || scr_y < y0 || line >= 64) {
        return;
    }
    base = ((((cr[0x4c] & 0x0f) << 8) | cr[0x4d]) * KiB + line * 16) &
           s3_vram_mask(s);
    fg = s3_hwc_color(s, s->hwc_fg);
    bg = s3_hwc_color(s, s->hwc_bg);
    for (int i = xoff; i < 64; i++) {
        int x = x0 + i - xoff;
        const uint8_t *g = vga->vram_ptr + base + (i / 16) * 4;
        int bit = 15 - (i & 15);
        bool a = ((g[0] << 8 | g[1]) >> bit) & 1;
        bool b = ((g[2] << 8 | g[3]) >> bit) & 1;

        if (x >= width) {
            break;
        }
        if (x11) {
            if (a) {
                px[x] = b ? fg : bg;
            }
            continue;
        }
        if (!a) {
            px[x] = b ? fg : bg;
        } else if (b) {
            px[x] ^= 0xffffff;
        }
    }
}

/* ------------------------------------------------------------------- reset */

static void s3_reset(void *opaque)
{
    S3TrioState *s = opaque;
    VGACommonState *vga = &s->vga;
    static const uint8_t memsize[] = { [1] = 0xc0, [2] = 0x80, [4] = 0x00 };

    vga_common_reset(vga);
    memset(&s->ge, 0, sizeof(s->ge));
    s->ge.dirty_lo = UINT32_MAX;
    s->ge.wrt_mask = s->ge.rd_mask = UINT32_MAX;
    s->sr_index = 0;
    s->hwc_stack = 0;
    vga->cr[0x36] = memsize[vga->vram_size_mb] | 0x0e;
    vga->cr[0x37] = 0x09;
    vga->cr[0x40] = 0x30;
    vga->cr[0x53] = 0x08;
    vga->cr[0x58] = 0x10;
    vga->cr[0x68] = 0x80;
    vga->cr[0x6f] = 0x1f;
    vga->sr[0x10] = 0x42;
    vga->sr[0x11] = 0x22;
    vga->sr[0x12] = 0x3c;
    vga->sr[0x13] = 0x2c;
    vga->force_shadow = false;
}

/* --------------------------------------------------------------- PCI glue */

static void s3_realize(PCIDevice *d, Error **errp)
{
    S3TrioState *s = S3_TRIO(d);
    VGACommonState *vga = &s->vga;
    Object *obj = OBJECT(d);

    if (vga->vram_size_mb != 1 && vga->vram_size_mb != 2 &&
        vga->vram_size_mb != 4) {
        error_setg(errp, "s3-trio64: vgamem_mb must be 1, 2 or 4");
        return;
    }
    if (!vga_common_init(vga, obj, errp)) {
        return;
    }
    vga->get_bpp = s3_get_bpp;
    vga->native_mode = s3_enhanced;
    vga->get_params = s3_get_params;
    vga->get_resolution = s3_get_resolution;
    vga->cursor_invalidate = s3_cursor_invalidate;
    vga->cursor_draw_line = s3_cursor_draw_line;
    vga->big_endian_fb = false;
    vga->con = qemu_graphic_console_create(DEVICE(d), 0, vga->hw_ops, vga);

    memory_region_init_io(&s->vga_io, obj, &s3_vga_io_ops, s, "s3-vga-io",
                          0x30);
    memory_region_init_alias(&s->vga_io_mono, obj, "s3-vga-3b0", &s->vga_io,
                             0, 0xc);
    memory_region_init_alias(&s->vga_io_color, obj, "s3-vga-3c0", &s->vga_io,
                             0x10, 0x20);
    memory_region_add_subregion(pci_address_space_io(d), 0x3b0,
                                &s->vga_io_mono);
    memory_region_add_subregion(pci_address_space_io(d), 0x3c0,
                                &s->vga_io_color);
    memory_region_init_io(&s->lowmem, obj, &s3_lowmem_ops, s, "s3-lowmem",
                          0x20000);
    memory_region_add_subregion_overlap(pci_address_space(d), 0xa0000,
                                        &s->lowmem, 2);
    for (int i = 0; i < S3_GE_PORTS; i++) {
        s->ge_port[i].s = s;
        s->ge_port[i].port = s3_ge_ports[i];
        memory_region_init_io(&s->ge_io[i], obj, &s3_ge_io_ops,
                              &s->ge_port[i], "s3-ge", 4);
        memory_region_add_subregion(pci_address_space_io(d), s3_ge_ports[i],
                                    &s->ge_io[i]);
    }

    memory_region_init(&s->bar, obj, "s3-bar0", S3_BAR_SIZE);
    memory_region_init_alias(&s->lfb, obj, "s3-lfb", &vga->vram, 0,
                             vga->vram_size);
    memory_region_init_io(&s->mmio, obj, &s3_mmio_ops, s, "s3-mmio",
                          S3_APERTURE);
    memory_region_init_io(&s->lfb_be, obj, &s3_lfb_be_ops, s, "s3-lfb-be",
                          vga->vram_size);
    memory_region_init_io(&s->mmio_be, obj, &s3_mmio_be_ops, s, "s3-mmio-be",
                          S3_APERTURE);
    memory_region_add_subregion(&s->bar, 0, &s->lfb);
    memory_region_add_subregion(&s->bar, S3_APERTURE, &s->mmio);
    memory_region_add_subregion(&s->bar, 2 * S3_APERTURE, &s->lfb_be);
    memory_region_add_subregion(&s->bar, 3 * S3_APERTURE, &s->mmio_be);
    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    d->config[PCI_INTERRUPT_PIN] = 1;
    qemu_register_reset(s3_reset, s);
    s3_reset(s);
}

static const VMStateDescription vmstate_s3_trio = {
    .name = "s3-trio64",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, S3TrioState),
        VMSTATE_STRUCT(vga, S3TrioState, 0, vmstate_vga_common,
                       VGACommonState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s3_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", S3TrioState, vga.vram_size_mb, 2),
};

static void s3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = s3_realize;
    k->vendor_id = 0x5333;
    k->device_id = 0x8811;
    k->revision = 0x40;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "S3 Trio64V+";
    dc->vmsd = &vmstate_s3_trio;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, s3_properties);
}

static const TypeInfo s3_trio_info = {
    .name = TYPE_S3_TRIO,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(S3TrioState),
    .class_init = s3_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void s3_trio_register_types(void)
{
    type_register_static(&s3_trio_info);
}

type_init(s3_trio_register_types)
