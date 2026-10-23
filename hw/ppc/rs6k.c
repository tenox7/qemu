/*
 * IBM RS/6000 7006 (POWERstation 41T/41W): PowerPC 601, Micro Channel
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "qemu/datadir.h"
#include "hw/ppc/ppc.h"
#include "cpu.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/ppc/rs6k_iocc.h"
#include "hw/ppc/rs6k_sio.h"
#include "hw/ppc/rs6k_scsi.h"
#include "hw/ppc/rs6k_ent.h"
#include "hw/ppc/rs6k_gxt.h"
#include "hw/ppc/rs6k_panel.h"
#include "hw/core/irq.h"
#include "hw/isa/isa.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "net/net.h"
#include "trace.h"

#define ROS_ADDR    0xfff00000
#define ROS_SIZE    (512 * KiB)

typedef struct {
    MachineState parent_obj;
    char *nvram;
    char *key;
} Rs6kMachineState;

#define RS6K_MACHINE(obj) \
    OBJECT_CHECK(Rs6kMachineState, (obj), MACHINE_TYPE_NAME("7006"))

#define ICP_MAX_SRC 32

typedef struct IcpSrc {
    uint32_t num;
    uint8_t prio;
    bool level;
    bool asserted;
    bool in_service;
    void (*accept)(void *opaque, uint32_t num);
    void (*eoi)(void *opaque, uint32_t num);
    void *opaque;
} IcpSrc;

typedef struct {
    PowerPCCPU *cpu;
    MemoryRegion ros;
    uint8_t *ros_image;
    MemoryRegion sysregs;
    MemoryRegion nvram;
    DeviceState *panel;
    MemoryRegion buc;
    uint32_t buc_sel;
    MemoryRegion vpd;
    uint8_t vpd_data[64];
    size_t vpd_len;
    GHashTable *regs;
    MemoryRegion icp;
    qemu_irq cpu_irq;
    uint8_t cppr, mfrr, xisr_prio;
    uint32_t xisr, dsier;
    IcpSrc src[ICP_MAX_SRC];
    int nsrc;
    MemoryRegion sec;
    IcpSrc *sec_src;
    uint32_t sec_status, sec_addr, sec_mask;
    uint32_t ecc_status, ecc_addr;
    Rs6kIOCCState *iocc;
    IcpSrc *iocc_src[IOCC_LEVELS];
    DeviceState *gxt;
    IcpSrc *gxt_src;
    Notifier machine_done;
    ram_addr_t ram_size;
    uint8_t *ram_ptr;
} Rs6kState;

/* Caller of the ROS I/O accessor routines, for tracing */
static target_ulong cpu_lr(void)
{
    return current_cpu ? POWERPC_CPU(current_cpu)->env.lr : 0;
}

static uint64_t unassigned_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "rs6k: unassigned read%u 0x%08" HWADDR_PRIx
                  " lr=%08x\n", size * 8, addr, (uint32_t)cpu_lr());
    return ~0ULL;
}

static void unassigned_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "rs6k: unassigned write%u 0x%08" HWADDR_PRIx
                  " = 0x%" PRIx64 " lr=%08x\n", size * 8, addr, val,
                  (uint32_t)cpu_lr());
}

