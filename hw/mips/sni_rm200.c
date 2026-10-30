/*
 * Siemens Nixdorf RM200C (PCI desktop, "ASIC PCI" / PCIMT chipset)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "qemu/datadir.h"
#include "qemu/timer.h"
#include "qemu/target-info.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/mips/mips.h"
#include "hw/intc/i8259.h"
#include "hw/dma/i8257.h"
#include "hw/isa/isa.h"
#include "hw/char/serial-isa.h"
#include "hw/char/parallel.h"
#include "hw/char/parallel-isa.h"
#include "hw/block/fdc.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/timer/i8254.h"
#include "hw/audio/pcspk.h"
#include "hw/input/i8042.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/qtest.h"
#include "system/tcg.h"
#include "target/mips/cpu.h"
#include "trace.h"

#define PROM_BASE       0x1fc00000
#define PROM_SIZE       (512 * KiB)
#define XBUS_BASE       0x1fd00000
#define NVRAM_BASE      0x1ff00000
#define NVRAM_SIZE      (32 * KiB)
#define ASIC_BASE       0x1fff0000
#define RM_ISA_MEM_BASE    0x10000000
#define RM_ISA_MEM_SIZE    0x04000000
#define RM_PCI_MEM_BASE    0x18000000
#define RM_PCI_MEM_SIZE    0x07c00000
#define RM_PCI_IO_BASE     0x14000000
#define RM_PCI_IO_SIZE     0x03c00000
#define CACHE_REPL_BASE 0x17c00000
#define CACHE_REPL_SIZE 0x00400000
#define INTACK_BASE     0x1a000000
#define MPA_BASE        0x1ffff000
#define MPA_SIZE        0x1000
#define RM_MAX_CPUS     4       /* real R4400 boards: 2 */

/* ITPEND / IRQSEL / CSITPEND bits */
#define IT_INT2  0x01
#define IT_INTD  0x02
#define IT_INTC  0x04
#define IT_INTB  0x08
#define IT_INTA  0x10
#define IT_EISA  0x20
#define IT_SCSI  0x40
#define IT_ETH   0x80
#define IT_PCI   (IT_INTA | IT_INTB | IT_INTC | IT_INTD)

#define SCSI_IRQ_DELAY_US   200

/* ASIC PCI registers, big-endian offsets */
#define ASIC_UCONF          0x04
#define ASIC_IOADTIMEOUT2   0x0c
#define ASIC_IOMEMCONF      0x14
#define ASIC_IOMMU          0x1c
#define ASIC_IOADTIMEOUT1   0x24
#define ASIC_DMAACCESS      0x2c
#define ASIC_DMAHIT         0x34
#define ASIC_ERRSTATUS      0x3c
#define ASIC_ERRADDR        0x44
#define ASIC_SYNDROME       0x4c
#define ASIC_ITPEND         0x54
#define ASIC_IRQSEL         0x5c
#define ASIC_TESTMEM        0x64
#define ASIC_ECCREG         0x6c
#define ASIC_CONFIG_ADDRESS 0x74
#define ASIC_ID             0x7c
#define ASIC_PIA_OE         0x84
#define ASIC_PIA_DATAOUT    0x8c
#define ASIC_PIA_DATAIN     0x94
#define ASIC_CACHECONF      0x9c
#define ASIC_INVSPACE       0xa4
#define ASIC_REGS_SIZE      0x1000

/* X-bus board registers, 64k apart */
enum {
    XB_CSMSR, XB_CSSWITCH, XB_CSITPEND, XB_AUTO_PO_EN, XB_CLR_TEMP,
    XB_AUTO_PO_DIS, XB_EXMSR, XB_UNUSED1, XB_CSWCSM, XB_UNUSED2, XB_CSLED,
    XB_CSMAPISA, XB_CSRSTBP, XB_CLRPOFF, XB_CSTIMER, XB_PWDN,
};

static const char *const xbus_names[16] = {
    "CSMSR", "CSSWITCH", "CSITPEND", "AUTO_PO_EN", "CLR_TEMP", "AUTO_PO_DIS",
    "EXMSR", "UNUSED1", "CSWCSM", "UNUSED2", "CSLED", "CSMAPISA", "CSRSTBP",
    "CLRPOFF", "CSTIMER", "PWDN",
};

/*
 * MP agent on R4400 CPU modules, one per CPU, each CPU sees its own at
 * MPA_BASE. 32-bit registers 8 bytes apart, little-endian offsets.
 */
#define MPA_CR          0x00    /* routing: bits 3-7 Int0-Int4, 9-12 messages */
#define MPA_MSG_SEND    0x10    /* message << 24 | data << 4 | target mask */
#define MPA_MSG_STATUS  0x18    /* bit 0 done, 1 no CPU, 2 retry, 7:4 CPUs */
#define MPA_CONF        0x20    /* bits 27-28 CPU number */
#define MPA_ITPEND      0x38    /* sources [10:0], routed to this CPU [22:12] */
#define MPA_MSG_RECV    0x40    /* last message received */

/* ITPEND sources: board Int0-Int4 (Int2 is the extra timer), messages */
#define MPA_IT_LINES    0x01f
#define MPA_IT_MSG      0x080
#define MPA_IT_CLOCKMSG 0x100
#define MPA_IT_INTERNAL 0x780
#define MPA_MSG_CLOCK   0x0b
#define MPA_QUEUE       8

/* Messages queue up instead of bouncing: neither PROM nor HAL resend */
typedef struct SniMpa {
    CPUMIPSState *env;
    uint32_t reg[MPA_SIZE / 8];
    uint32_t msg;
    uint32_t queue[MPA_QUEUE];
    int queued;
    uint16_t pend;
} SniMpa;

#define TYPE_SNI_RM200_MACHINE MACHINE_TYPE_NAME("sni-rm200")
#define TYPE_SNI_RM300_MACHINE MACHINE_TYPE_NAME("sni-rm300")
OBJECT_DECLARE_SIMPLE_TYPE(SniRm200MachineState, SNI_RM200_MACHINE)

