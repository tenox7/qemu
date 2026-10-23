/*
 * IBM RS/6000 PowerPC I/O Channel Controller (Micro Channel bridge)
 *
 * The 601 reaches the IOCC through I/O controller interface segments
 * (T=1, BUID 0x20). With the IOCC select bit set the offset addresses the
 * control space: on-chip RAM, the IOCC registers and the POS registers of
 * the 16 slots. Otherwise it addresses the Micro Channel, where the first
 * 64K are I/O cycles and the rest are memory cycles.
 *
 * The IOCC is the DMA controller for DMA slaves: a Channel Status register
 * per arbitration level selects a channel whose address register holds a
 * TCW number and offset. The TCW table lives in system memory at the
 * address in the TCE register. Interrupt level n is presented to the
 * processor as source (BUID << 4) | n at the priority in its XIVR, once,
 * until an end of interrupt command to that level.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/ppc/rs6k_iocc.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include "trace.h"

#define CTL_REGS_ADDR   0x010000
#define CTL_POS_ADDR    0x400000
#define CTL_SIZE        0x10000000

#define REG_TCE         0x09c
#define REG_IEE         0x180
#define REG_IRR         0x188
#define REG_MIR         0x190
#define REG_XIVR        0x200
#define REG_SLAVE       0x380
#define REG_CSR         0x400
#define REG_EOI         0x480
#define REG_ARB         0x500
#define REG_ARB_END     0x540

#define CSR_STATUS      0xf0000000
#define CSR_ENABLED     0x10000000
#define CSR_SLAVE       0x08000000
#define CSR_SYSMEM      0x04000000
#define CSR_COUNT       0x000fffff

#define TCW_SYSMEM      0x2

#define REG(s, off)     ((s)->regs[(off) >> 2])
#define LEVEL_BIT(n)    (0x80000000u >> (n))

static uint32_t iocc_lr(void)
{
    return current_cpu ? POWERPC_CPU(current_cpu)->env.lr : 0;
}

static uint64_t iocc_unassigned_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "iocc: unassigned %s read%u 0x%08" HWADDR_PRIx
                  " lr=%08x\n", (char *)opaque, size * 8, addr, iocc_lr());
    return ~0ULL;
}

static void iocc_unassigned_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "iocc: unassigned %s write%u 0x%08" HWADDR_PRIx
                  " = 0x%" PRIx64 " lr=%08x\n", (char *)opaque, size * 8, addr,
                  val, iocc_lr());
}

static const MemoryRegionOps iocc_unassigned_ops = {
    .read = iocc_unassigned_read,
    .write = iocc_unassigned_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
};

static void iocc_update_irq(Rs6kIOCCState *s)
{
    uint16_t active = s->lines & ~s->masked;

    if (!s->present) {
        return;
    }
    for (int n = 0; n < IOCC_LEVELS; n++) {
        bool on = (active & (1 << n)) && (REG(s, REG_IEE) & LEVEL_BIT(n));

        s->present(s->present_opaque, n, REG(s, REG_XIVR + n * 4), on);
    }
}

static void iocc_set_irq(void *opaque, int n, int level)
{
    Rs6kIOCCState *s = opaque;

    s->lines = deposit32(s->lines, n, 1, level != 0);
    iocc_update_irq(s);
}

void rs6k_iocc_set_presenter(Rs6kIOCCState *s, Rs6kIOCCPresent fn,
                             void *opaque)
{
    s->present = fn;
    s->present_opaque = opaque;
}

void rs6k_iocc_accept(Rs6kIOCCState *s, int level)
{
    s->masked |= 1 << level;
    iocc_update_irq(s);
}

void rs6k_iocc_eoi(Rs6kIOCCState *s, int level)
{
    s->masked &= ~(1 << level);
    iocc_update_irq(s);
}

static uint32_t iocc_irr(Rs6kIOCCState *s)
{
    uint32_t irr = 0;

    for (int n = 0; n < IOCC_LEVELS; n++) {
        if (s->lines & (1 << n)) {
            irr |= LEVEL_BIT(n);
        }
    }
    return irr & REG(s, REG_IEE);
}

/* Enable command: a load moves a disabled channel (B'00x0') to enabled */
static uint32_t iocc_enable(Rs6kIOCCState *s, int arb)
{
    uint32_t *csr = &REG(s, REG_CSR + arb * 4);
    uint32_t old = *csr;

    if (!(old & 0xd0000000)) {
        *csr |= CSR_ENABLED;
    }
    return old;
}

/* Disable command: a store moves an enabled channel (B'00x1') to disabled */
static void iocc_disable(Rs6kIOCCState *s, int arb)
{
    uint32_t *csr = &REG(s, REG_CSR + arb * 4);

    if ((*csr & 0xd0000000) == CSR_ENABLED) {
        *csr &= ~CSR_ENABLED;
    }
}

static uint64_t iocc_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kIOCCState *s = opaque;
    uint32_t val = REG(s, addr);

    if (addr == REG_IRR) {
        val = iocc_irr(s);
    } else if (addr >= REG_ARB && addr < REG_ARB_END) {
        val = iocc_enable(s, (addr - REG_ARB) >> 2);
    }
    trace_rs6k_iocc_reg_read(CTL_REGS_ADDR + addr, val);
    return val;
}