static const MemoryRegionOps unassigned_ops = {
    .read = unassigned_read,
    .write = unassigned_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

/*
 * T=1 segment accesses arrive here with the effective address as offset;
 * the live segment register selects the bus unit and its address space.
 */
#define SR_BUID(sr)     (((sr) >> 20) & 0x1ff)
#define SR_IOCC_SELECT  0x80
#define BUID_IOCC0      0x20

static AddressSpace *ds_target(Rs6kState *s, hwaddr ea, hwaddr *addr)
{
    uint32_t sr = current_cpu ? POWERPC_CPU(current_cpu)->env.sr[ea >> 28] : 0;

    trace_rs6k_ds(sr, ea, cpu_lr());
    if (SR_BUID(sr) != BUID_IOCC0) {
        qemu_log_mask(LOG_UNIMP, "rs6k: access to BUID 0x%x sr=%08x ea=%08"
                      HWADDR_PRIx "\n", SR_BUID(sr), sr, ea);
        return NULL;
    }
    if (sr & SR_IOCC_SELECT) {
        *addr = ea & 0x0fffffff;
        return &s->iocc->ctl_as;
    }
    *addr = ((hwaddr)(sr & 0xf) << 28) | (ea & 0x0fffffff);
    return &s->iocc->bus_as;
}

static uint64_t ds_read(void *opaque, hwaddr ea, unsigned size)
{
    Rs6kState *s = opaque;
    hwaddr addr;
    AddressSpace *as = ds_target(s, ea, &addr);
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    if (!as) {
        return ~0ULL;
    }
    switch (size) {
    case 1:
        return address_space_ldub(as, addr, attrs, NULL);
    case 2:
        return address_space_lduw_be(as, addr, attrs, NULL);
    case 4:
        return address_space_ldl_be(as, addr, attrs, NULL);
    default:
        return address_space_ldq_be(as, addr, attrs, NULL);
    }
}

static void ds_write(void *opaque, hwaddr ea, uint64_t val, unsigned size)
{
    Rs6kState *s = opaque;
    hwaddr addr;
    AddressSpace *as = ds_target(s, ea, &addr);
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    if (!as) {
        return;
    }
    switch (size) {
    case 1:
        address_space_stb(as, addr, val, attrs, NULL);
        break;
    case 2:
        address_space_stw_be(as, addr, val, attrs, NULL);
        break;
    case 4:
        address_space_stl_be(as, addr, val, attrs, NULL);
        break;
    default:
        address_space_stq_be(as, addr, val, attrs, NULL);
    }
}

static const MemoryRegionOps ds_ops = {
    .read = ds_read,
    .write = ds_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

/*
 * Interrupt presentation layer of the processor at 0xff100000: XIRR
 * (CPPR || XISR) without and with side effects, DSIER and the MFRR used
 * for interprocessor interrupts (source 2). Sources are either level
 * sensitive (presented again after EOI while asserted) or one-shot
 * messages.
 */
#define ICP_ADDR        0xff100000
#define ICP_SIZE        0x10
#define ICP_SRC_IPI     2

static IcpSrc *icp_find(Rs6kState *s, uint32_t num)
{
    for (int i = 0; i < s->nsrc; i++) {
        if (s->src[i].num == num) {
            return &s->src[i];
        }
    }
    return NULL;
}

static void icp_update(Rs6kState *s)
{
    uint32_t num = 0;
    uint8_t prio = 0xff;

    if (s->mfrr < prio) {
        num = ICP_SRC_IPI;
        prio = s->mfrr;
    }
    for (int i = 0; i < s->nsrc; i++) {
        IcpSrc *src = &s->src[i];

        if (src->asserted && !src->in_service && src->prio < prio) {
            num = src->num;
            prio = src->prio;
        }
    }
    s->xisr = prio < s->cppr ? num : 0;
    s->xisr_prio = prio;
    qemu_set_irq(s->cpu_irq, s->xisr != 0);
}

static IcpSrc *icp_add_source(Rs6kState *s, uint32_t num, bool level,
                              void (*eoi)(void *opaque, uint32_t num),
                              void *opaque)
{
    IcpSrc *src = &s->src[s->nsrc++];

    assert(s->nsrc <= ICP_MAX_SRC);
    src->num = num;
    src->prio = 0xff;
    src->level = level;
    src->eoi = eoi;
    src->opaque = opaque;
    return src;
}

static void icp_set(Rs6kState *s, IcpSrc *src, uint8_t prio, bool asserted)
{
    src->prio = prio;
    src->asserted = asserted;
    icp_update(s);
}

static uint64_t icp_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kState *s = opaque;
    uint32_t xirr = (s->cppr << 24) | s->xisr;
    IcpSrc *src;
    uint64_t val;

    switch (addr) {
    case 0:
        val = xirr;
        break;
    case 4:
        val = size == 1 ? s->cppr : xirr;
        if (size == 4 && s->xisr) {
            s->cppr = s->xisr_prio;
            src = icp_find(s, s->xisr);
            if (src) {
                src->in_service = true;
                src->asserted &= src->level;
                if (src->accept) {
                    src->accept(src->opaque, src->num);
                }
            }
            icp_update(s);
        }
        break;
    case 8:
        val = s->dsier;
        break;
    case 12:
        val = s->mfrr;
        break;
    default:
        val = 0;
    }
    trace_rs6k_icp_read(addr, size, val);
    return val;
}

static void icp_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rs6kState *s = opaque;
    IcpSrc *src;

    trace_rs6k_icp_write(addr, size, val);
    switch (addr) {
    case 4:
        if (size == 1) {
            s->cppr = val;
            break;
        }
        s->cppr = val >> 24;
        src = icp_find(s, val & 0xffffff);
        if (src && src->in_service) {
            src->in_service = false;
            if (src->eoi) {
                src->eoi(src->opaque, src->num);
            }
        }
        break;
    case 8:
        s->dsier = val;
        break;
    case 12:
        s->mfrr = val;
        break;
    }
    icp_update(s);
}