struct SniRm200MachineState {
    MachineState parent_obj;
    char *nvram_file;
    uint8_t csmsr;
    uint8_t csswitch;
    int ncpus;
    uint8_t mpa_lines;
    QEMUTimer *boot_timer;
    QEMUTimer *slice_timer;
    uint32_t slice_us;
    uint32_t scsi_delay_us;
    SniMpa mpa[RM_MAX_CPUS];
};

#define TYPE_SNI_PCIMT "sni-pcimt-pcihost"
OBJECT_DECLARE_SIMPLE_TYPE(SniPcimtState, SNI_PCIMT)

struct SniPcimtState {
    PCIHostState parent_obj;

    MemoryRegion asic_mr;
    MemoryRegion xbus_mr;
    MemoryRegion cfg_mr;
    MemoryRegion intack_mr;
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    MemoryRegion isa_mem_win;
    MemoryRegion pci_mem_win;
    MemoryRegion pci_io_win;
    MemoryRegion io_bg;
    MemoryRegion mem_bg;

    qemu_irq cpu_irq[5];
    qemu_irq xtimer_irq;
    QEMUTimer *scsi_timer;
    QEMUTimer *xtimer;
    int64_t xtimer_next;
    int64_t xtimer_period;
    uint16_t xtimer_count[3];
    bool xtimer_msb[3];
    uint32_t asic[ASIC_REGS_SIZE / 4];
    uint8_t xbus[16];
    uint8_t pend;
    uint8_t csmsr;
    uint8_t csswitch;
    bool has_xtimer;
    uint32_t scsi_delay_us;
};

/*
 * IRQSEL is not a plain mask (SINIX PROM writes 0x0005a33f, SINIX 0x0005b7bf
 * and expects SCSI on IP3). The NT HAL leaves bits 16-18 at 0, only enables
 * Int0 and dispatches every source from CSITPEND, so 0 routes all to Int0.
 */
static void pcimt_update_irq(SniPcimtState *s)
{
    uint8_t act = s->pend;
    bool int0 = !(s->asic[ASIC_IRQSEL >> 2] & 0x70000);

    qemu_set_irq(s->cpu_irq[0], int0 ? !!act : !!(act & IT_INT2));
    qemu_set_irq(s->cpu_irq[1], !int0 && (act & (IT_EISA | IT_SCSI)));
    qemu_set_irq(s->cpu_irq[3], !int0 && (act & IT_PCI));
    qemu_set_irq(s->cpu_irq[4], !int0 && (act & IT_ETH));
}

static void pcimt_scsi_irq(void *opaque)
{
    SniPcimtState *s = opaque;

    s->pend |= IT_SCSI;
    pcimt_update_irq(s);
}

/*
 * Real disks take milliseconds. SINIX enters init with a stale splhi(), so a
 * SCSI completion arriving within microseconds can slip between biowait()'s
 * B_DONE check and sleep(), losing the wakeup and hanging the boot.
 * NT does not need this. Without -icount the delay costs at least 1 ms per
 * I/O on hosts without ppoll() (macOS), hence scsi-delay=0 for NT there.
 */
static void pcimt_set_pend(void *opaque, int bit, int level)
{
    SniPcimtState *s = opaque;

    if (1 << bit == IT_SCSI && level && s->scsi_delay_us) {
        if (!(s->pend & IT_SCSI) && !timer_pending(s->scsi_timer)) {
            timer_mod(s->scsi_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      s->scsi_delay_us * SCALE_US);
        }
        return;
    }
    if (1 << bit == IT_SCSI) {
        timer_del(s->scsi_timer);
    }
    if (level) {
        s->pend |= 1 << bit;
    } else {
        s->pend &= ~(1 << bit);
    }
    pcimt_update_irq(s);
}

/*
 * 32-bit registers sit in the low half of 64-bit slots: offset +4 in
 * big-endian mode, +0 in little-endian (NT) mode. Normalize to the former.
 */
static hwaddr asic_reg(hwaddr addr)
{
    return target_big_endian() ? addr : addr ^ 4;
}

static uint64_t asic_read(void *opaque, hwaddr addr, unsigned size)
{
    SniPcimtState *s = opaque;
    uint32_t val;

    addr = asic_reg(addr);
    val = s->asic[addr >> 2];

    if (addr == ASIC_ITPEND) {
        val = s->pend;
    }
    trace_sni_asic_read(addr, val);
    return val;
}

static void asic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SniPcimtState *s = opaque;

    addr = asic_reg(addr);
    trace_sni_asic_write(addr, val);
    switch (addr) {
    case ASIC_ID:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    case ASIC_ITPEND:
        return;
    }
    s->asic[addr >> 2] = val;
    if (addr == ASIC_IRQSEL) {
        pcimt_update_irq(s);
    }
}

static const MemoryRegionOps asic_ops = {
    .read = asic_read,
    .write = asic_write,
#if TARGET_BIG_ENDIAN
    .endianness = DEVICE_BIG_ENDIAN,
#else
    .endianness = DEVICE_LITTLE_ENDIAN,
#endif
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

/*
 * Extra timer behind CSTIMER on multiprocessor boards: an 8254 clocked at
 * 7.159 MHz, counter 0 output clocking counter 2. The NT HAL makes it the
 * clock tick of the second CPU and acknowledges it by writing UNUSED2.
 */
#define XTIMER_HZ       7159090

static void xtimer_tick(void *opaque)
{
    SniPcimtState *s = opaque;

    qemu_irq_raise(s->xtimer_irq);
    s->xtimer_next += s->xtimer_period;
    timer_mod(s->xtimer, s->xtimer_next);
}

static void xtimer_write(SniPcimtState *s, hwaddr addr, uint8_t val)
{
    int n = (addr >> 2) & 3;

    if (n == 3) {
        n = val >> 6;
        if (n < 3) {
            s->xtimer_msb[n] = false;
        }
        return;
    }
    if (!s->xtimer_msb[n]) {
        s->xtimer_count[n] = (s->xtimer_count[n] & 0xff00) | val;
        s->xtimer_msb[n] = true;
        return;
    }
    s->xtimer_count[n] = (s->xtimer_count[n] & 0x00ff) | (val << 8);
    s->xtimer_msb[n] = false;
    s->xtimer_period = muldiv64((s->xtimer_count[0] ?: 0x10000) *
                                (s->xtimer_count[2] ?: 0x10000),
                                NANOSECONDS_PER_SECOND, XTIMER_HZ);
    trace_sni_xtimer(s->xtimer_period);
    s->xtimer_next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->xtimer_period;
    timer_mod(s->xtimer, s->xtimer_next);
}

static uint8_t xbus_get(SniPcimtState *s, int reg)
{
    switch (reg) {
    case XB_CSMSR:
        return s->csmsr;
    case XB_CSSWITCH:
        return s->csswitch;
    case XB_CSITPEND:
        return (s->pend & IT_EISA) | (~s->pend & ~IT_EISA & 0xff);
    }
    return s->xbus[reg];
}

static uint64_t xbus_read(void *opaque, hwaddr addr, unsigned size)
{
    SniPcimtState *s = opaque;
    int reg = addr >> 16;
    uint8_t v = xbus_get(s, reg);

    trace_sni_xbus_read(xbus_names[reg], v);
    return v * 0x01010101u;
}

static void xbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SniPcimtState *s = opaque;
    int reg = addr >> 16;

    trace_sni_xbus_write(xbus_names[reg], val);
    s->xbus[reg] = val;
    if (reg == XB_CSWCSM && (val & 0xff) == 0xfd) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
    if (s->has_xtimer && reg == XB_CSTIMER) {
        xtimer_write(s, addr, val);
    }
    if (s->has_xtimer && reg == XB_UNUSED2) {
        qemu_irq_lower(s->xtimer_irq);
    }
}

