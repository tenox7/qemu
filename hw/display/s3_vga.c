/*
 * QEMU PCI S3 Trio (VGA compatible)
 *
 * Copyright (c) 2017 Hervé Poussineau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* S3 Trio is a very complex graphic card. Only some parts of them have
 * been implemented.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "vga_int.h"
#include "hw/display/vga.h"
#include "hw/display/vga_regs.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "qom/object.h"
#include "trace.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/reset.h"

#define TYPE_S3_TRIO "s3-trio"

enum {
    REG_DISP_STAT      = 0x00,
    REG_H_DISP         = 0x01,
    REG_H_SYNC_START   = 0x02,
    REG_H_SYNC_WID     = 0x03,
    REG_V_TOTAL        = 0x04,
    REG_V_DISP         = 0x05,
    REG_V_SYNC_STRT    = 0x06,
    REG_V_SYNC_WID     = 0x07,
    REG_DISP_CNTL      = 0x08,
    REG_H_TOTAL        = 0x09,
    REG_SUBSYS_STAT    = 0x10, /* read-only */
    REG_SUBSYS_CNTL    = 0x10, /* write-only */
    REG_ROM_PAGE_SEL   = 0x11,
    REG_ADVFUNC_CNTL   = 0x12,
    REG_CUR_Y          = 0x20,
    REG_CUR_X          = 0x21,
    REG_DESTY_AXSTP    = 0x22,
    REG_DESTX_DIASTP   = 0x23,
    REG_ERR_TERM       = 0x24,
    REG_MAJ_AXIS_PCNT  = 0x25,
    REG_GP_STAT        = 0x26, /* read-only */
    REG_CMD            = 0x26, /* write-only */
    REG_SHORT_STROKE   = 0x27,
    REG_BKGD_COLOR     = 0x28,
    REG_FRGD_COLOR     = 0x29,
    REG_WRT_MASK       = 0x2A,
    REG_RD_MASK        = 0x2B,
    REG_COLOR_CMP      = 0x2C,
    REG_BKGD_MIX       = 0x2D,
    REG_FRGD_MIX       = 0x2E,
    REG_MULTIFUNC_CNTL = 0x2F,
    REG_PIX_TRANS      = 0x38,
};

enum {
    DISP_STAT_SENSE = 0x0001,
};

enum {
    GP_STAT_BUSY = 0x0200,
    GP_STAT_FIFO_EMPTY = 0x0400,
};

enum {
    CMD_WRTDATA  = 0x0001,
    CMD_PLANAR   = 0x0002,
    CMD_LASTPIX  = 0x0004,
    CMD_LINETYPE = 0x0008,
    CMD_DRAW     = 0x0010,
    CMD_INC_X    = 0x0020,
    CMD_YMAJAXIS = 0x0040,
    CMD_INC_Y    = 0x0080,
    CMD_PCDATA   = 0x0100,
    CMD_16BIT    = 0x0200,
    CMD_BYTSEQ   = 0x1000,
};

#define CMD_CMD_MASK 0xE000
enum {
    CMD_CMD_NOP    = 0x0000,
    CMD_CMD_LINE   = 0x2000,
    CMD_CMD_RECT   = 0x4000,
    CMD_CMD_RECTV1 = 0x6000,
    CMD_CMD_RECTV2 = 0x8000,
    CMD_CMD_LINEAF = 0xA000,
    CMD_CMD_BITBLT = 0xC000,
    CMD_CMD_PATBLT = 0xE000,
};

#define BKGD_MIX_BSS_MASK 0x0060
enum {
    BKGD_MIX_BSS_BKGD = 0x0000,
    BKGD_MIX_BSS_FRGD = 0x0020,
    BKGD_MIX_BSS_PIX  = 0x0040,
    BKGD_MIX_BSS_BMP  = 0x0060,
};

#define FRGD_MIX_FSS_MASK 0x0060
enum {
    FRGD_MIX_FSS_BKGD = 0x0000,
    FRGD_MIX_FSS_FRGD = 0x0020,
    FRGD_MIX_FSS_PIX  = 0x0040,
    FRGD_MIX_FSS_BMP  = 0x0060,
};

#define PIX_CNTL_MIXSEL_MASK 0x00C0
enum {
    PIX_CNTL_MIXSEL_FOREMIX = 0x0000,
    PIX_CNTL_MIXSEL_PATTERN = 0x0040,
    PIX_CNTL_MIXSEL_VAR     = 0x0080,
    PIX_CNTL_MIXSEL_TRANS   = 0x00C0,
};

typedef enum {
    S3_MODEL_TRIO,
    S3_MODEL_VISION864,
} S3Model;

typedef struct S3TrioState {
    PCIDevice dev;
    VGACommonState vga;
    uint8_t model;
    uint16_t maj_axis, min_axis;
    PortioList portio;

    bitbang_i2c_interface bbi2c;
    I2CDDCState i2cddc;
    uint8_t ddc_reg;

    MemoryRegion linear_fb;
    bool lfb_mapped;

    uint32_t dclk;
    uint32_t mclk;

    uint16_t disp_stat; /* 02e8 */
    uint16_t h_disp; /* 06e8 */
    uint16_t h_sync_strt; /* 0ae8 */
    uint16_t h_sync_wid; /* 0ee8 */
    uint16_t v_total; /* 12e8 */
    uint16_t v_disp; /* 16e8 */
    uint16_t v_sync_strt; /* 1ae8 */
    uint16_t v_sync_wid; /* 1ee8 */
    uint16_t disp_cntl; /* 22e8 */
    uint16_t h_total; /* 26e8 */
    uint16_t subsys_cntl; /* 42e8 (W) */
    uint16_t subsys_stat; /* 42e8 (R) */
    uint16_t rom_page_sel; /* 46e8 */
    uint16_t advfunc_cntl; /* 4ae8 */
    uint16_t cur_y; /* 82e8 */
    uint16_t cur_x; /* 86e8 */
    uint16_t desty_axstep; /* 8ae8 */
    uint16_t destx_diastp; /* 8ee8 */
    uint16_t err_term; /* 92e8 */
    uint16_t maj_axis_pcnt; /* 96e8 */
    uint16_t gp_stat; /* 9ae8 (R) */
    uint16_t cmd; /* 9ae8 (W) */
    uint16_t short_stroke; /* 9ee8 */
    uint16_t bkgd_color; /* a2e8 */
    uint16_t frgd_color; /* a6e8 */
    uint16_t wrt_mask; /* aae8 */
    uint16_t rd_mask; /* aee8 */
    uint16_t color_cmp; /* b2e8 */
    uint16_t bkgd_mix; /* b6e8 */
    uint16_t frgd_mix; /* bae8 */
    uint16_t mfc[16]; /* bee8 */
    uint16_t pix_trans; /* e2e8 */

    uint8_t cursor_fg[3];
    uint8_t cursor_bg[3];
    uint8_t cursor_fg_idx;
    uint8_t cursor_bg_idx;
    uint32_t last_cursor_x;
    uint32_t last_cursor_y;
    bool last_cursor_on;
    uint8_t unlock_pll;
    uint8_t unlock_compatibility_registers;
    uint8_t unlock_control_registers_1;
    uint8_t unlock_control_registers_2;
} S3TrioState;