static const MemoryRegionOps icp_ops = {
    .read = icp_read,
    .write = icp_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* IOCC interrupt level n arrives as source (BUID << 4) | n */
static void rs6k_iocc_present(void *opaque, int level, uint8_t prio,
                              bool asserted)
{
    Rs6kState *s = opaque;
    IcpSrc *src = s->iocc_src[level];

    if (src->prio != prio || src->asserted != asserted) {
        icp_set(s, src, prio, asserted);
    }
}

static void rs6k_iocc_accepted(void *opaque, uint32_t num)
{
    Rs6kState *s = opaque;

    rs6k_iocc_accept(s->iocc, num & 0xf);
}

/* An EOI written to XIRR goes on to the IOCC and unmasks the level */
static void rs6k_iocc_eoied(void *opaque, uint32_t num)
{
    Rs6kState *s = opaque;

    rs6k_iocc_eoi(s->iocc, num & 0xf);
}

/*
 * Storage error checker at 0xfef00000: an access to memory not backed by
 * an enabled bank latches the status and address and raises source 0x10.
 */
#define SEC_ADDR        0xfef00000
#define SEC_SIZE        0x100
#define SEC_SRC         0x10
#define SEC_ADDR_ERR    0x20000000

/*
 * SEC-DED check bits over each doubleword: columns of the check matrix for
 * data bits 0 (msb) to 63, as listed in the ROS. Check bit i is 1 << i.
 */
static const uint8_t ecc_columns[64] = {
    0xc4, 0xc2, 0xc1, 0xbc, 0xa2, 0xa1, 0x91, 0x92, 0x62, 0x61, 0xe0, 0x5e,
    0x51, 0xd0, 0xc8, 0x49, 0x31, 0xb0, 0x70, 0x2f, 0xa8, 0x68, 0x64, 0xa4,
    0x98, 0x58, 0x38, 0x97, 0x54, 0x34, 0x32, 0x52, 0x4c, 0x2c, 0x1c, 0xcb,
    0x2a, 0x1a, 0x19, 0x29, 0x26, 0x16, 0x0e, 0xe5, 0x15, 0x0d, 0x8c, 0x94,
    0x13, 0x0b, 0x07, 0xf2, 0x8a, 0x86, 0x46, 0x4a, 0x89, 0x85, 0x83, 0x79,
    0x45, 0x43, 0x23, 0x25,
};

#define SEC_UE          0x80000000
#define SEC_CE          0x40000000

static uint8_t ecc_bits(uint64_t dw)
{
    uint8_t ecc = 0;

    for (int i = 0; i < 64; i++) {
        if (dw & (1ULL << (63 - i))) {
            ecc ^= ecc_columns[i];
        }
    }
    return ecc;
}

static void sec_error(Rs6kState *s, uint32_t status, hwaddr addr)
{
    s->sec_status |= status;
    s->sec_addr = addr;
    if (!s->sec_mask) {
        icp_set(s, s->sec_src, 0, true);
    }
}

static uint64_t sec_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kState *s = opaque;
    uint32_t val = 0;

    switch (addr) {
    case 0x04:
        val = s->sec_status;
        s->sec_status = 0;
        break;
    case 0x0c:
        val = s->sec_addr;
        break;
    case 0x14:
        val = s->sec_mask;
        break;
    }
    trace_rs6k_sec_read(addr, val);
    return val;
}

static void sec_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rs6kState *s = opaque;

    trace_rs6k_sec_write(addr, val);
    if (addr == 0x14) {
        s->sec_mask = val;
    }
}