static const MemoryRegionOps xbus_ops = {
    .read = xbus_read,
    .write = xbus_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static uint64_t pcicfg_read(void *opaque, hwaddr addr, unsigned size)
{
    SniPcimtState *s = opaque;
    uint32_t a = (s->asic[ASIC_CONFIG_ADDRESS >> 2] & 0x00fffffc) | addr;
    uint32_t val = pci_data_read(PCI_HOST_BRIDGE(s)->bus, a, size);

    trace_sni_pcicfg_read(a, size, val);
    return val;
}

static void pcicfg_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    SniPcimtState *s = opaque;
    uint32_t a = (s->asic[ASIC_CONFIG_ADDRESS >> 2] & 0x00fffffc) | addr;

    trace_sni_pcicfg_write(a, size, val);
    pci_data_write(PCI_HOST_BRIDGE(s)->bus, a, val, size);
}

static const MemoryRegionOps pcicfg_ops = {
    .read = pcicfg_read,
    .write = pcicfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t intack_read(void *opaque, hwaddr addr, unsigned size)
{
    return pic_read_irq(isa_pic);
}

static void intack_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
}

static const MemoryRegionOps intack_ops = {
    .read = intack_read,
    .write = intack_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t bg_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: read  0x%08" HWADDR_PRIx "/%d\n",
                  (const char *)opaque, addr, size);
    return ~0ull;
}

static void bg_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%08" HWADDR_PRIx "/%d = 0x%" PRIx64
                  "\n", (const char *)opaque, addr, size, val);
}

static const MemoryRegionOps bg_ops = {
    .read = bg_read,
    .write = bg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

/* PCI slot/pin -> pending bit, from Linux irq_tab_rm200 */
static int pcimt_map_irq(PCIDevice *d, int pin)
{
    static const uint8_t tab[8][4] = {
        { 0, 0, 0, 0 },     /* EISA bridge */
        { 6, 6, 6, 6 },     /* SCSI */
        { 7, 7, 7, 7 },     /* Ethernet */
        { 3, 3, 3, 3 },     /* VGA -> INTB */
        { 0, 0, 0, 0 },
        { 3, 2, 1, 4 },     /* slot 2 */
        { 2, 1, 4, 3 },     /* slot 3 */
        { 1, 4, 3, 2 },     /* slot 4 */
    };

    return tab[PCI_SLOT(d->devfn) & 7][pin & 3];
}

static void pcimt_reset(DeviceState *dev)
{
    SniPcimtState *s = SNI_PCIMT(dev);

    memset(s->asic, 0, sizeof(s->asic));
    memset(s->xbus, 0, sizeof(s->xbus));
    s->pend &= IT_EISA | IT_SCSI | IT_ETH | IT_PCI;
    pcimt_update_irq(s);
    timer_del(s->xtimer);
    memset(s->xtimer_count, 0, sizeof(s->xtimer_count));
    memset(s->xtimer_msb, 0, sizeof(s->xtimer_msb));
    qemu_irq_lower(s->xtimer_irq);
}

static void pcimt_realize(DeviceState *dev, Error **errp)
{
    SniPcimtState *s = SNI_PCIMT(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    memory_region_init(&s->pci_mem, OBJECT(s), "pci-mem", 4 * GiB);
    memory_region_init(&s->pci_io, OBJECT(s), "pci-io", RM_PCI_IO_SIZE);
    memory_region_init_io(&s->mem_bg, OBJECT(s), &bg_ops, (void *)"pcimem-bg",
                          "pci-mem-bg", 4 * GiB);
    memory_region_add_subregion_overlap(&s->pci_mem, 0, &s->mem_bg, -100);
    memory_region_init_io(&s->io_bg, OBJECT(s), &bg_ops, (void *)"pciio-bg",
                          "pci-io-bg", RM_PCI_IO_SIZE);
    memory_region_add_subregion_overlap(&s->pci_io, 0, &s->io_bg, -100);

    memory_region_init_alias(&s->isa_mem_win, OBJECT(s), "isa-mem-win",
                             &s->pci_mem, 0, RM_ISA_MEM_SIZE);
    memory_region_init_alias(&s->pci_mem_win, OBJECT(s), "pci-mem-win",
                             &s->pci_mem, RM_PCI_MEM_BASE, RM_PCI_MEM_SIZE);
    memory_region_init_alias(&s->pci_io_win, OBJECT(s), "pci-io-win",
                             &s->pci_io, 0, RM_PCI_IO_SIZE);
    sysbus_init_mmio(sbd, &s->isa_mem_win);
    sysbus_init_mmio(sbd, &s->pci_mem_win);
    sysbus_init_mmio(sbd, &s->pci_io_win);

    memory_region_init_io(&s->asic_mr, OBJECT(s), &asic_ops, s, "asic-pci",
                          ASIC_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->asic_mr);
    memory_region_init_io(&s->xbus_mr, OBJECT(s), &xbus_ops, s, "xbus-regs",
                          16 * 64 * KiB);
    sysbus_init_mmio(sbd, &s->xbus_mr);
    memory_region_init_io(&s->intack_mr, OBJECT(s), &intack_ops, s,
                          "pci-intack", 16 * MiB);
    sysbus_init_mmio(sbd, &s->intack_mr);

    memory_region_init_io(&s->cfg_mr, OBJECT(s), &pcicfg_ops, s, "pci-conf",
                          4);
    memory_region_add_subregion(&s->pci_io, 0xcfc, &s->cfg_mr);

    for (i = 0; i < ARRAY_SIZE(s->cpu_irq); i++) {
        sysbus_init_irq(sbd, &s->cpu_irq[i]);
    }
    sysbus_init_irq(sbd, &s->xtimer_irq);
    qdev_init_gpio_in(dev, pcimt_set_pend, 8);
    s->scsi_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pcimt_scsi_irq, s);
    s->xtimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, xtimer_tick, s);

    phb->bus = pci_register_root_bus(dev, "pci", pcimt_set_pend,
                                     pcimt_map_irq, s, &s->pci_mem,
                                     &s->pci_io, 0, 8, TYPE_PCI_BUS);
}