OBJECT_DECLARE_SIMPLE_TYPE(S3TrioState, S3_TRIO)


#define min_axis_pcnt mfc[0]
#define scissors_t    mfc[1]
#define scissors_l    mfc[2]
#define scissors_b    mfc[3]
#define scissors_r    mfc[4]
#define mem_cntl      mfc[5]
#define pattern_l     mfc[8]
#define pattern_h     mfc[9]
#define pix_cntl      mfc[10]
#define color_compare mfc[14]

static void s3_trio_update_lfb(S3TrioState *s)
{
    static const uint32_t law_size[4] = { 64 * KiB, 1 * MiB, 2 * MiB, 4 * MiB };
    MemoryRegion *pcimem = pci_address_space(&s->dev);
    uint32_t base = (s->vga.cr[0x59] << 24) | (s->vga.cr[0x5a] << 16);
    uint64_t size = law_size[s->vga.cr[0x58] & 0x03];
    bool enable = !!(s->vga.cr[0x58] & 0x10);

    if (size > memory_region_size(&s->vga.vram)) {
        size = memory_region_size(&s->vga.vram);
    }

    memory_region_transaction_begin();
    if (s->lfb_mapped) {
        memory_region_del_subregion(pcimem, &s->linear_fb);
        s->lfb_mapped = false;
    }
    if (enable && base) {
        memory_region_set_size(&s->linear_fb, size);
        memory_region_add_subregion(pcimem, base, &s->linear_fb);
        s->lfb_mapped = true;
    }
    memory_region_transaction_commit();
}

/*
 * With CR31 bit 0 set, the A0000 window shows the 64 KB bank in CR6A.
 * CR35 bits 3-0 and CR51 bits 3-2 write the low and high bank bits.
 */
static void s3_update_bank(S3TrioState *s)
{
    VGACommonState *v = &s->vga;
    uint32_t off = 0;

    if (v->cr[0x31] & 0x01) {
        off = v->cr[0x6a] << (v->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M ? 16 : 14);
    }
    vga_set_bank_offset(v, off & (v->vram_size - 1));
}

static inline int address_to_reg(uint32_t addr)
{
    assert((addr & 0x3ff) == 0x2e8);
    return addr >> 10;
}

static inline void do_cmd_done(S3TrioState *s)
{
    s->gp_stat &= ~GP_STAT_BUSY;
}

/*
 * The engine reads as idle while it waits for CPU pixel data: the OS/2
 * 864 GRADD starts fully clipped transfers, sends nothing, then waits
 * for idle.
 */
static uint16_t s3_gp_stat(S3TrioState *s)
{
    if (s->cmd & CMD_PCDATA) {
        return s->gp_stat & ~GP_STAT_BUSY;
    }
    return s->gp_stat;
}

static uint32_t s3_pitch(S3TrioState *s)
{
    uint32_t offset = s->vga.cr[0x13] | ((s->vga.cr[0x51] & 0x30) << 4);

    return offset ? offset * 8 : 1024;
}

static bool s3_in_scissors(S3TrioState *s, int x, int y)
{
    return x >= (s->scissors_l & 0xfff) && x <= (s->scissors_r & 0xfff) &&
           y >= (s->scissors_t & 0xfff) && y <= (s->scissors_b & 0xfff);
}

static uint8_t s3_mix(uint8_t op, uint8_t src, uint8_t dst)
{
    switch (op & 0xf) {
    case 0x0: return ~dst;
    case 0x1: return 0;
    case 0x2: return 0xff;
    case 0x3: return dst;
    case 0x4: return ~src;
    case 0x5: return src ^ dst;
    case 0x6: return ~(src ^ dst);
    case 0x7: return src;
    case 0x8: return ~src | ~dst;
    case 0x9: return dst | ~src;
    case 0xa: return ~dst | src;
    case 0xb: return src | dst;
    case 0xc: return src & dst;
    case 0xd: return src & ~dst;
    case 0xe: return ~src & dst;
    default:  return ~src & ~dst;
    }
}

static uint8_t s3_mix_source(S3TrioState *s, uint16_t mix, uint8_t cpu, uint8_t bmp)
{
    switch (mix & FRGD_MIX_FSS_MASK) {
    case FRGD_MIX_FSS_BKGD:
        return s->bkgd_color;
    case FRGD_MIX_FSS_FRGD:
        return s->frgd_color;
    case FRGD_MIX_FSS_PIX:
        return cpu;
    default:
        return bmp;
    }
}

static void s3_put_pixel(S3TrioState *s, int x, int y, uint16_t mix, uint8_t cpu, uint8_t bmp)
{
    uint32_t offset = y * s3_pitch(s) + x;
    uint8_t src, dst, res, mask;
    uint8_t *p8;

    if (x < 0 || y < 0 || !s3_in_scissors(s, x, y) ||
        offset >= s->vga.vram_size) {
        return;
    }
    if ((mix & 0xf) == 0x3) {
        return;
    }
    src = s3_mix_source(s, mix, cpu, bmp);
    if (s->color_compare & 0x100) {
        if ((s->color_compare & 0x80) == 0x80 && s->color_cmp != src) {
            return;
        } else if ((s->color_compare & 0x80) == 0x00 && s->color_cmp == src) {
            return;
        }
    }
    p8 = s->vga.vram_ptr + offset;
    dst = *p8;
    mask = s->wrt_mask;
    res = (s3_mix(mix, src, dst) & mask) | (dst & ~mask);
    if (res != dst) {
        *p8 = res;
        memory_region_set_dirty(&s->vga.vram, offset, 1);
    }
}

