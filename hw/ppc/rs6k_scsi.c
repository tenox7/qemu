/*
 * IBM RS/6000 7006 planar SCSI (NCR 53C720, MCA id 0x8fba, slot 13)
 *
 * The 53C720 has the register set of the 53C810, so the chip is QEMU's
 * LSI 53C810 on a private PCI segment: its registers appear at Micro
 * Channel I/O 0x80, it interrupts on level 4 and its bus-master DMA goes
 * through the IOCC TCW table. The planar status word at 0x7c reads bit 0
 * set while the chip is not interrupting and bits 4-7 as error flags.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/ppc/rs6k_iocc.h"
#include "hw/ppc/rs6k_scsi.h"

OBJECT_DECLARE_SIMPLE_TYPE(Rs6kSCSIState, RS6K_SCSI)

#define SCSI_SLOT       13
#define SCSI_ID         0x8fba
#define SCSI_IO_ADDR    0x80
#define SCSI_IO_SIZE    0x60
#define SCSI_STATUS     0x7c
#define SCSI_NO_INTR    0x1
#define SCSI_LEVEL      4
#define LSI_MMIO_BAR    1

struct Rs6kSCSIState {
    PCIHostState parent_obj;

    Rs6kIOCCState *iocc;
    PCIDevice *lsi;
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    MemoryRegion regs;
    MemoryRegion status;
    qemu_irq irq;
    bool irq_level;
};

static void scsi_set_irq(void *opaque, int n, int level)
{
    Rs6kSCSIState *s = opaque;

    s->irq_level = level;
    qemu_set_irq(s->irq, level);
}

static uint64_t scsi_status_read(void *opaque, hwaddr addr, unsigned size)
{
    Rs6kSCSIState *s = opaque;

    return s->irq_level ? 0 : SCSI_NO_INTR;
}

static void scsi_status_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
}

static const MemoryRegionOps scsi_status_ops = {
    .read = scsi_status_read,
    .write = scsi_status_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int scsi_map_irq(PCIDevice *dev, int pin)
{
    return 0;
}

static AddressSpace *scsi_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    Rs6kSCSIState *s = opaque;

    return &s->iocc->dma_as;
}

static const PCIIOMMUOps scsi_iommu_ops = {
    .get_address_space = scsi_dma_as,
};

static void rs6k_scsi_realize(DeviceState *dev, Error **errp)
{
    Rs6kSCSIState *s = RS6K_SCSI(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Object *obj = OBJECT(dev);

    if (!s->iocc) {
        error_setg(errp, "iocc link not set");
        return;
    }
    s->irq = qdev_get_gpio_in(DEVICE(s->iocc), SCSI_LEVEL);
    memory_region_init(&s->pci_mem, obj, "scsi.pci-mem", 4 * GiB);
    memory_region_init(&s->pci_io, obj, "scsi.pci-io", 64 * KiB);
    phb->bus = pci_register_root_bus(dev, "scsi-pci", scsi_set_irq,
                                     scsi_map_irq, s, &s->pci_mem,
                                     &s->pci_io, 0, 1, TYPE_PCI_BUS);
    pci_setup_iommu(phb->bus, &scsi_iommu_ops, s);

    s->lsi = pci_new(0, "lsi53c810");
    if (!pci_realize_and_unref(s->lsi, phb->bus, errp)) {
        return;
    }
    memory_region_init_alias(&s->regs, obj, "scsi.regs",
                             s->lsi->io_regions[LSI_MMIO_BAR].memory, 0,
                             SCSI_IO_SIZE);
    memory_region_add_subregion(&s->iocc->bus_io, SCSI_IO_ADDR, &s->regs);
    memory_region_init_io(&s->status, obj, &scsi_status_ops, s, "scsi.status",
                          4);
    memory_region_add_subregion(&s->iocc->bus_io, SCSI_STATUS, &s->status);
    rs6k_iocc_set_slot(s->iocc, SCSI_SLOT, SCSI_ID, NULL, s);
}

/* There is no PCI configuration on the MCA side: always a bus master */
static void rs6k_scsi_reset_exit(Object *obj, ResetType type)
{
    Rs6kSCSIState *s = RS6K_SCSI(obj);

    pci_default_write_config(s->lsi, PCI_COMMAND, PCI_COMMAND_MASTER, 2);
}

static const Property rs6k_scsi_properties[] = {
    DEFINE_PROP_LINK("iocc", Rs6kSCSIState, iocc, TYPE_RS6K_IOCC,
                     Rs6kIOCCState *),
};

static void rs6k_scsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.exit = rs6k_scsi_reset_exit;
    dc->realize = rs6k_scsi_realize;
    device_class_set_props(dc, rs6k_scsi_properties);
    dc->user_creatable = false;
}

static const TypeInfo rs6k_scsi_types[] = {
    {
        .name = TYPE_RS6K_SCSI,
        .parent = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(Rs6kSCSIState),
        .class_init = rs6k_scsi_class_init,
    },
};

DEFINE_TYPES(rs6k_scsi_types)