static const MemoryRegionOps sec_ops = {
    .read = sec_read,
    .write = sec_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Sparse byte-wide register file for system registers that have no
 * behaviour yet; unwritten bytes read as 0xff.
 */
static uint64_t regfile_read(Rs6kState *s, uint32_t key, unsigned size)
{
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        gpointer v = g_hash_table_lookup(s->regs, GUINT_TO_POINTER(key + i));

        val = (val << 8) | (v ? GPOINTER_TO_UINT(v) & 0xff : 0xff);
    }
    return val;
}

static void regfile_write(Rs6kState *s, uint32_t key, uint64_t val,
                          unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        uint8_t b = val >> ((size - 1 - i) * 8);

        g_hash_table_insert(s->regs, GUINT_TO_POINTER(key + i),
                            GUINT_TO_POINTER(0x100 | b));
    }
}

/* System registers at 0xff000000 */
#define SYSREG_ADDR 0xff000000
#define SYSREG_SIZE 0x00e00000

/* Power status and keylock decode: the key position in the low two bits */
#define KEYLOCK     0x0000e4

/*
 * Bus unit controllers: writing 0x80000000 | n to the select register maps
 * the configuration registers of BUC n at 0xff200000. Each starts with a
 * device ID whose top nibble is the type (1 memory, 3 I/O, 0xf absent).
 */
#define BUC_SEL     0x00000c
#define BUC_ADDR    0xff200000
#define BUC_SIZE    0x00100000
#define BUC_ON      0x80000000
#define BUC_MEMC    2
#define BUC_IOCC    3
#define BUC_GXT     4
#define BUC_KEY(n, off) ((0x10 + (n)) << 24 | (off))

#define MEMC_CTL        0x1004
#define MEMC_ECC_TEST   0x18000000
#define MEMC_BANKS      0x1014

/* the feature ROM of the selected bus unit */
#define FROM_ADDR   0x00a00000
#define FROM_END    0x00c00000

static bool buc_gxt_selected(Rs6kState *s)
{
    return s->gxt && s->buc_sel == (BUC_ON | BUC_GXT);
}

static uint64_t sysreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kState *s = opaque;
    uint64_t val = 0;

    if (buc_gxt_selected(s) && addr >= FROM_ADDR && addr < FROM_END) {
        for (unsigned i = 0; i < size; i++) {
            val = val << 8 | rs6k_gxt_rom_read(s->gxt, addr - FROM_ADDR + i);
        }
        return val;
    }
    if (addr >= KEYLOCK && addr + size <= KEYLOCK + 4) {
        val = extract32(~3u | rs6k_panel_key(s->panel),
                        (KEYLOCK + 4 - addr - size) * 8, size * 8);
    } else {
        val = regfile_read(s, addr, size);
    }
    trace_rs6k_sysreg_read(SYSREG_ADDR + addr, size, val, cpu_lr());
    return val;
}

static void sysreg_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    Rs6kState *s = opaque;

    trace_rs6k_sysreg_write(SYSREG_ADDR + addr, size, val, cpu_lr());
    regfile_write(s, addr, val, size);
    if (addr == BUC_SEL && size == 4) {
        s->buc_sel = val;
    }
}

static const MemoryRegionOps sysreg_ops = {
    .read = sysreg_read,
    .write = sysreg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

static uint32_t buc_get(Rs6kState *s, int buc, hwaddr off)
{
    return regfile_read(s, BUC_KEY(buc, off), 4);
}

static void buc_preset(Rs6kState *s, int buc, hwaddr off, uint32_t val)
{
    regfile_write(s, BUC_KEY(buc, off), val, 4);
}

static uint64_t buc_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kState *s = opaque;
    uint64_t val = ~0ULL >> (64 - size * 8);

    if (buc_gxt_selected(s)) {
        val = extract64(rs6k_gxt_cfg_read(s->gxt, addr & ~3),
                        (4 - (addr & 3) - size) * 8, size * 8);
    } else if (s->buc_sel & BUC_ON) {
        val = regfile_read(s, BUC_KEY(s->buc_sel & 0xf, addr), size);
    }
    trace_rs6k_buc_read(s->buc_sel & 0xf, addr, val);
    return val;
}