static bool move_to_next_pixel(S3TrioState *s)
{
    int dx = s->cmd & CMD_INC_X ? 1 : -1;
    int dy = s->cmd & CMD_INC_Y ? 1 : -1;

    switch (s->cmd & CMD_CMD_MASK) {
    case CMD_CMD_RECT:
        if (s->maj_axis < s->maj_axis_pcnt) {
            s->maj_axis++;
            s->cur_x += dx;
            return false;
        }
        s->maj_axis = 0;
        s->cur_x -= s->maj_axis_pcnt * dx;
        s->cur_y += dy;
        if (s->min_axis++ == s->min_axis_pcnt) {
            do_cmd_done(s);
        }
        return true;
    case CMD_CMD_LINE:
        if (s->cmd & CMD_LINETYPE) {
            static const int xstep[] = { 1,  1,  0, -1, -1, -1, 0, 1 };
            static const int ystep[] = { 0, -1, -1, -1,  0,  1, 1, 1 };
            int dir = (s->cmd >> 5) & 7;

            s->cur_x += xstep[dir];
            s->cur_y += ystep[dir];
        } else {
            int16_t err = s->err_term;
            bool diag = err >= 0;

            if (s->cmd & CMD_YMAJAXIS) {
                s->cur_y += dy;
                if (diag) {
                    s->cur_x += dx;
                }
            } else {
                s->cur_x += dx;
                if (diag) {
                    s->cur_y += dy;
                }
            }
            s->err_term = err + (diag ? (int16_t)s->destx_diastp
                                      : (int16_t)s->desty_axstep);
        }
        if (s->maj_axis++ == s->maj_axis_pcnt) {
            do_cmd_done(s);
        }
        return false;
    default:
        do_cmd_done(s);
        return true;
    }
}

static bool s3_engine_pixel(S3TrioState *s, bool fg, uint8_t cpu)
{
    bool last = (s->cmd & CMD_CMD_MASK) == CMD_CMD_LINE &&
                s->maj_axis == s->maj_axis_pcnt && (s->cmd & CMD_LASTPIX);

    if (!last) {
        s3_put_pixel(s, s->cur_x, s->cur_y, fg ? s->frgd_mix : s->bkgd_mix,
                     cpu, 0);
    }
    return move_to_next_pixel(s);
}

static void s3_pix_trans_data(S3TrioState *s, const uint8_t *bytes, int n)
{
    bool mono = (s->cmd & CMD_PLANAR) ||
                (s->pix_cntl & PIX_CNTL_MIXSEL_MASK) == PIX_CNTL_MIXSEL_VAR;
    int i, bit;

    if (!(s->gp_stat & GP_STAT_BUSY) || !(s->cmd & CMD_PCDATA)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s3_trio: pixel data written with no transfer in progress\n");
        return;
    }
    for (i = 0; i < n; i++) {
        if (mono) {
            for (bit = 7; bit >= 0; bit--) {
                if (s3_engine_pixel(s, bytes[i] & (1 << bit), bytes[i]) ||
                    !(s->gp_stat & GP_STAT_BUSY)) {
                    return;
                }
            }
        } else {
            if (s3_engine_pixel(s, true, bytes[i]) ||
                !(s->gp_stat & GP_STAT_BUSY)) {
                return;
            }
        }
    }
}

static void s3_pix_trans_write(S3TrioState *s, uint32_t val, unsigned size)
{
    uint8_t b[4] = { val, val >> 8, val >> 16, val >> 24 };
    uint8_t swapped[4] = { b[1], b[0], b[3], b[2] };

    s->pix_trans = val;
    s3_pix_trans_data(s, (s->cmd & CMD_BYTSEQ) || size == 1 ? b : swapped,
                      size);
}

static void s3_run_solid(S3TrioState *s)
{
    if (s->cmd & CMD_PCDATA) {
        return;
    }
    if ((s->pix_cntl & PIX_CNTL_MIXSEL_MASK) == PIX_CNTL_MIXSEL_PATTERN) {
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented mixsel PATTERN\n");
    }
    while (s->gp_stat & GP_STAT_BUSY) {
        s3_engine_pixel(s, true, 0);
    }
}

/*
 * Both rectangles start at the given corners and run in the direction of
 * INC_X and INC_Y, so overlapping copies to the right or down start at
 * the bottom right.
 */
static void s3_do_cmd_blt(S3TrioState *s, bool pattern)
{
    int width = s->maj_axis_pcnt + 1;
    int height = s->min_axis_pcnt + 1;
    int sx = s->cur_x, sy = s->cur_y;
    int dx = s->destx_diastp, dy = s->desty_axstep;
    int xstep = s->cmd & CMD_INC_X ? 1 : -1;
    int ystep = s->cmd & CMD_INC_Y ? 1 : -1;
    uint32_t pitch = s3_pitch(s);
    int mixsel = s->pix_cntl & PIX_CNTL_MIXSEL_MASK;
    int x, y;

    for (y = 0; y < height; y++) {
        int oy = y * ystep;

        for (x = 0; x < width; x++) {
            int ox = x * xstep;
            int64_t soff;
            uint8_t src;
            bool fg = true;

            if (pattern) {
                soff = (sy + ((dy + oy) & 7)) * pitch + sx + ((dx + ox) & 7);
            } else {
                soff = (int64_t)(sy + oy) * pitch + sx + ox;
            }
            if (soff < 0 || soff >= s->vga.vram_size) {
                continue;
            }
            src = s->vga.vram_ptr[soff];
            if (mixsel == PIX_CNTL_MIXSEL_TRANS) {
                fg = src & s->rd_mask;
            } else if (mixsel == PIX_CNTL_MIXSEL_PATTERN) {
                fg = src & s->rd_mask;
            }
            s3_put_pixel(s, dx + ox, dy + oy, fg ? s->frgd_mix : s->bkgd_mix,
                         0, src);
        }
    }
}

static void do_cmd_init(S3TrioState *s)
{
    s->gp_stat |= GP_STAT_BUSY;
    s->maj_axis = 0;
    s->min_axis = 0;

}