static const Property pcimt_props[] = {
    DEFINE_PROP_UINT8("csmsr", SniPcimtState, csmsr, 0x87),
    DEFINE_PROP_UINT8("csswitch", SniPcimtState, csswitch, 0x00),
    DEFINE_PROP_BOOL("xtimer", SniPcimtState, has_xtimer, false),
    DEFINE_PROP_UINT32("scsi-delay", SniPcimtState, scsi_delay_us,
                       SCSI_IRQ_DELAY_US),
};

static void pcimt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pcimt_realize;
    device_class_set_legacy_reset(dc, pcimt_reset);
    device_class_set_props(dc, pcimt_props);
    dc->user_creatable = false;
}

static const TypeInfo pcimt_info = {
    .name = TYPE_SNI_PCIMT,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(SniPcimtState),
    .class_init = pcimt_class_init,
};

/* Intel 82375EB PCEB PCI-EISA bridge, config space only */
#define TYPE_SNI_PCEB "i82375-pceb"

static void pceb_realize(PCIDevice *d, Error **errp)
{
}

static void pceb_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = pceb_realize;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = 0x0482;
    k->revision = 0x05;
    k->class_id = 0x0000;
    dc->user_creatable = false;
}

static const TypeInfo pceb_info = {
    .name = TYPE_SNI_PCEB,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = pceb_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* ESC config (0x22/0x23) and SuperIO config (0x2e/0x2f) index/data pairs */
typedef struct IdxRegs {
    const char *name;
    uint8_t idx;
    uint8_t regs[256];
} IdxRegs;

static uint64_t idx_read(void *opaque, hwaddr addr, unsigned size)
{
    IdxRegs *r = opaque;
    if (!addr) {
        return r->idx;
    }
    trace_sni_idx_read(r->name, r->idx, r->regs[r->idx]);
    return r->regs[r->idx];
}

static void idx_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IdxRegs *r = opaque;

    if (!addr) {
        r->idx = val;
        return;
    }
    trace_sni_idx_write(r->name, r->idx, val);
    r->regs[r->idx] = val;
}