static void buc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rs6kState *s = opaque;

    trace_rs6k_buc_write(s->buc_sel & 0xf, addr, val);
    if (buc_gxt_selected(s) && size == 4) {
        rs6k_gxt_cfg_write(s->gxt, addr, val);
    } else if (s->buc_sel & BUC_ON) {
        regfile_write(s, BUC_KEY(s->buc_sel & 0xf, addr), val, size);
    }
}

static const MemoryRegionOps buc_ops = {
    .read = buc_read,
    .write = buc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

/*
 * Battery-backed NVRAM at 0xff600000 holds the ROS configuration, the IPL
 * device list and the AIX error log; the word at 0x300 is the operator
 * panel's LED display. The "nvram" machine property keeps the contents in
 * a file.
 */
#define NVRAM_ADDR  0xff600000
#define NVRAM_SIZE  (32 * KiB)
#define LED_OFF     0x300

static void rs6k_init_nvram(Rs6kState *s, MemoryRegion *sysmem,
                            const char *path)
{
    if (path) {
        memory_region_init_ram_from_file(&s->nvram, NULL, "rs6k.nvram",
                                         NVRAM_SIZE, 0, RAM_SHARED, path, 0,
                                         &error_fatal);
    } else {
        memory_region_init_ram(&s->nvram, NULL, "rs6k.nvram", NVRAM_SIZE,
                               &error_fatal);
    }
    memory_region_add_subregion_overlap(sysmem, NVRAM_ADDR, &s->nvram, 1);
}

/* The panel's id ties its window to the clicks that turn the key */
static void rs6k_init_panel(Rs6kState *s, MachineState *machine)
{
    const char *key = RS6K_MACHINE(machine)->key;

    s->panel = qdev_new(TYPE_RS6K_PANEL);
    s->panel->id = g_strdup("panel");
    object_property_add_child(OBJECT(machine), "panel", OBJECT(s->panel));
    if (key) {
        object_property_set_str(OBJECT(s->panel), "key", key, &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->panel), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->panel), 0,
                            NVRAM_ADDR + LED_OFF, 2);
}

/* Planar VPD: keyword records '*', two letters, length in halfwords */
#define VPD_ADDR    0xffe00000
#define VPD_SIZE    0x00100000

static const uint8_t planar_vpd[] = {
    '*', 'Z', '0', 5, 80, 40, 40, 0, 0, 0,
};

/* AIX takes the Ethernet address from the section its description opens */
static const uint8_t ent_vpd[] = {
    '*', 'D', 'S', 11, 'S', 'T', 'A', 'N', 'D', 'A', 'R', 'D', ' ',
    'E', 'T', 'H', 'E', 'R', 'N', 'E', 'T', 0,
    '*', 'N', 'A', 5,
};

static void rs6k_init_vpd(Rs6kState *s, const uint8_t *mac)
{
    memcpy(s->vpd_data, planar_vpd, sizeof(planar_vpd));
    s->vpd_len = sizeof(planar_vpd);
    if (!mac) {
        return;
    }
    memcpy(s->vpd_data + s->vpd_len, ent_vpd, sizeof(ent_vpd));
    s->vpd_len += sizeof(ent_vpd);
    memcpy(s->vpd_data + s->vpd_len, mac, 6);
    s->vpd_len += 6;
}

static uint64_t vpd_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr a = addr + i;

        val = (val << 8) | (a < s->vpd_len ? s->vpd_data[a] : 0xff);
    }
    return val;
}