static void do_cmd(S3TrioState *s)
{
    trace_s3_vga_cmd(s->cmd);

    do_cmd_init(s);

    if ((s->cmd & CMD_WRTDATA) == 0) {
        qemu_log_mask(LOG_UNIMP,
                      "s3_trio: CMD_WRTDATA=0 not implemented (%04x)\n", s->cmd);
    }

    switch (s->cmd & CMD_CMD_MASK) {
    case CMD_CMD_NOP:
        do_cmd_done(s);
        break;
    case CMD_CMD_LINE:
        if ((s->cmd & CMD_LINETYPE) == 0) {
            trace_s3_vga_cmd_line_bresenham(s->cur_x, s->cur_y,
                                            s->cmd & CMD_INC_X ? 1 : -1,
                                            s->cmd & CMD_INC_Y ? 1 : -1,
                                            s->maj_axis_pcnt,
                                            s->cmd & CMD_YMAJAXIS ? 'Y' : 'X');
        } else {
            trace_s3_vga_cmd_line_vector(s->cur_x, s->cur_y, (s->cmd >> 5) & 7,
                                         s->maj_axis_pcnt);
        }
        s3_run_solid(s);
        break;
    case CMD_CMD_RECT:
        trace_s3_vga_cmd_rect(s->cur_x, s->cur_y, s->cmd & CMD_INC_X ? 1 : -1,
                              s->cmd & CMD_INC_Y ? 1 : -1, s->maj_axis_pcnt,
                              s->min_axis_pcnt);
        s3_run_solid(s);
        break;
    case CMD_CMD_RECTV1:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_RECTV1 not implemented (%04x)\n",
                      s->cmd);
        do_cmd_done(s);
        break;
    case CMD_CMD_RECTV2:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_RECTV2 not implemented (%04x)\n",
                      s->cmd);
        do_cmd_done(s);
        break;
    case CMD_CMD_LINEAF:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_LINEAF not implemented (%04x)\n",
                      s->cmd);
        do_cmd_done(s);
        break;
    case CMD_CMD_BITBLT:
        trace_s3_vga_cmd_bitblt(s->cur_x, s->cur_y, s->destx_diastp, s->desty_axstep,
                                s->maj_axis_pcnt, s->min_axis_pcnt);
        s3_do_cmd_blt(s, false);
        do_cmd_done(s);
        break;
    case CMD_CMD_PATBLT:
        trace_s3_vga_cmd_bitblt(s->cur_x, s->cur_y, s->destx_diastp, s->desty_axstep,
                                s->maj_axis_pcnt, s->min_axis_pcnt);
        s3_do_cmd_blt(s, true);
        do_cmd_done(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: illegal command %04x\n",
                      s->cmd);
        do_cmd_done(s);
        break;
    }
}

static bool s3_cursor_on(S3TrioState *s)
{
    return s->vga.cr[0x45] & 0x01;
}

static void s3_cursor_invalidate(VGACommonState *v)
{
    S3TrioState *s = container_of(v, S3TrioState, vga);
    bool on = s3_cursor_on(s);

    if (on != s->last_cursor_on || v->hw_cursor_x != s->last_cursor_x ||
        v->hw_cursor_y != s->last_cursor_y) {
        if (s->last_cursor_on) {
            vga_invalidate_scanlines(v, s->last_cursor_y, s->last_cursor_y + 64);
        }
        if (on) {
            vga_invalidate_scanlines(v, v->hw_cursor_y, v->hw_cursor_y + 64);
        }
        s->last_cursor_on = on;
        s->last_cursor_x = v->hw_cursor_x;
        s->last_cursor_y = v->hw_cursor_y;
    }
}

static uint32_t s3_cursor_color(S3TrioState *s, const uint8_t *c)
{
    const uint8_t *pal;

    if (s->vga.get_bpp(&s->vga) == 8) {
        pal = s->vga.palette + c[0] * 3;
        return rgb_to_pixel32(c6_to_8(pal[0]), c6_to_8(pal[1]), c6_to_8(pal[2]));
    }
    return rgb_to_pixel32(c[0], c[1], c[2]);
}

static void s3_cursor_draw_line(VGACommonState *v, uint8_t *d1, int scr_y)
{
    S3TrioState *s = container_of(v, S3TrioState, vga);
    uint32_t *d = (uint32_t *)d1;
    uint32_t start, fg, bg;
    int hotx = s->vga.cr[0x4e] & 0x3f;
    int hoty = s->vga.cr[0x4f] & 0x3f;
    int row, col, x;
    const uint8_t *src;

    if (!s3_cursor_on(s) || scr_y < v->hw_cursor_y) {
        return;
    }
    row = scr_y - v->hw_cursor_y + hoty;
    if (row >= 64) {
        return;
    }
    start = (((s->vga.cr[0x4c] & 0x0f) << 8) | s->vga.cr[0x4d]) * 1024;
    if (start + 1024 > v->vram_size) {
        return;
    }
    src = v->vram_ptr + start + row * 16;
    fg = s3_cursor_color(s, s->cursor_fg);
    bg = s3_cursor_color(s, s->cursor_bg);
    for (col = hotx; col < 64; col++) {
        int g = col / 16, bit = 15 - (col % 16);
        uint16_t and = (src[g * 4] << 8) | src[g * 4 + 1];
        uint16_t xor = (src[g * 4 + 2] << 8) | src[g * 4 + 3];
        bool a = and & (1 << bit), x_ = xor & (1 << bit);

        x = v->hw_cursor_x + col - hotx;
        if (x >= v->last_scr_width) {
            break;
        }
        if (a && x_) {
            d[x] ^= 0xffffff;
        } else if (!a) {
            d[x] = x_ ? fg : bg;
        }
    }
}

static uint32_t s3_trio_enable_readb(void *opaque, uint32_t addr)
{
    uint32_t val;
    val = 0;
    trace_s3_vga_enable_readb(addr, val);
    return val;
}

static void s3_trio_enable_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    trace_s3_vga_enable_writeb(addr, val);
}

/*
 * The RAMDAC has one address register for reads and writes, read back
 * at 3C8. Setting a read address prefetches that entry and increments
 * it, and so does every third data access. OS/2 VVIDEO reads 3C9 until
 * 3C8 changes to find the DAC state.
 */