static const MemoryRegionOps idx_ops = {
    .read = idx_read,
    .write = idx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void add_idx_regs(MemoryRegion *io, hwaddr base, const char *name)
{
    IdxRegs *r = g_new0(IdxRegs, 1);
    MemoryRegion *mr = g_new(MemoryRegion, 1);

    r->name = name;
    memory_region_init_io(mr, NULL, &idx_ops, r, name, 2);
    memory_region_add_subregion(io, base, mr);
}

static MemTxResult unassigned_read(void *opaque, hwaddr addr, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "bus error: read  0x%09" HWADDR_PRIx
                  "/%d\n", addr, size);
    *data = ~0ull;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult unassigned_write(void *opaque, hwaddr addr, uint64_t data,
                                    unsigned size, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_GUEST_ERROR, "bus error: write 0x%09" HWADDR_PRIx
                  "/%d = 0x%" PRIx64 "\n", addr, size, data);
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps unassigned_ops = {
    .read_with_attrs = unassigned_read,
    .write_with_attrs = unassigned_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static uint32_t mpa_routed(SniMpa *m)
{
    uint32_t cr = qatomic_read(&m->reg[MPA_CR >> 3]);

    return ((cr >> 3) & MPA_IT_LINES) | ((cr >> 2) & MPA_IT_INTERNAL);
}

/* Board lines reach every agent; each forwards what its CR routes there */
static void mpa_update(SniRm200MachineState *s)
{
    int i, pin;

    for (i = 0; i < s->ncpus; i++) {
        SniMpa *m = &s->mpa[i];
        uint32_t act = (s->mpa_lines | m->pend) & mpa_routed(m);

        for (pin = 0; pin < 5; pin++) {
            int level = !!(act & (1 << pin));

            if (pin == 4) {
                level |= !!(act & MPA_IT_INTERNAL);
            }
            qemu_set_irq(m->env->irq[2 + pin], level);
        }
    }
}

static void mpa_set_line(void *opaque, int n, int level)
{
    SniRm200MachineState *s = opaque;

    if (level) {
        qatomic_set(&s->mpa_lines, s->mpa_lines | 1 << n);
    } else {
        qatomic_set(&s->mpa_lines, s->mpa_lines & ~(1 << n));
    }
    mpa_update(s);
}

/* Single-threaded TCG runs all CPUs in turn on one host thread */
static bool mpa_round_robin(void)
{
    return !tcg_enabled() || !qemu_tcg_mttcg_enabled();
}

static uint32_t mpa_send(SniRm200MachineState *s, int from, uint32_t val)
{
    uint32_t status = 1;
    int i;

    for (i = 0; i < 4; i++) {
        SniMpa *m;

        if (!(val & (1 << i))) {
            continue;
        }
        trace_sni_mpa_msg(from, i, val);
        if (i >= s->ncpus) {
            status |= 2;
            continue;
        }
        m = &s->mpa[i];
        if ((val >> 24) == MPA_MSG_CLOCK) {
            qatomic_set(&m->pend, m->pend | MPA_IT_CLOCKMSG);
            continue;
        }
        if (m->queued == MPA_QUEUE) {
            qemu_log_mask(LOG_GUEST_ERROR, "mpagent: cpu%d queue full\n", i);
            continue;
        }
        m->queue[m->queued++] = (val & ~0xfu) | (1 << from);
        qatomic_set(&m->pend, m->pend | MPA_IT_MSG);
        /* Let round-robin TCG run the target next, as if it ran in parallel */
        if (i != from && current_cpu && mpa_round_robin()) {
            cpu_exit(current_cpu);
        }
    }
    return status;
}

static uint32_t mpa_receive(SniMpa *m)
{
    uint16_t pend = m->pend & ~MPA_IT_CLOCKMSG;

    if (m->queued) {
        m->msg = m->queue[0];
        memmove(m->queue, m->queue + 1, --m->queued * sizeof(m->queue[0]));
    }
    if (!m->queued) {
        pend &= ~MPA_IT_MSG;
    }
    qatomic_set(&m->pend, pend);
    return m->msg;
}

static hwaddr mpa_reg(hwaddr addr)
{
    return target_big_endian() ? addr ^ 4 : addr;
}

static int mpa_cpu(uint64_t *pc)
{
    if (!current_cpu) {
        *pc = 0;
        return 0;
    }
    *pc = cpu_env(current_cpu)->active_tc.PC;
    return current_cpu->cpu_index;
}

/*
 * Reads run without the BQL. The NT HAL's idle loop polls ITPEND and CR
 * a few hundred thousand times a second, and taking the BQL for every poll
 * starves the I/O thread. Only MSG_RECV changes state.
 */
static uint64_t mpa_read(void *opaque, hwaddr addr, unsigned size)
{
    SniRm200MachineState *s = opaque;
    uint64_t pc;
    int cpu = mpa_cpu(&pc);
    SniMpa *m = &s->mpa[cpu];
    uint32_t val, src;

    addr = mpa_reg(addr);
    switch (addr) {
    case MPA_CONF:
        val = (qatomic_read(&m->reg[addr >> 3]) & ~(3u << 27)) | (cpu << 27);
        break;
    case MPA_MSG_STATUS:
        /* PROM takes the CPU present mask from here after a self-ping */
        val = qatomic_read(&m->reg[addr >> 3]) | (((1u << s->ncpus) - 1) << 4);
        break;
    case MPA_ITPEND:
        src = qatomic_read(&s->mpa_lines) | qatomic_read(&m->pend);
        val = src | ((src & mpa_routed(m)) << 12);
        break;
    case MPA_MSG_RECV: {
        BQL_LOCK_GUARD();

        val = mpa_receive(m);
        mpa_update(s);
        break;
    }
    default:
        val = addr & 4 ? 0 : qatomic_read(&m->reg[addr >> 3]);
    }
    trace_sni_mpa_read(cpu, addr, val, pc);
    return val;
}

static void mpa_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SniRm200MachineState *s = opaque;
    uint64_t pc;
    int cpu = mpa_cpu(&pc);
    SniMpa *m = &s->mpa[cpu];

    BQL_LOCK_GUARD();
    addr = mpa_reg(addr);
    trace_sni_mpa_write(cpu, addr, val, pc);
    if (addr & 4) {
        return;
    }
    switch (addr) {
    case MPA_MSG_SEND:
        qatomic_set(&m->reg[MPA_MSG_STATUS >> 3], mpa_send(s, cpu, val));
        break;
    case MPA_MSG_STATUS:
    case MPA_ITPEND:
    case MPA_MSG_RECV:
        break;
    default:
        qatomic_set(&m->reg[addr >> 3], val);
    }
    mpa_update(s);
}

static const MemoryRegionOps mpa_ops = {
    .read = mpa_read,
    .write = mpa_write,
#if TARGET_BIG_ENDIAN
    .endianness = DEVICE_BIG_ENDIAN,
#else
    .endianness = DEVICE_LITTLE_ENDIAN,
#endif
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void mpa_reset(void *opaque)
{
    SniRm200MachineState *s = opaque;
    int i;

    for (i = 0; i < s->ncpus; i++) {
        memset(s->mpa[i].reg, 0, sizeof(s->mpa[i].reg));
        s->mpa[i].msg = 0;
        s->mpa[i].queued = 0;
        s->mpa[i].pend = 0;
    }
    mpa_update(s);

    /*
     * Every CPU clears the PROM's shared CPU table right after reset.
     * Round-robin TCG would let CPU 0 fill the table before the others even
     * start, so CPU 0 comes out of reset a little after them.
     */
    if (s->ncpus > 1) {
        env_cpu(s->mpa[0].env)->halted = 1;
        timer_mod(s->boot_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
        if (s->slice_us && mpa_round_robin()) {
            timer_mod(s->slice_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      s->slice_us * SCALE_US);
        }
    }
}

/*
 * Round-robin TCG runs each CPU for up to the next timer deadline (divided
 * by the CPU count), often milliseconds. NT expects a thread readied for an
 * idle CPU to start at once, win32k crashes during boot otherwise, so keep
 * the slices short.
 */
static void mpa_slice_timer(void *opaque)
{
    SniRm200MachineState *s = opaque;

    timer_mod(s->slice_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->slice_us * SCALE_US);
    qemu_cpu_kick(env_cpu(s->mpa[0].env));
}

static void mpa_start_cpu(CPUState *cs, run_on_cpu_data data)
{
    cs->halted = 0;
}

static void mpa_boot_timer(void *opaque)
{
    SniRm200MachineState *s = opaque;

    async_run_on_cpu(env_cpu(s->mpa[0].env), mpa_start_cpu, RUN_ON_CPU_NULL);
}

static void main_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void load_prom(MachineState *machine, MemoryRegion *prom)
{
    uint8_t *p = memory_region_get_ram_ptr(prom);
    g_autofree char *filename = NULL;
    g_autofree uint8_t *buf = NULL;
    gsize len;
    int i;

    if (!machine->firmware) {
        if (!qtest_enabled()) {
            error_report("no PROM image given, use -bios");
            exit(1);
        }
        return;
    }
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!filename || !g_file_get_contents(filename, (char **)&buf, &len,
                                          NULL)) {
        error_report("could not load PROM '%s'", machine->firmware);
        exit(1);
    }
    if (len > PROM_SIZE) {
        len = PROM_SIZE;
    }
    memset(p, 0xff, PROM_SIZE);
    memcpy(p, buf, len);

    /*
     * Flash dumps hold 32-bit words in little-endian byte order. A big-endian
     * CPU fetches them unchanged, so swap if the reset vector reads as "j".
     */
    if (target_big_endian() && (p[3] >> 2) == 2) {
        for (i = 0; i < PROM_SIZE; i += 4) {
            stl_be_p(p + i, ldl_le_p(p + i));
        }
    }
}

/*
 * Memory lives at 0x20000000: 4 banks 128MB apart, each with two 64MB sides.
 * Unpopulated space reads as all-ones without a bus error (the PROM sizes
 * memory by probing; 5.02xx PROMs also probe the larger SIMM layouts up
 * to 4GB). The first 256MB are also visible at 0.
 */
#define MEM_BASE        0x20000000
#define MEM_SPACE       (512 * MiB)
#define MEM_SIDE        (64 * MiB)
#define MEM_LOW         (256 * MiB)
#define MEM_TOP         0x100000000ull

static uint64_t nomem_read(void *opaque, hwaddr addr, unsigned size)
{
    return ~0ull;
}

static void nomem_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
}

static const MemoryRegionOps nomem_ops = {
    .read = nomem_read,
    .write = nomem_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void rm200_init_memory(MachineState *machine, MemoryRegion *sysmem)
{
    MemoryRegion *space = g_new(MemoryRegion, 1);
    MemoryRegion *nomem = g_new(MemoryRegion, 1);
    MemoryRegion *nomem_hi = g_new(MemoryRegion, 1);
    MemoryRegion *low = g_new(MemoryRegion, 1);
    uint64_t left = machine->ram_size;
    uint64_t off = 0;
    int side;

    if (left > MEM_SPACE || left % (16 * MiB)) {
        error_report("RAM size must be a multiple of 16MB, at most 512MB");
        exit(1);
    }
    memory_region_init(space, NULL, "rm200.memspace", MEM_SPACE);
    memory_region_init_io(nomem, NULL, &nomem_ops, NULL, "rm200.nomem",
                          MEM_SPACE);
    memory_region_add_subregion_overlap(space, 0, nomem, -1);
    for (side = 0; side < MEM_SPACE / MEM_SIDE && left; side++) {
        uint64_t sz = pow2floor(MIN(left, MEM_SIDE));
        MemoryRegion *mr = g_new(MemoryRegion, 1);

        memory_region_init_alias(mr, NULL, "rm200.dimm", machine->ram, off,
                                 sz);
        memory_region_add_subregion(space, side * MEM_SIDE, mr);
        off += sz;
        left -= sz;
    }
    memory_region_add_subregion(sysmem, MEM_BASE, space);
    memory_region_init_io(nomem_hi, NULL, &nomem_ops, NULL, "rm200.nomem-hi",
                          MEM_TOP - MEM_BASE - MEM_SPACE);
    memory_region_add_subregion_overlap(sysmem, MEM_BASE + MEM_SPACE,
                                        nomem_hi, -1);
    memory_region_init_alias(low, NULL, "rm200.lowmem", space, 0, MEM_LOW);
    memory_region_add_subregion(sysmem, 0, low);
}

/*
 * IDPROM in the first 2KB of NVRAM. 0x22 = 0xff selects the long format,
 * 0x50 motherboard module number, 0x8b motherboard serial number (18
 * alphanumerics, checked by POST), 0x7ff makes the 2KB sum to zero.
 */
#define IDPROM_SIZE     0x800

static void rm200_init_idprom(uint8_t *p)
{
    /* Little-endian (NT) mode sees the byte-wide NVRAM at address ^ 3 */
    int x = target_big_endian() ? 0 : 3;
    uint8_t id[IDPROM_SIZE] = { 0 };
    uint8_t sum = 0;
    int i;

    if (!buffer_is_zero(p, NVRAM_SIZE)) {
        return;
    }
    /* SINIX firmware wants the long-format flag, NT firmware 5 digits */
    if (target_big_endian()) {
        id[0x22] = 0xff;
    } else {
        memcpy(id + 0x22, "00000", 5);
    }
    memcpy(id + 0x50, "QEMU RM200C", 11);
    memcpy(id + 0x8b, "QEMU00000000000001", 18);
    for (i = 0; i < IDPROM_SIZE - 1; i++) {
        sum += id[i];
    }
    id[IDPROM_SIZE - 1] = -sum;
    for (i = 0; i < IDPROM_SIZE; i++) {
        p[i ^ x] = id[i];
    }
}

#ifdef CONFIG_POSIX
static void rm200_nvram_file(const char *path, MemoryRegion *mr)
{
    int fd = qemu_create(path, O_RDWR | O_BINARY, 0644, &error_fatal);

    if (ftruncate(fd, NVRAM_SIZE) < 0) {
        error_report("could not size NVRAM file '%s'", path);
        exit(1);
    }
    memory_region_init_ram_from_fd(mr, NULL, "rm200.nvram", NVRAM_SIZE,
                                   RAM_SHARED, fd, 0, &error_fatal);
}
#else
typedef struct {
    Notifier exit;
    const char *path;
    void *ptr;
} NvramFile;

static void rm200_nvram_save(Notifier *n, void *data)
{
    NvramFile *nf = container_of(n, NvramFile, exit);

    g_file_set_contents(nf->path, nf->ptr, NVRAM_SIZE, NULL);
}

/* No shared file mappings: load the file now, write it back at exit */
static void rm200_nvram_file(const char *path, MemoryRegion *mr)
{
    NvramFile *nf = g_new0(NvramFile, 1);
    g_autofree gchar *buf = NULL;
    gsize len;

    memory_region_init_ram(mr, NULL, "rm200.nvram", NVRAM_SIZE, &error_fatal);
    nf->exit.notify = rm200_nvram_save;
    nf->path = path;
    nf->ptr = memory_region_get_ram_ptr(mr);
    if (g_file_get_contents(path, &buf, &len, NULL)) {
        memcpy(nf->ptr, buf, MIN(len, NVRAM_SIZE));
    }
    qemu_add_exit_notifier(&nf->exit);
}
#endif

static void rm200_init_nvram(const char *path, MemoryRegion *mr)
{
    if (path) {
        rm200_nvram_file(path, mr);
    } else {
        memory_region_init_ram(mr, NULL, "rm200.nvram", NVRAM_SIZE,
                               &error_fatal);
    }
    rm200_init_idprom(memory_region_get_ram_ptr(mr));
}

static void sni_rm200_init(MachineState *machine)
{
    SniRm200MachineState *rm = SNI_RM200_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *prom = g_new(MemoryRegion, 1);
    MemoryRegion *nvram = g_new(MemoryRegion, 1);
    MemoryRegion *nvram_alias;
    MemoryRegion *cache_repl = g_new(MemoryRegion, 1);
    MemoryRegion *unassigned = g_new(MemoryRegion, 1);
    MemoryRegion *mpa_mr;
    DeviceState *dev;
    SysBusDevice *sbd;
    SniPcimtState *pcimt;
    PCIBus *pci_bus;
    ISABus *isa_bus;
    ISADevice *pit, *isadev;
    qemu_irq *i8259, *lines;
    DriveInfo *fds[MAX_FD];
    Clock *cpuclk;
    MIPSCPU *cpu;
    CPUMIPSState *env;
    bool has_mpa;
    int i;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, 133333333);
    rm->ncpus = machine->smp.cpus;
    for (i = 0; i < rm->ncpus; i++) {
        cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk,
                                         target_big_endian());
        qemu_register_reset(main_cpu_reset, cpu);
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        rm->mpa[i].env = &cpu->env;
    }
    env = rm->mpa[0].env;

    /* Config.SC clear: a CPU module with secondary cache and an MP agent */
    has_mpa = !(env->CP0_Config0 & (1 << 17));
    if (rm->ncpus > 1 && !has_mpa) {
        error_report("more than one CPU needs CPU modules with an MP agent, "
                     "use -cpu R4400");
        exit(1);
    }

    memory_region_init_io(unassigned, NULL, &unassigned_ops, NULL,
                          "unassigned", 1ull << 36);
    memory_region_add_subregion_overlap(sysmem, 0, unassigned, -1000);

    rm200_init_memory(machine, sysmem);

    memory_region_init_rom(prom, NULL, "rm200.prom", PROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, PROM_BASE, prom);
    load_prom(machine, prom);

    rm200_init_nvram(SNI_RM200_MACHINE(machine)->nvram_file, nvram);
    memory_region_add_subregion(sysmem, NVRAM_BASE, nvram);
    for (i = 1; i < 512 * KiB / NVRAM_SIZE; i++) {
        nvram_alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(nvram_alias, NULL, "rm200.nvram-mirror",
                                 nvram, 0, NVRAM_SIZE);
        memory_region_add_subregion(sysmem, NVRAM_BASE + i * NVRAM_SIZE,
                                    nvram_alias);
    }

    memory_region_init_ram(cache_repl, NULL, "rm200.cache-repl",
                           CACHE_REPL_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, CACHE_REPL_BASE, cache_repl);

    /* ASIC PCI, board registers, PCI host */
    dev = qdev_new(TYPE_SNI_PCIMT);
    qdev_prop_set_uint8(dev, "csmsr", rm->csmsr);
    qdev_prop_set_uint8(dev, "csswitch", rm->csswitch);
    qdev_prop_set_bit(dev, "xtimer", has_mpa);
    qdev_prop_set_uint32(dev, "scsi-delay", rm->scsi_delay_us);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    pcimt = SNI_PCIMT(dev);
    sysbus_mmio_map(sbd, 0, RM_ISA_MEM_BASE);
    sysbus_mmio_map(sbd, 1, RM_PCI_MEM_BASE);
    sysbus_mmio_map(sbd, 2, RM_PCI_IO_BASE);
    sysbus_mmio_map(sbd, 3, ASIC_BASE);
    sysbus_mmio_map(sbd, 4, XBUS_BASE);
    sysbus_mmio_map_overlap(sbd, 5, INTACK_BASE, 1);
    if (has_mpa) {
        /* Board Int0-Int4 and the extra timer (on Int2) go through MP agents */
        lines = qemu_allocate_irqs(mpa_set_line, rm, 5);
        for (i = 0; i < 5; i++) {
            if (i != 2) {
                sysbus_connect_irq(sbd, i, lines[i]);
            }
        }
        sysbus_connect_irq(sbd, 5, lines[2]);
        mpa_mr = g_new(MemoryRegion, 1);
        memory_region_init_io(mpa_mr, OBJECT(machine), &mpa_ops, rm,
                              "rm200.mpagent", MPA_SIZE);
        memory_region_enable_lockless_io(mpa_mr);
        memory_region_add_subregion(sysmem, MPA_BASE, mpa_mr);
        rm->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mpa_boot_timer, rm);
        rm->slice_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mpa_slice_timer,
                                       rm);
        qemu_register_reset(mpa_reset, rm);
    } else {
        for (i = 0; i < 5; i++) {
            sysbus_connect_irq(sbd, i, env->irq[2 + i]);
        }
    }
    pci_bus = PCI_HOST_BRIDGE(dev)->bus;

    /* EISA side of the Intel 82374EB ESC */
    isa_bus = isa_bus_new(NULL, &pcimt->pci_mem, &pcimt->pci_io,
                          &error_abort);
    i8259 = i8259_init(isa_bus, qdev_get_gpio_in(dev, 5));
    isa_bus_register_input_irqs(isa_bus, i8259);
    i8257_dma_init(OBJECT(machine), isa_bus, 0);
    pit = i8254_pit_init(isa_bus, 0x40, 0, NULL);
    isadev = isa_new(TYPE_PC_SPEAKER);
    object_property_set_link(OBJECT(isadev), "pit", OBJECT(pit), &error_fatal);
    isa_realize_and_unref(isadev, isa_bus, &error_fatal);
    mc146818_rtc_init(isa_bus, 1900, NULL);
    add_idx_regs(&pcimt->pci_io, 0x22, "esc-cfg");

    /* National SuperIO: UARTs, LPT, FDC; 8042 keyboard controller */
    add_idx_regs(&pcimt->pci_io, 0x2e, "superio-cfg");
    for (i = 0; i < 2; i++) {
        isadev = isa_new(TYPE_ISA_SERIAL);
        qdev_prop_set_uint32(DEVICE(isadev), "index", i);
        qdev_prop_set_chr(DEVICE(isadev), "chardev", serial_hd(i));
        isa_realize_and_unref(isadev, isa_bus, &error_fatal);
    }
    isadev = isa_new(TYPE_ISA_PARALLEL);
    qdev_prop_set_uint32(DEVICE(isadev), "iobase", 0x3bc);
    qdev_prop_set_chr(DEVICE(isadev), "chardev", parallel_hds[0] ?:
                      qemu_chr_new("rm200.lpt", "null", NULL));
    isa_realize_and_unref(isadev, isa_bus, &error_fatal);
    isa_create_simple(isa_bus, TYPE_I8042);
    for (i = 0; i < MAX_FD; i++) {
        fds[i] = drive_get(IF_FLOPPY, 0, i);
    }
    isadev = isa_new(TYPE_ISA_FDC);
    isa_realize_and_unref(isadev, isa_bus, &error_fatal);
    isa_fdc_init_drives(isadev, fds);

    /* PCI devices */
    pci_create_simple(pci_bus, PCI_DEVFN(0, 0), TYPE_SNI_PCEB);
    dev = DEVICE(pci_create_simple(pci_bus, PCI_DEVFN(1, 0), "lsi53c810"));
    lsi53c8xx_handle_legacy_cmdline(dev);
    pci_init_nic_in_slot(pci_bus, "pcnet", NULL, "2");
    pci_create_simple(pci_bus, PCI_DEVFN(3, 0), "cirrus-vga-gd5434");
    pci_init_nic_devices(pci_bus, "pcnet");
}