static void vpd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps vpd_ops = {
    .read = vpd_read,
    .write = vpd_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

/*
 * The ROS is cacheable: dcbz on it builds a zeroed cache block that dcbi
 * or dcbf discard. Model the cache with a writable copy of the ROS.
 */
static void rs6k_dcache_discard(void *opaque, hwaddr addr, bool flush)
{
    Rs6kState *s = opaque;

    if (addr >= ROS_ADDR && addr < ROS_ADDR + ROS_SIZE) {
        memcpy(memory_region_get_ram_ptr(&s->ros) + (addr - ROS_ADDR),
               s->ros_image + (addr - ROS_ADDR), 32);
        return;
    }
    if (addr >= s->ram_size) {
        return;
    }
    /*
     * A line written back in an ECC diagnostic mode has zero check bits and
     * fails when read again; the ROS always reads it and then does a dcbi.
     */
    if (!flush) {
        if (s->ecc_status && addr == s->ecc_addr) {
            sec_error(s, s->ecc_status, addr);
        }
        return;
    }
    /* the write-back reaches memory only through the enabled banks */
    if (!buc_get(s, BUC_MEMC, MEMC_BANKS)) {
        sec_error(s, SEC_ADDR_ERR, addr);
        return;
    }
    s->ecc_status = 0;
    if (!(buc_get(s, BUC_MEMC, MEMC_CTL) & MEMC_ECC_TEST)) {
        return;
    }
    for (int i = 0; i < 32; i += 8) {
        uint8_t syn = ecc_bits(ldq_be_p(s->ram_ptr + addr + i));

        if (syn) {
            s->ecc_status = (ctpop8(syn) & 1 ? SEC_CE : SEC_UE) | syn;
            s->ecc_addr = addr;
            break;
        }
    }
}

static void rs6k_reset(void *opaque)
{
    Rs6kState *s = opaque;

    memcpy(memory_region_get_ram_ptr(&s->ros), s->ros_image, ROS_SIZE);
    s->buc_sel = 0;
    s->cppr = 0;
    s->mfrr = 0xff;
    for (int i = 0; i < s->nsrc; i++) {
        s->src[i].asserted = false;
        s->src[i].in_service = false;
    }
    s->sec_status = 0;
    s->sec_mask = 0xfe;
    icp_update(s);
    cpu_reset(CPU(s->cpu));
    cpu_ppc_tb_reset(&s->cpu->env);
}

static void rs6k_init_iocc(Rs6kState *s)
{
    DeviceState *sio, *scsi, *ent;

    s->iocc = RS6K_IOCC(qdev_new(TYPE_RS6K_IOCC));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->iocc), &error_fatal);
    for (int i = 0; i < IOCC_LEVELS; i++) {
        s->iocc_src[i] = icp_add_source(s, BUID_IOCC0 << 4 | i, false,
                                        rs6k_iocc_eoied, s);
        s->iocc_src[i]->accept = rs6k_iocc_accepted;
    }
    rs6k_iocc_set_presenter(s->iocc, rs6k_iocc_present, s);

    sio = qdev_new(TYPE_RS6K_SIO);
    object_property_set_link(OBJECT(sio), "iocc", OBJECT(s->iocc),
                             &error_abort);
    qdev_prop_set_chr(sio, "chardev0", serial_hd(0));
    qdev_prop_set_chr(sio, "chardev1", serial_hd(1));
    qdev_realize_and_unref(sio, NULL, &error_fatal);
    qemu_register_resettable(OBJECT(sio));

    scsi = qdev_new(TYPE_RS6K_SCSI);
    object_property_set_link(OBJECT(scsi), "iocc", OBJECT(s->iocc),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(scsi), &error_fatal);

    ent = qemu_create_nic_device(TYPE_RS6K_ENT, true, NULL);
    if (!ent) {
        rs6k_init_vpd(s, NULL);
        return;
    }
    object_property_set_link(OBJECT(ent), "iocc", OBJECT(s->iocc),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ent), &error_fatal);
    rs6k_init_vpd(s, rs6k_ent_macaddr(ent));
}

/*
 * SIMM presence detect: the top nibble of each of the two registers codes
 * the memory in one pair of banks, largest first.
 */
static const struct {
    uint32_t mb;
    uint32_t pd;
} simm_pairs[] = {
    { 128, 0x9 }, { 64, 0x1 }, { 32, 0xd }, { 16, 0x2 }, { 8, 0x3 },
};

static void rs6k_init_simms(Rs6kState *s, uint64_t mb)
{
    static const hwaddr pd_reg[] = { 0x1024, 0x102c };

    for (int i = 0; i < ARRAY_SIZE(pd_reg); i++) {
        uint32_t pd = 0;

        for (int j = 0; j < ARRAY_SIZE(simm_pairs); j++) {
            if (mb >= simm_pairs[j].mb) {
                pd = simm_pairs[j].pd;
                mb -= simm_pairs[j].mb;
                break;
            }
        }
        buc_preset(s, BUC_MEMC, pd_reg[i], pd << 28);
    }
}