static uint32_t s3_dac_read(S3TrioState *s, uint32_t addr)
{
    uint32_t val = vga_ioport_read(&s->vga, addr);

    if (addr == VGA_PEL_D && !s->vga.dac_sub_index) {
        s->vga.dac_write_index++;
    }
    return val;
}

static void s3_dac_write(S3TrioState *s, uint32_t addr, uint32_t val)
{
    vga_ioport_write(&s->vga, addr, val);
    if (addr == VGA_PEL_IR) {
        s->vga.dac_write_index = val + 1;
    }
}

static uint32_t s3_trio_dac_ioport_readb(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;

    val = s3_dac_read(s, addr - 0x2ea + VGA_PEL_MSK);
    trace_s3_vga_dac_readb(addr, val);
    return val;
}

static void s3_trio_dac_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;
    trace_s3_vga_dac_writeb(addr, val);
    s3_dac_write(s, addr - 0x2ea + VGA_PEL_MSK, val);
}

static uint16_t* s3_trio_get_register(S3TrioState *s, uint32_t addr, int is_write, uint32_t* val_if_write)
{
    uint16_t *p = NULL;

    switch (addr) {
    case REG_DISP_STAT:
        p = is_write ? &s->h_total : &s->disp_stat;
        break;
    case REG_H_DISP:
        p = is_write ? &s->h_disp : NULL;
        break;
    case REG_H_SYNC_START:
        p = is_write ? &s->h_sync_strt : NULL;
        break;
    case REG_H_SYNC_WID:
        p = is_write ? &s->h_sync_wid : NULL;
        break;
    case REG_V_TOTAL:
        p = is_write ? &s->v_total : NULL;
        break;
    case REG_V_DISP:
        p = is_write ? &s->v_disp : NULL;
        break;
    case REG_V_SYNC_STRT:
        p = is_write ? &s->v_sync_strt : NULL;
        break;
    case REG_V_SYNC_WID:
        p = is_write ? &s->v_sync_wid : NULL;
        break;
    case REG_DISP_CNTL:
        p = is_write ? &s->disp_cntl : NULL;
        break;
    case REG_H_TOTAL:
        p = is_write ? NULL: &s->h_total;
        break;
    case REG_SUBSYS_STAT: /* or REG_SUBSYS_CNTL */
        p = is_write ? &s->subsys_cntl : &s->subsys_stat;
        break;
    case REG_ROM_PAGE_SEL:
        p = is_write ? &s->rom_page_sel : NULL;
        break;
    case REG_ADVFUNC_CNTL:
        p = is_write ? &s->advfunc_cntl : NULL;
        break;
    case REG_CUR_Y:
        p = &s->cur_y;
        break;
    case REG_CUR_X:
        p = &s->cur_x;
        break;
    case REG_DESTY_AXSTP:
        p = is_write ? &s->desty_axstep : NULL;
        break;
    case REG_DESTX_DIASTP:
        p = is_write ? &s->destx_diastp : NULL;
        break;
    case REG_ERR_TERM:
        p = &s->err_term;
        break;
    case REG_MAJ_AXIS_PCNT:
        if (is_write) {
            p = &s->maj_axis_pcnt;
            *val_if_write &= 0x0fff;
        }
        break;
    case REG_GP_STAT: /* or REG_CMD */
        p = is_write ? &s->cmd : &s->gp_stat;
        break;
    case REG_SHORT_STROKE:
        p = is_write ? &s->short_stroke : NULL;
        break;
    case REG_BKGD_COLOR:
        p = is_write ? &s->bkgd_color : NULL;
        break;
    case REG_FRGD_COLOR:
        p = is_write ? &s->frgd_color : NULL;
        break;
    case REG_WRT_MASK:
        p = is_write ? &s->wrt_mask : NULL;
        break;
    case REG_RD_MASK:
        p = is_write ? &s->rd_mask : NULL;
        break;
    case REG_COLOR_CMP:
        p = is_write ? &s->color_cmp : NULL;
        break;
    case REG_BKGD_MIX:
        p = is_write ? &s->bkgd_mix : NULL;
        break;
    case REG_FRGD_MIX:
        p = is_write ? &s->frgd_mix : NULL;
        break;
    case REG_MULTIFUNC_CNTL:
        if (is_write) {
            p = &s->mfc[(*val_if_write >> 12) & 0xf];
            *val_if_write &= 0x0fff;
        } else {
            p = NULL;
        }
        break;
    case REG_PIX_TRANS:
        p = &s->pix_trans;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: invalid register 0x%04x\n",
                      addr);
        break;
    }

    return p;
}

static uint32_t s3_trio_ioport_readb(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;
    uint16_t *p;

    if (addr & 0x2) {
        p = NULL;
    } else {
        p = s3_trio_get_register(s, address_to_reg(addr & ~0x3), 0, NULL);
    }

    if (p) {
        val = ((p == &s->gp_stat ? s3_gp_stat(s) : *p) >> ((~addr & 1) * 8)) & 0xff;
    } else {
        val = 0;
    }

    trace_s3_vga_io_readb(addr, val);
    return val;
}

/*
 * BEE8 reads return the register picked by READ_SEL (index F) with its
 * index in bits 15-12 and step READ_SEL; OS/2 VVIDEO reads until index E.
 */
static uint16_t s3_read_reg_data(S3TrioState *s)
{
    static const uint8_t index[] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0xa, 0xe };
    unsigned sel = s->mfc[0xf] & 0x7;

    s->mfc[0xf] = (s->mfc[0xf] & ~0x7) | ((sel + 1) & 0x7);
    if (sel >= ARRAY_SIZE(index)) {
        return 0xffff;
    }
    return (index[sel] << 12) | (s->mfc[index[sel]] & 0xfff);
}

static uint32_t s3_trio_ioport_readw(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;
    uint16_t *p;

    if (addr == 0xbee8) {
        val = s3_read_reg_data(s);
        trace_s3_vga_io_readw(addr, val);
        return val;
    }
    if (addr & 0x2) {
        p = NULL;
    } else {
        p = s3_trio_get_register(s, address_to_reg(addr), 0, NULL);
    }

    if (p == &s->gp_stat) {
        val = s3_gp_stat(s);
    } else if (p) {
        val = *p;
    } else {
        val = 0;
    }

    trace_s3_vga_io_readw(addr, val);
    return val;
}

