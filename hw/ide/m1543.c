/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ALi M1543c (Aladdin IV) IDE controller emulation.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/range.h"
#include "hw/ide/pci.h"
#include "hw/core/irq.h"
#include "hw/southbridge/m1543.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "ide-internal.h"
#include "trace.h"

static uint64_t bmdma_read(void *opaque, hwaddr addr,
                           unsigned size)
{
    BMDMAState *bm = opaque;
    uint32_t val;

    if (size != 1) {
        return ((uint64_t)1 << (size * 8)) - 1;
    }

    switch (addr & 3) {
    case 0:
        val = bm->cmd;
        break;
    case 2:
        val = bm->status;
        break;
    default:
        val = 0xff;
        break;
    }

    trace_bmdma_read_m1543(addr, val);
    return val;
}

static void bmdma_write(void *opaque, hwaddr addr,
                        uint64_t val, unsigned size)
{
    BMDMAState *bm = opaque;

    if (size != 1) {
        return;
    }

    trace_bmdma_write_m1543(addr, val);
    switch (addr & 3) {
    case 0:
        bmdma_cmd_writeb(bm, val);
        break;
    case 2:
        bmdma_status_writeb(bm, val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps via_bmdma_ops = {
    .read = bmdma_read,
    .write = bmdma_write,
};

static void bmdma_setup_bar(PCIIDEState *d)
{
    int i;

    memory_region_init(&d->bmdma_bar, OBJECT(d), "m1543-bmdma-container", 16);
    for (i = 0; i < ARRAY_SIZE(d->bmdma); i++) {
        BMDMAState *bm = &d->bmdma[i];

        memory_region_init_io(&bm->extra_io, OBJECT(d), &via_bmdma_ops, bm,
                              "m1543-bmdma", 4);
        memory_region_add_subregion(&d->bmdma_bar, i * 8, &bm->extra_io);
        memory_region_init_io(&bm->addr_ioport, OBJECT(d),
                              &bmdma_addr_ioport_ops, bm, "bmdma", 4);
        memory_region_add_subregion(&d->bmdma_bar, (i * 8) + 4,
                                    &bm->addr_ioport);
    }
}

static void m1543_pci_ide_update_mode(PCIIDEState *s)
{
    PCIDevice *d = PCI_DEVICE(s);

    /*
     * The interrupt pin cannot be hidden, as this will cause Tru64 to fail
     * to boot with the following errors (which results in an unhelpful and
     * misleading "init_rootdev: boot device translation failed" panic):
     *
     * ata0 at pci2 slot 15
     * ata0: ACER M1543C
     * pci_intline_handler_add: invalid INTx value: 0
     *  slot = 15, busp = 0xfffffc003fd74780
     * ata_probe: handler_add failed
     * cam_logger: SCSI event packet
     * cam_logger: No associated bus target lun
     * ata_attach: probe failed
     */

    pci_ide_update_mode(s);
    pci_config_set_interrupt_pin(d->config, 1);
}

static inline bool m1543_ide_legacy_mode(PCIDevice *pd)
{
    return (pci_get_byte(pd->config + PCI_CLASS_PROG) & 0x0f) == 0x0a;
}

static void m1543_ide_set_irq(void *opaque, int n, int level)
{
    PCIIDEState *s = opaque;

    qemu_set_irq(s->isa_irq[n], level);
}

static uint32_t m1543_ide_cfg_read(PCIDevice *pd, uint32_t addr, int len)
{
    uint32_t val = pci_default_read_config(pd, addr, len);
    uint8_t *pci_conf = pd->config;
    unsigned i;

    /* Fix up the base addresses in legacy mode. OpenVMS needs this. */
    if (m1543_ide_legacy_mode(pd) && val == PCI_BASE_ADDRESS_SPACE_IO) {
        switch (addr) {
        case PCI_BASE_ADDRESS_0:
            val = 0x01f0 | PCI_BASE_ADDRESS_SPACE_IO;
            break;
        case PCI_BASE_ADDRESS_1:
            val = 0x03f4 | PCI_BASE_ADDRESS_SPACE_IO;
            break;
        case PCI_BASE_ADDRESS_2:
            val = 0x0170 | PCI_BASE_ADDRESS_SPACE_IO;
            break;
        case PCI_BASE_ADDRESS_3:
            val = 0x0374 | PCI_BASE_ADDRESS_SPACE_IO;
            break;
        }
    }

    /* Mask base addresses when in compatibility mode, if required. */
    if (pci_conf[0x53] & 0x08) {
        if (ranges_overlap(addr, len, PCI_BASE_ADDRESS_0, 16) &&
            m1543_ide_legacy_mode(pd)) {
            for (i = addr; i < addr + len; i++) {
                if (i >= PCI_BASE_ADDRESS_0 && i < PCI_BASE_ADDRESS_0 + 16) {
                    val &= ~(0xffULL << ((i - addr) << 3));
                }
            }
        }
    }

    return val;
}

static void m1543_ide_cfg_write(PCIDevice *pd, uint32_t addr,
                              uint32_t val, int len)
{
    PCIIDEState *d = PCI_IDE(pd);

    if (ranges_overlap(addr, len, PCI_BASE_ADDRESS_0, 16) &&
        m1543_ide_legacy_mode(pd)) {
        return;
    }

    pci_default_write_config(pd, addr, val, len);
    if (range_covers_byte(addr, len, PCI_CLASS_PROG)) {
        m1543_pci_ide_update_mode(d);
    }
}

static void m1543_ide_reset(DeviceState *dev)
{
    PCIIDEState *d = PCI_IDE(dev);
    PCIDevice *pd = PCI_DEVICE(dev);
    uint8_t *pci_conf = pd->config;
    int i;

    for (i = 0; i < ARRAY_SIZE(d->bus); i++) {
        ide_bus_reset(&d->bus[i], IDE_RESET_HARDWARE);
    }

    pci_config_set_prog_interface(pci_conf, 0x8a); /* legacy mode */
    m1543_pci_ide_update_mode(d);

    pci_set_word(pci_conf + PCI_COMMAND,
                 PCI_COMMAND_FAST_BACK | PCI_COMMAND_WAIT);
    pci_set_word(pci_conf + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);
    pci_set_byte(pci_conf + PCI_MIN_GNT, 0x02);
    pci_set_byte(pci_conf + PCI_MAX_LAT, 0x04);

    /* Ultra DMA Test */
    pci_set_byte(pci_conf + 0x4b, 0x4a);
    /* Reserved */
    pci_set_word(pci_conf + 0x4e, 0x1aba);
    /* Class Code Attributes */
    pci_set_byte(pci_conf + 0x50, 0x01);
    /* Ultra DMA + FIFO Settings */
    pci_set_long(pci_conf + 0x54, 0x44445555);
    /* IDE frequency */
    pci_set_byte(pci_conf + 0x78, 0x21);
}

static void m1543_ide_realize(PCIDevice *pd, Error **errp)
{
    PCIIDEState *d = PCI_IDE(pd);
    DeviceState *ds = DEVICE(pd);
    unsigned i;

    memory_region_init_io(&d->data_bar[0], OBJECT(d), &pci_ide_data_le_ops,
                          &d->bus[0], "m1543-ide0-data", 8);
    pci_register_bar(pd, 0, PCI_BASE_ADDRESS_SPACE_IO, &d->data_bar[0]);

    memory_region_init_io(&d->cmd_bar[0], OBJECT(d), &pci_ide_cmd_le_ops,
                          &d->bus[0], "m1543-ide0-cmd", 4);
    pci_register_bar(pd, 1, PCI_BASE_ADDRESS_SPACE_IO, &d->cmd_bar[0]);

    memory_region_init_io(&d->data_bar[1], OBJECT(d), &pci_ide_data_le_ops,
                          &d->bus[1], "m1543-ide1-data", 8);
    pci_register_bar(pd, 2, PCI_BASE_ADDRESS_SPACE_IO, &d->data_bar[1]);

    memory_region_init_io(&d->cmd_bar[1], OBJECT(d), &pci_ide_cmd_le_ops,
                          &d->bus[1], "m1543-ide1-cmd", 4);
    pci_register_bar(pd, 3, PCI_BASE_ADDRESS_SPACE_IO, &d->cmd_bar[1]);

    bmdma_setup_bar(d);
    pci_register_bar(pd, 4, PCI_BASE_ADDRESS_SPACE_IO, &d->bmdma_bar);

    qdev_init_gpio_in(ds, m1543_ide_set_irq, ARRAY_SIZE(d->bus));
    for (i = 0; i < ARRAY_SIZE(d->bus); i++) {
        ide_bus_init(&d->bus[i], sizeof(d->bus[i]), ds, i, MAX_IDE_DEVS);
        ide_bus_init_output_irq(&d->bus[i], qdev_get_gpio_in(ds, i));

        bmdma_init(&d->bus[i], &d->bmdma[i], d);
        ide_bus_register_restart_cb(&d->bus[i]);
    }

    /* Initialize the write masks. */
    pci_set_byte(pd->wmask + PCI_CLASS_PROG, 0x05);
    pci_set_byte(pd->wmask + 0x4f, 0x00);
    pci_set_long(pd->wmask + 0x50, 0x8bff7f2a);
    pci_set_long(pd->wmask + 0x58, 0x7f7f7f03);
    pci_set_long(pd->wmask + 0x5c, 0x7f7f7f03);
}

static void m1543_ide_exitfn(PCIDevice *pd)
{
    PCIIDEState *d = PCI_IDE(pd);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(d->bmdma); ++i) {
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].extra_io);
        memory_region_del_subregion(&d->bmdma_bar, &d->bmdma[i].addr_ioport);
    }
}

static void m1543_ide_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, m1543_ide_reset);
    dc->vmsd = &vmstate_ide_pci;
    dc->hotpluggable = false;
    /* Reason: only works as function of the M1543 chipset */
    dc->user_creatable = false;
    k->config_read = m1543_ide_cfg_read;
    k->config_write = m1543_ide_cfg_write;
    k->realize = m1543_ide_realize;
    k->exit = m1543_ide_exitfn;
    k->vendor_id = PCI_VENDOR_ID_AL;
    k->device_id = PCI_DEVICE_ID_AL_M5229;
    k->revision = 0xc1;
    k->class_id = PCI_CLASS_STORAGE_IDE;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo m1543_ide_info = {
    .name          = TYPE_M1543_IDE,
    .parent        = TYPE_PCI_IDE,
    .class_init    = m1543_ide_class_init,
};

static void m1543_ide_register_types(void)
{
    type_register_static(&m1543_ide_info);
}

type_init(m1543_ide_register_types)