/*
 * Time of day: a DS1285 (MC146818) whose index and data ports appear at
 * 0xff0000c0 and 0xff0000c4. AIX expects register A at 0x20, else it
 * reinitialises the clock to 1970.
 */
#define TOD_ADDR    0xff0000c0
#define TOD_IO      0x70

static void tod_irq(void *opaque, int n, int level)
{
}

static void rs6k_init_tod(MemoryRegion *sysmem)
{
    MemoryRegion *io = g_new(MemoryRegion, 1);
    MemoryRegion *port = g_new(MemoryRegion, 2);
    MC146818RtcState *rtc;
    ISABus *bus;

    memory_region_init(io, NULL, "rs6k.tod-io", 64 * KiB);
    bus = isa_bus_new(NULL, sysmem, io, &error_fatal);
    rtc = mc146818_rtc_init(bus, 2000, qemu_allocate_irq(tod_irq, NULL, 0));
    mc146818rtc_set_cmos_data(rtc, RTC_REG_A, 0x20);
    for (int i = 0; i < 2; i++) {
        memory_region_init_alias(&port[i], NULL, "rs6k.tod", io, TOD_IO + i,
                                 1);
        memory_region_add_subregion_overlap(sysmem, TOD_ADDR + i * 4,
                                            &port[i], 1);
    }
}

static void rs6k_gxt_present(void *opaque, uint32_t source, uint8_t prio,
                             bool asserted)
{
    Rs6kState *s = opaque;

    s->gxt_src->num = source;
    if (s->gxt_src->prio != prio || s->gxt_src->asserted != asserted) {
        icp_set(s, s->gxt_src, prio, asserted);
    }
}

/* A GXT150L given with -device sits in the graphics slot, bus unit 4 */
static void rs6k_machine_done(Notifier *notifier, void *data)
{
    Rs6kState *s = container_of(notifier, Rs6kState, machine_done);

    s->gxt = DEVICE(object_resolve_type_unambiguous(TYPE_RS6K_GXT, NULL));
    if (!s->gxt) {
        return;
    }
    s->gxt_src = icp_add_source(s, 0, true, NULL, NULL);
    rs6k_gxt_set_presenter(s->gxt, rs6k_gxt_present, s);
}

static void rs6k_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *unassigned = g_new(MemoryRegion, 1);
    MemoryRegion *ds = g_new(MemoryRegion, 1);
    const char *fw = machine->firmware ?: "11H5778_7006-41T_U5.BIN";
    g_autofree char *filename = NULL;
    Rs6kState *s = g_new0(Rs6kState, 1);
    PowerPCCPU *cpu;

    cpu = POWERPC_CPU(cpu_create(machine->cpu_type));
    s->cpu = cpu;
    if (!(cpu->env.flags & POWERPC_FLAG_601)) {
        error_report("this machine needs a PowerPC 601");
        exit(1);
    }
    /* The 601 RTC and decrementer count nanoseconds */
    cpu_ppc_tb_init(&cpu->env, 1000 * 1000 * 1000);
    qemu_register_reset(rs6k_reset, s);
    s->machine_done.notify = rs6k_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
    cpu->env.dcache_discard = rs6k_dcache_discard;
    cpu->env.dcache_discard_opaque = s;
    s->cpu_irq = qdev_get_gpio_in(DEVICE(cpu), PPC6xx_INPUT_INT);

    memory_region_add_subregion(sysmem, 0, machine->ram);
    s->ram_size = machine->ram_size;
    s->ram_ptr = memory_region_get_ram_ptr(machine->ram);

    memory_region_init_io(unassigned, NULL, &unassigned_ops, NULL,
                          "rs6k.unassigned", 4 * GiB);
    memory_region_add_subregion_overlap(sysmem, 0, unassigned, -1000);

    rs6k_init_iocc(s);
    memory_region_init_io(ds, NULL, &ds_ops, s, "rs6k.direct-store",
                          4 * GiB);
    memory_region_add_subregion(sysmem, PPC601_DIRECT_STORE_BASE, ds);

    s->regs = g_hash_table_new(NULL, NULL);
    memory_region_init_io(&s->sysregs, NULL, &sysreg_ops, s, "rs6k.sysregs",
                          SYSREG_SIZE);
    memory_region_add_subregion(sysmem, SYSREG_ADDR, &s->sysregs);
    /* memory controller: identification and SIMM presence detect */
    buc_preset(s, BUC_MEMC, 0x0000, 0x14190000);
    buc_preset(s, BUC_MEMC, 0x0004, 0x00100001);
    rs6k_init_simms(s, machine->ram_size / MiB);
    /* IOCC: I/O type, one BUID */
    buc_preset(s, BUC_IOCC, 0x0000, 0x34000000);
    buc_preset(s, BUC_IOCC, 0x0004, 0x00300000);
    memory_region_init_io(&s->buc, NULL, &buc_ops, s, "rs6k.buc", BUC_SIZE);
    memory_region_add_subregion_overlap(sysmem, BUC_ADDR, &s->buc, 1);

    memory_region_init_io(&s->icp, NULL, &icp_ops, s, "rs6k.icp", ICP_SIZE);
    memory_region_add_subregion_overlap(sysmem, ICP_ADDR, &s->icp, 1);
    s->sec_src = icp_add_source(s, SEC_SRC, false, NULL, NULL);
    memory_region_init_io(&s->sec, NULL, &sec_ops, s, "rs6k.sec", SEC_SIZE);
    memory_region_add_subregion(sysmem, SEC_ADDR, &s->sec);
    memory_region_init_io(&s->vpd, NULL, &vpd_ops, s, "rs6k.vpd", VPD_SIZE);
    memory_region_add_subregion(sysmem, VPD_ADDR, &s->vpd);
    rs6k_init_nvram(s, sysmem, RS6K_MACHINE(machine)->nvram);
    rs6k_init_panel(s, machine);
    rs6k_init_tod(sysmem);

    memory_region_init_ram(&s->ros, NULL, "rs6k.ros", ROS_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, ROS_ADDR, &s->ros);
    s->ros_image = g_malloc0(ROS_SIZE);
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, fw);
    if (!filename || load_image_size(filename, s->ros_image, ROS_SIZE) < 0) {
        error_report("could not load ROS image '%s'", fw);
        exit(1);
    }
}