static void s3_trio_post_write(S3TrioState* s, uint32_t addr)
{
    switch (address_to_reg(addr)) {
    case REG_H_DISP:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented write to H_DISP\n");
        break;
    case REG_V_DISP:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented write to V_DISP\n");
        break;
    case REG_SUBSYS_CNTL:
        s->subsys_cntl &= ~(1 << 12); /* clear CHPTST */
        break;
    case REG_CMD:
        do_cmd(s);
        break;
    default:
        break;
    }
}

static void s3_trio_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;
    uint16_t *p;
    uint8_t *c;

    trace_s3_vga_io_writeb(addr, val);
    if (address_to_reg(addr & ~0x3) == REG_PIX_TRANS) {
        s3_pix_trans_write(s, val, 1);
        return;
    }
    if (addr & 0x2) {
        return;
    }
    p = s3_trio_get_register(s, address_to_reg(addr & ~0x1), 1, &val);

    if (p) {
        c = (uint8_t*)p;
        c[~addr & 1] = val;
    }

    s3_trio_post_write(s, addr & ~0x1);
}

static void s3_trio_ioport_writew(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;
    uint16_t *p;

    trace_s3_vga_io_writew(addr, val);
    if (address_to_reg(addr & ~0x3) == REG_PIX_TRANS) {
        s3_pix_trans_write(s, val, 2);
        return;
    }
    if (addr & 0x2) {
        return;
    }
    p = s3_trio_get_register(s, address_to_reg(addr), 1, &val);

    if (p) {
        *p = val & 0xffff;
    }

    s3_trio_post_write(s, addr & ~0x1);
}

static void s3_trio_ioport_writel(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;

    trace_s3_vga_io_writel(addr, val);
    if (address_to_reg(addr) == REG_PIX_TRANS) {
        s3_pix_trans_write(s, val, 4);
        return;
    }

    s3_trio_ioport_writew(s, addr, val & 0xffff);
}

static uint32_t s3_trio_ioport_readl(void *opaque, uint32_t addr)
{
    return s3_trio_ioport_readw(opaque, addr);
}

static uint32_t s3_trio_alias_mono(uint32_t addr)
{
    switch (addr) {
    case VGA_CRT_IM:
        return VGA_CRT_IC;
    case VGA_CRT_DM:
        return VGA_CRT_DC;
    case VGA_IS1_RM:
        return VGA_IS1_RC;
    default:
        return addr;
    }
}

static uint32_t s3_trio_vga_ioport_read(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;

    addr = s3_trio_alias_mono(addr);

    switch (addr) {
    case VGA_CRT_DM:
    case VGA_CRT_DC:
        switch (s->vga.cr_index) {
        case 0x2d:
            val = 0x88;
            break;
        case 0x2e:
            val = s->model == S3_MODEL_VISION864 ? 0xc1 : 0x10;
            break;
        case 0x30:
            val = s->model == S3_MODEL_VISION864 ? 0xc1 : 0xe0;
            break;
        case 0x36:
        {
            static const uint8_t smem[] = { 7, 6, 4, 2, 0, 0, 5, 5, 3 };
            if (s->vga.vram_size_mb < sizeof(smem)) {
                val = smem[s->vga.vram_size_mb];
            } else {
                val = smem[sizeof(smem) - 1];
            }
            val = val << 5;
            break;
        }
        case 0x55:
            val = s->ddc_reg;
            break;
        case 0x45:
            s->cursor_fg_idx = 0;
            s->cursor_bg_idx = 0;
            val = vga_ioport_read(&s->vga, addr);
            break;
        default:
            val = vga_ioport_read(&s->vga, addr);
            break;
        }
        break;
    case VGA_SEQ_D:
        switch (s->vga.sr_index) {
        case 0x17: /* CLKSYN */
        {
            uint32_t *clk;
            if (!(s->vga.sr[0x14] & 0x01)) {
                s->dclk++;
            }
            if (!(s->vga.sr[0x14] & 0x03)) {
                s->mclk++;
            }
            if (s->vga.sr[0x14] & 0x04) {
                clk = &s->mclk;
            } else {
                clk = &s->dclk;
            }
            val = (*clk) & 0xff;
            break;
        }
        default:
            val = vga_ioport_read(&s->vga, addr);
            break;
        }
        break;
    case VGA_PEL_D:
        val = s3_dac_read(s, addr);
        break;
    default:
        val = vga_ioport_read(&s->vga, addr);
        break;
    }

    trace_s3_vga_io_readb(addr, val);
    return val;
}