static char *rm200_get_nvram(Object *obj, Error **errp)
{
    return g_strdup(SNI_RM200_MACHINE(obj)->nvram_file);
}

static void rm200_set_nvram(Object *obj, const char *value, Error **errp)
{
    SniRm200MachineState *s = SNI_RM200_MACHINE(obj);

    g_free(s->nvram_file);
    s->nvram_file = g_strdup(value);
}

/* No x86 option ROMs; SINIX sets drive mode pages QEMU cannot apply */
static GlobalProperty rm200_props[] = {
    { "pcnet", "romfile", "" },
    { "cirrus-vga", "romfile", "" },  /* also covers cirrus-vga-gd5434 */
    { "VGA", "romfile", "" },
    { "scsi-hd", "quirk_mode_select_ignore", "on" },
    { "scsi-cd", "quirk_mode_select_ignore", "on" },
};

static void sni_rm200_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    compat_props_add(mc->compat_props, rm200_props, ARRAY_SIZE(rm200_props));
    mc->desc = "Siemens Nixdorf RM200C (PCI)";
    mc->default_nic = "pcnet";
    mc->init = sni_rm200_init;
    mc->block_default_type = IF_SCSI;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R4700");
    mc->default_ram_id = "rm200.ram";
    mc->default_ram_size = 64 * MiB;
    object_class_property_add_str(oc, "nvram", rm200_get_nvram,
                                  rm200_set_nvram);
    object_class_property_set_description(oc, "nvram",
                                          "File backing the 32KB NVRAM");
}