static void iocc_reg_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Rs6kIOCCState *s = opaque;

    trace_rs6k_iocc_reg_write(CTL_REGS_ADDR + addr, val);
    if (addr >= REG_ARB && addr < REG_ARB_END) {
        iocc_disable(s, (addr - REG_ARB) >> 2);
        return;
    }
    if (addr >= REG_EOI && addr < REG_EOI + IOCC_LEVELS * 4) {
        rs6k_iocc_eoi(s, (addr - REG_EOI) >> 2);
        return;
    }
    switch (addr) {
    case REG_IRR:
        return;
    case REG_MIR:
        REG(s, addr) &= val;
        return;
    }
    REG(s, addr) = val;
    if (addr == REG_IEE || (addr >= REG_XIVR && addr < REG_XIVR + 0x40)) {
        iocc_update_irq(s);
    }
}

static const MemoryRegionOps iocc_reg_ops = {
    .read = iocc_reg_read,
    .write = iocc_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Real address of bus address ba through the TCW table, or -1 if unmapped.
 * The low bits of the TCE register size the table at 32K << n.
 */
static hwaddr iocc_tcw_translate(Rs6kIOCCState *s, uint32_t ba)
{
    uint32_t tce = REG(s, REG_TCE);
    uint32_t entries = (32 * KiB << (tce & 7)) / 4;
    uint32_t tcw;

    if (ba >> 12 >= entries) {
        return -1;
    }
    tcw = address_space_ldl_be(&address_space_memory,
                               (tce & ~0xfffu) + (ba >> 12) * 4,
                               MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(tcw & TCW_SYSMEM)) {
        return -1;
    }
    return (tcw & ~0xfffu) | (ba & 0xfff);
}

/* Bus masters reach system memory through the TCW table */
static IOMMUTLBEntry iocc_iommu_translate(IOMMUMemoryRegion *iommu,
                                          hwaddr addr, IOMMUAccessFlags flag,
                                          int iommu_idx)
{
    Rs6kIOCCState *s = container_of(iommu, Rs6kIOCCState, iommu);
    hwaddr pa = iocc_tcw_translate(s, addr);
    IOMMUTLBEntry e = {
        .target_as = &address_space_memory,
        .iova = addr & ~0xfffULL,
        .translated_addr = pa & ~0xfffULL,
        .addr_mask = 0xfff,
        .perm = IOMMU_RW,
    };

    if (pa == -1) {
        e.target_as = &s->bus_as;
        e.translated_addr = e.iova;
    }
    return e;
}

static bool iocc_dma_ready(uint32_t csr)
{
    return (csr & (CSR_STATUS | CSR_SLAVE)) == (CSR_ENABLED | CSR_SLAVE);
}

/* Bytes left for the DMA slave on arbitration level arb, 0 if not enabled */
int rs6k_iocc_dma_count(Rs6kIOCCState *s, int arb)
{
    uint32_t csr = REG(s, REG_CSR + arb * 4);

    return iocc_dma_ready(csr) ? (csr & CSR_COUNT) + 1 : 0;
}

/*
 * Third-party DMA for the DMA slave on arbitration level arb: move up to
 * len bytes between buf and memory. Returns the bytes moved; tc is set when
 * the length count expires, which also disables the channel.
 */
int rs6k_iocc_dma(Rs6kIOCCState *s, int arb, uint8_t *buf, int len,
                  bool to_device, bool *tc)
{
    uint32_t *csr = &REG(s, REG_CSR + arb * 4);
    uint32_t *addr = &REG(s, REG_SLAVE + ((*csr >> 20) & 0xf) * 4);
    int n;

    *tc = false;
    if (!iocc_dma_ready(*csr)) {
        return 0;
    }
    for (n = 0; n < len && !*tc; n++) {
        AddressSpace *as = &s->bus_as;
        hwaddr pa = *addr;

        if (*csr & CSR_SYSMEM) {
            as = &address_space_memory;
            pa = iocc_tcw_translate(s, *addr);
        }
        trace_rs6k_iocc_dma(arb, *addr, pa, to_device);
        if (pa == -1) {
            break;
        }
        address_space_rw(as, pa, MEMTXATTRS_UNSPECIFIED, buf + n, 1,
                         !to_device);
        (*addr)++;
        *csr = (*csr & ~CSR_COUNT) | ((*csr - 1) & CSR_COUNT);
        if ((*csr & CSR_COUNT) == CSR_COUNT) {
            *csr &= ~CSR_STATUS;
            *tc = true;
        }
    }
    return n;
}

/* Slot n has its eight POS bytes at offset n << 16 */
static uint64_t iocc_pos_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kIOCCState *s = opaque;
    int slot = (addr >> 16) & 0xf;
    Rs6kIOCCSlot *sl = &s->slot[slot];
    int reg = addr & 7;
    uint8_t val;

    switch (reg) {
    case 0:
        val = sl->id;
        break;
    case 1:
        val = sl->id >> 8;
        break;
    default:
        val = sl->pos[reg];
    }
    trace_rs6k_iocc_pos_read(slot, reg, val);
    return val;
}