static void s3_trio_vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;

    trace_s3_vga_io_writeb(addr, val);
    addr = s3_trio_alias_mono(addr);

    switch (addr) {
    case VGA_CRT_DM:
    case VGA_CRT_DC:
        if (s->vga.cr_index >= 0x40 && s->vga.cr_index <= 0x4f &&
            !s->unlock_control_registers_2) {
            break;
        }
        switch (s->vga.cr_index) {
        case 0x08:
            s->unlock_pll = (val == 0x06);
            break;
        case 0x10: /* memory pll data */
        case 0x11: /* memory pll data */
        case 0x12: /* video pll data */
        case 0x13: /* video pll data */
        case 0x15:
        case 0x18:
            if (s->unlock_pll) {
                qemu_log_mask(LOG_UNIMP,
                              "s3_trio: unimplemented PLL change\n");
            } else {
                vga_ioport_write(&s->vga, addr, val);
            }
            break;
        case 0x33:
            s->unlock_compatibility_registers = ((val & ~0xad) == 0);
            break;
        case 0x38:
            s->unlock_control_registers_1 = (val == 0x48);
            break;
        case 0x39:
            s->unlock_control_registers_2 = ((val & 0xf0) == 0xa0);
            break;
        case 0x45: /* cursor enable */
        case 0x4c:
        case 0x4d:
        case 0x4e:
        case 0x4f:
            vga_ioport_write(&s->vga, addr, val);
            break;
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
            vga_ioport_write(&s->vga, addr, val);
            s->vga.hw_cursor_x = ((s->vga.cr[0x46] & 0x07) << 8) | s->vga.cr[0x47];
            s->vga.hw_cursor_y = ((s->vga.cr[0x48] & 0x07) << 8) | s->vga.cr[0x49];
            break;
        case 0x4a:
            s->cursor_fg[s->cursor_fg_idx] = val;
            s->cursor_fg_idx = (s->cursor_fg_idx + 1) % 3;
            break;
        case 0x4b:
            s->cursor_bg[s->cursor_bg_idx] = val;
            s->cursor_bg_idx = (s->cursor_bg_idx + 1) % 3;
            break;
        case 0x58:
        case 0x59:
        case 0x5a:
            vga_ioport_write(&s->vga, addr, val);
            s3_trio_update_lfb(s);
            break;
        case 0x3a: /* non-VGA mode */
            vga_ioport_write(&s->vga, addr, val);
            if (val & 0x10) {
                s->vga.ar_index |= 0x20;
            }
            break;
        case 0x31:
        case 0x6a:
            vga_ioport_write(&s->vga, addr, val);
            s3_update_bank(s);
            break;
        case 0x35:
            vga_ioport_write(&s->vga, addr, val);
            s->vga.cr[0x6a] = (s->vga.cr[0x6a] & ~0x0f) | (val & 0x0f);
            s3_update_bank(s);
            break;
        case 0x51:
            vga_ioport_write(&s->vga, addr, val);
            s->vga.cr[0x6a] = (s->vga.cr[0x6a] & ~0x30) | ((val & 0x0c) << 2);
            s3_update_bank(s);
            break;
        case 0x55:
        {
            bool scl = val & 0x01;
            bool sda = val & 0x02;

            scl = bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SCL, scl);
            sda = bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SDA, sda);
            s->ddc_reg = (val & 0x03) | (scl ? 0x04 : 0) | (sda ? 0x08 : 0);
            break;
        }
        default:
            vga_ioport_write(&s->vga, addr, val);
            break;
        }
        break;
    case VGA_SEQ_I:
        s->vga.sr_index = val;
        break;
    case VGA_SEQ_D:
        switch (s->vga.sr_index) {
        case 0x00 ... 0x07:
            vga_ioport_write(&s->vga, addr, val);
            s3_update_bank(s);
            break;
        case 0x08:
            s->vga.sr[s->vga.sr_index] = val;
            break;
        case 0x09 ... 0x1c:
            if ((s->vga.sr[0x08] & 0x0f) == 0x06) {
                s->vga.sr[s->vga.sr_index] = val;
            }
            break;
        default:
            break;
        }
        break;
    case VGA_PEL_IR:
        s3_dac_write(s, addr, val);
        break;
    default:
        vga_ioport_write(&s->vga, addr, val);
        break;
    }
}