static void sni_rm200_instance_init(Object *obj)
{
    SniRm200MachineState *s = SNI_RM200_MACHINE(obj);

    s->csmsr = 0x87;
    s->scsi_delay_us = SCSI_IRQ_DELAY_US;
    object_property_add_uint32_ptr(obj, "scsi-delay", &s->scsi_delay_us,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "scsi-delay",
                                    "Delay of SCSI completion interrupts, "
                                    "in microseconds (0 = none)");
}

static const TypeInfo sni_rm200_type = {
    .name = TYPE_SNI_RM200_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SniRm200MachineState),
    .instance_init = sni_rm200_instance_init,
    .class_init = sni_rm200_class_init,
};

/*
 * PCI minitower: same ASIC, CSMSR bit 7 clear, up to two R4400 modules.
 * The MP agents, PROM and NT HAL also run four (NT 4.0 Server uses them).
 * Its PROM reads "temperature normal" as CSSWITCH bit 5 set.
 */
static void sni_rm300_instance_init(Object *obj)
{
    SniRm200MachineState *s = SNI_RM200_MACHINE(obj);

    s->csmsr = 0x0f;
    s->csswitch = 0x20;
    s->slice_us = 100;
    object_property_add_uint32_ptr(obj, "slice", &s->slice_us,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "slice",
                                    "Longest CPU time slice with -smp > 1 "
                                    "and single-threaded TCG, in "
                                    "microseconds (0 = QEMU default)");
}

static void sni_rm300_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    /* Every machine class starts with empty compat_props */
    compat_props_add(mc->compat_props, rm200_props, ARRAY_SIZE(rm200_props));
    mc->desc = "Siemens Nixdorf RM300-C (PCI, up to 4 CPUs)";
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R4400");
    mc->max_cpus = RM_MAX_CPUS;
}

static const TypeInfo sni_rm300_type = {
    .name = TYPE_SNI_RM300_MACHINE,
    .parent = TYPE_SNI_RM200_MACHINE,
    .instance_init = sni_rm300_instance_init,
    .class_init = sni_rm300_class_init,
};

static void sni_rm200_register_types(void)
{
    type_register_static(&pcimt_info);
    type_register_static(&pceb_info);
    type_register_static(&sni_rm200_type);
    type_register_static(&sni_rm300_type);
}

type_init(sni_rm200_register_types)