static void iocc_pos_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Rs6kIOCCState *s = opaque;
    int slot = (addr >> 16) & 0xf;
    Rs6kIOCCSlot *sl = &s->slot[slot];
    int reg = addr & 7;

    trace_rs6k_iocc_pos_write(slot, reg, val);
    if (reg < 2 || sl->id == 0xffff) {
        return;
    }
    sl->pos[reg] = val;
    if (sl->pos_write) {
        sl->pos_write(sl->opaque, reg, val);
    }
}

static const MemoryRegionOps iocc_pos_ops = {
    .read = iocc_pos_read,
    .write = iocc_pos_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

void rs6k_iocc_set_slot(Rs6kIOCCState *s, int slot, uint16_t id,
                        void (*pos_write)(void *opaque, int reg, uint8_t val),
                        void *opaque)
{
    s->slot[slot].id = id;
    s->slot[slot].pos_write = pos_write;
    s->slot[slot].opaque = opaque;
}

static void rs6k_iocc_reset(DeviceState *dev)
{
    Rs6kIOCCState *s = RS6K_IOCC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->masked = 0;
    for (int i = 0; i < IOCC_SLOTS; i++) {
        memset(s->slot[i].pos, 0xff, sizeof(s->slot[i].pos));
        s->slot[i].pos[2] = 0;
    }
    iocc_update_irq(s);
}

static void rs6k_iocc_realize(DeviceState *dev, Error **errp)
{
    Rs6kIOCCState *s = RS6K_IOCC(dev);
    Object *obj = OBJECT(dev);

    memory_region_init(&s->ctl, obj, "iocc.ctl", CTL_SIZE);
    memory_region_init_io(&s->ctl_unassigned, obj, &iocc_unassigned_ops,
                          (void *)"ctl", "iocc.ctl-unassigned", CTL_SIZE);
    memory_region_add_subregion_overlap(&s->ctl, 0, &s->ctl_unassigned, -1);
    memory_region_init_ram(&s->ctl_ram, obj, "iocc.ram", IOCC_RAM_SIZE, errp);
    memory_region_add_subregion(&s->ctl, 0, &s->ctl_ram);
    memory_region_init_io(&s->ctl_regs, obj, &iocc_reg_ops, s, "iocc.regs",
                          IOCC_REGS_SIZE);
    memory_region_add_subregion_overlap(&s->ctl, CTL_REGS_ADDR, &s->ctl_regs,
                                        1);
    memory_region_init_io(&s->ctl_pos, obj, &iocc_pos_ops, s, "iocc.pos",
                          IOCC_SLOTS << 16);
    memory_region_add_subregion(&s->ctl, CTL_POS_ADDR, &s->ctl_pos);
    address_space_init(&s->ctl_as, &s->ctl, "iocc-ctl");

    memory_region_init(&s->bus, obj, "mca", 4 * GiB);
    memory_region_init_io(&s->bus_unassigned, obj, &iocc_unassigned_ops,
                          (void *)"bus", "mca.unassigned", 4 * GiB);
    memory_region_add_subregion_overlap(&s->bus, 0, &s->bus_unassigned, -1);
    memory_region_init(&s->bus_io, obj, "mca.io", 64 * KiB);
    memory_region_add_subregion(&s->bus, 0, &s->bus_io);
    /* AIX reaches the registers through bus segments too */
    memory_region_init_alias(&s->bus_regs, obj, "mca.iocc-regs", &s->ctl_regs,
                             0, IOCC_REGS_SIZE);
    memory_region_add_subregion(&s->bus, CTL_REGS_ADDR, &s->bus_regs);
    address_space_init(&s->bus_as, &s->bus, "mca");

    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_RS6K_IOCC_IOMMU, obj, "iocc.iommu", 4 * GiB);
    address_space_init(&s->dma_as, MEMORY_REGION(&s->iommu), "iocc-dma");

    qdev_init_gpio_in(dev, iocc_set_irq, IOCC_LEVELS);
    for (int i = 0; i < IOCC_SLOTS; i++) {
        s->slot[i].id = 0xffff;
    }
}

static void rs6k_iocc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rs6k_iocc_realize;
    device_class_set_legacy_reset(dc, rs6k_iocc_reset);
    dc->user_creatable = false;
}

static void rs6k_iocc_iommu_class_init(ObjectClass *klass, const void *data)
{
    IOMMU_MEMORY_REGION_CLASS(klass)->translate = iocc_iommu_translate;
}

static const TypeInfo rs6k_iocc_types[] = {
    {
        .name = TYPE_RS6K_IOCC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Rs6kIOCCState),
        .class_init = rs6k_iocc_class_init,
    },
    {
        .name = TYPE_RS6K_IOCC_IOMMU,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = rs6k_iocc_iommu_class_init,
    },
};

DEFINE_TYPES(rs6k_iocc_types)