static const MemoryRegionPortio s3_trio_portio_list[] = {
    { 0x0102, 1, 1, .read = s3_trio_enable_readb, .write = s3_trio_enable_writeb, },
    { 0x02e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x02e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x02ea, 4, 1, .read = s3_trio_dac_ioport_readb, .write = s3_trio_dac_ioport_writeb, },
    { 0x03b4,  2, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03ba,  1, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03c0, 16, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03d4,  2, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03da,  1, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x06e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x06e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x0ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x0ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x0ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x0ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x16e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x1ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x1ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x1ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x1ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x22e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x22e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x26e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x26e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x2ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x2ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x2ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x2ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x32e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x32e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x36e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x36e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x3ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x3ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x3ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x3ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x42e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x42e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x46e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x46e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x4ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x4ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x82e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x82e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x86e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x86e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x8ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x8ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x8ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x8ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x92e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x92e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x96e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x96e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x9ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x9ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x9ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x9ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xa2e8, 1, 4, .read = s3_trio_ioport_readl, .write = s3_trio_ioport_writel, },
    { 0xa2e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xa2e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xa6e8, 1, 4, .read = s3_trio_ioport_readl, .write = s3_trio_ioport_writel, },
    { 0xa6e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xa6e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xaae8, 1, 4, .read = s3_trio_ioport_readl, .write = s3_trio_ioport_writel, },
    { 0xaae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xaae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xaee8, 1, 4, .read = s3_trio_ioport_readl, .write = s3_trio_ioport_writel, },
    { 0xaee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xaee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xb2e8, 1, 4, .read = s3_trio_ioport_readl, .write = s3_trio_ioport_writel, },
    { 0xb2e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xb2e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xb6e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xb6e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xbae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xbae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xbee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xbee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xe2e8, 1, 4, .write = s3_trio_ioport_writel, },
    { 0xe2e8, 2, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xe2e8, 4, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    PORTIO_END_OF_LIST()
};

static bool s3_trio_native_mode(VGACommonState *s)
{
    return s->cr[0x3a] & 0x10;
}

static int s3_trio_get_bpp(VGACommonState *s)
{
    if (!s3_trio_native_mode(s)) {
        return 8;
    }
    switch ((s->cr[0x50] >> 4) & 3) {
    case 1:
        return ((s->cr[0x67] >> 4) & 0xf) == 5 ? 16 : 15;
    case 3:
        return 32;
    default:
        return 8;
    }
}

static void s3_trio_get_resolution(VGACommonState *s, int *pwidth, int *pheight)
{
    int width, height;

    width = s->cr[VGA_CRTC_H_DISP] | ((s->cr[0x5d] & 0x02) << 7);
    width = (width + 1) * 8;
    height = s->cr[VGA_CRTC_V_DISP_END] |
        ((s->cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
        ((s->cr[VGA_CRTC_OVERFLOW] & 0x40) << 3) |
        ((s->cr[0x5e] & 0x02) << 9);
    height = height + 1;
    if (s->cr[0x42] & 0x20) {
        height *= 2;
    }
    *pwidth = width;
    *pheight = height;
}

static VMStateDescription vmstate_s3_trio = {
    .name = TYPE_S3_TRIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, S3TrioState),
        VMSTATE_STRUCT(vga, S3TrioState, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_UINT16(maj_axis, S3TrioState),
        VMSTATE_UINT16(min_axis, S3TrioState),
        VMSTATE_UINT16(disp_stat, S3TrioState),
        VMSTATE_UINT16(h_disp, S3TrioState),
        VMSTATE_UINT16(h_sync_strt, S3TrioState),
        VMSTATE_UINT16(h_sync_wid, S3TrioState),
        VMSTATE_UINT16(v_total, S3TrioState),
        VMSTATE_UINT16(v_disp, S3TrioState),
        VMSTATE_UINT16(v_sync_strt, S3TrioState),
        VMSTATE_UINT16(v_sync_wid, S3TrioState),
        VMSTATE_UINT16(disp_cntl, S3TrioState),
        VMSTATE_UINT16(h_total, S3TrioState),
        VMSTATE_UINT16(subsys_cntl, S3TrioState),
        VMSTATE_UINT16(subsys_stat, S3TrioState),
        VMSTATE_UINT16(rom_page_sel, S3TrioState),
        VMSTATE_UINT16(advfunc_cntl, S3TrioState),
        VMSTATE_UINT16(cur_y, S3TrioState),
        VMSTATE_UINT16(cur_x, S3TrioState),
        VMSTATE_UINT16(desty_axstep, S3TrioState),
        VMSTATE_UINT16(destx_diastp, S3TrioState),
        VMSTATE_UINT16(err_term, S3TrioState),
        VMSTATE_UINT16(maj_axis_pcnt, S3TrioState),
        VMSTATE_UINT16(gp_stat, S3TrioState),
        VMSTATE_UINT16(cmd, S3TrioState),
        VMSTATE_UINT16(short_stroke, S3TrioState),
        VMSTATE_UINT16(bkgd_color, S3TrioState),
        VMSTATE_UINT16(frgd_color, S3TrioState),
        VMSTATE_UINT16(wrt_mask, S3TrioState),
        VMSTATE_UINT16(rd_mask, S3TrioState),
        VMSTATE_UINT16(color_cmp, S3TrioState),
        VMSTATE_UINT16(bkgd_mix, S3TrioState),
        VMSTATE_UINT16(frgd_mix, S3TrioState),
        VMSTATE_UINT16_ARRAY(mfc, S3TrioState, 16),
        VMSTATE_UINT16(pix_trans, S3TrioState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s3_trio_properties[] = {
    DEFINE_PROP_UINT32("vram_size_mb", S3TrioState, vga.vram_size_mb, 8),
    /* 0 = Trio (default), 1 = Vision864 */
    DEFINE_PROP_UINT8("model", S3TrioState, model, S3_MODEL_TRIO),
};

static void s3_trio_reset(DeviceState *d)
{
    S3TrioState *s = S3_TRIO(d);

    vga_common_reset(&s->vga);

    /* no video BIOS on non-x86, probably. */
    s->vga.msr |= VGA_MIS_COLOR;

    static const uint8_t ar_reset[VGA_ATT_C] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        [VGA_ATC_MODE]         = 0x0c,
        [VGA_ATC_OVERSCAN]     = 0x00,
        [VGA_ATC_PLANE_ENABLE] = 0x0f,
        [VGA_ATC_PEL]          = 0x08,
        [VGA_ATC_COLOR_PAGE]   = 0x00,
    };

    memcpy(s->vga.ar, ar_reset, sizeof(ar_reset));
    s->vga.ar_index |= 0x20;

    s->gp_stat = GP_STAT_FIFO_EMPTY;

    memset(s->cursor_fg, 0x00, sizeof(s->cursor_fg));
    memset(s->cursor_bg, 0x00, sizeof(s->cursor_bg));
    s->cursor_fg_idx = 0;
    s->cursor_bg_idx = 0;

    s->disp_stat |= DISP_STAT_SENSE;
}

static void s3_trio_realize(PCIDevice *dev, Error **errp)

{
    S3TrioState *s = S3_TRIO(dev);
    Object *o = OBJECT(dev);
    const MemoryRegionPortio *vga_ports, *vbe_ports;
    MemoryRegion* vga_io_memory;
    I2CBus *i2cbus;

    /* setup VGA */
    if (!vga_common_init(&s->vga, OBJECT(dev), errp)) {
        return;
    }
    s->vga.legacy_address_space = pci_address_space(dev);
    vga_io_memory = vga_init_io(&s->vga, o, &vga_ports, &vbe_ports);
    memory_region_add_subregion_overlap(s->vga.legacy_address_space,
                                        0x000a0000, vga_io_memory, 1);
    memory_region_set_coalescing(vga_io_memory);
    memory_region_set_coalescing(&s->vga.vram);

    s->vga.con = qemu_graphic_console_create(DEVICE(s), 0, s->vga.hw_ops, &s->vga);

    s->vga.get_bpp = s3_trio_get_bpp;
    s->vga.native_mode = s3_trio_native_mode;
    s->vga.get_resolution = s3_trio_get_resolution;
    s->vga.cursor_invalidate = s3_cursor_invalidate;
    s->vga.cursor_draw_line = s3_cursor_draw_line;

    isa_register_portio_list(NULL, &s->portio, 0, s3_trio_portio_list, s, "s3_trio");

    /* setup PCI */
    if (s->model == S3_MODEL_VISION864) {
        pci_config_set_device_id(dev->config, PCI_DEVICE_ID_S3_VISION864);
    } else if (s->model != S3_MODEL_TRIO) {
        error_setg(errp, "model must be 0 (Trio) or 1 (Vision864)");
        return;
    }
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vga.vram);

    memory_region_init_alias(&s->linear_fb, o, "s3-trio.lfb", &s->vga.vram, 0,
                             memory_region_size(&s->vga.vram));
    s->lfb_mapped = false;

    i2cbus = i2c_init_bus(DEVICE(s), "s3-trio.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(i2cbus), &error_abort);
    s->ddc_reg = 0x0f;
}

static void s3_trio_init(Object *obj)
{
    S3TrioState *s = S3_TRIO(obj);

    object_initialize_child(obj, "edid", &s->i2cddc, TYPE_I2CDDC);
}

static void s3_trio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = s3_trio_realize;
    //k->romfile = "vgabios-s3.bin";
    k->vendor_id = PCI_VENDOR_ID_S3;
    k->device_id = PCI_DEVICE_ID_S3_TRIO;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    device_class_set_legacy_reset(dc, s3_trio_reset);
    dc->desc = "S3 Trio 32 VGA";
    dc->vmsd  = &vmstate_s3_trio;
    device_class_set_props(dc, s3_trio_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s3_trio_info = {
    .name          = "s3-trio",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(S3TrioState),
    .instance_init = s3_trio_init,
    .class_init    = s3_trio_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void s3_register_types(void)
{
    type_register_static(&s3_trio_info);
}

type_init(s3_register_types)