/*
 * IBM CD-ROM drives power up with 512-byte blocks, which AIX expects. The
 * ROS sets them again with a SCSI-1 (PF=0) MODE SELECT.
 */
static GlobalProperty rs6k_compat[] = {
    { "scsi-cd", "physical_block_size", "512" },
    { "scsi-cd", "quirk_mode_page_vendor_specific_apple", "on" },
};

static char *rs6k_get_nvram(Object *obj, Error **errp)
{
    return g_strdup(RS6K_MACHINE(obj)->nvram);
}

static void rs6k_set_nvram(Object *obj, const char *value, Error **errp)
{
    Rs6kMachineState *ms = RS6K_MACHINE(obj);

    g_free(ms->nvram);
    ms->nvram = g_strdup(value);
}

static char *rs6k_get_key(Object *obj, Error **errp)
{
    return g_strdup(RS6K_MACHINE(obj)->key);
}

static void rs6k_set_key(Object *obj, const char *value, Error **errp)
{
    Rs6kMachineState *ms = RS6K_MACHINE(obj);

    g_free(ms->key);
    ms->key = g_strdup(value);
}

static void rs6k_machine_init(MachineClass *mc)
{
    ObjectClass *oc = OBJECT_CLASS(mc);

    object_class_property_add_str(oc, "nvram", rs6k_get_nvram,
                                  rs6k_set_nvram);
    object_class_property_set_description(oc, "nvram",
                                          "File keeping the NVRAM contents");
    object_class_property_add_str(oc, "key", rs6k_get_key, rs6k_set_key);
    object_class_property_set_description(oc, "key",
                                          "Key mode switch at power-on: "
                                          "normal, service or secure");
    compat_props_add(mc->compat_props, rs6k_compat, G_N_ELEMENTS(rs6k_compat));
    mc->desc = "IBM RS/6000 7006 (PowerPC 601, Micro Channel)";
    mc->init = rs6k_init;
    mc->max_cpus = 1;
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "rs6k.ram";
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("601_v1");
    mc->block_default_type = IF_SCSI;
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_RS6K_GXT);
}

DEFINE_MACHINE_EXTENDED("7006", MACHINE, Rs6kMachineState, rs6k_machine_init,
                        false, NULL)
