/*
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-bridge/dec_pci.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

static void dec_pci_bridge_realize(PCIDevice *dev, Error **errp)
{
    DECPCIBridge *br = DEC_PCI_BRIDGE(dev);

    pci_bridge_initfn(dev, TYPE_PCI_BUS);
    pci_set_word(dev->config + PCI_COMMAND, PCI_COMMAND_MEMORY);
    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MEDIUM);

    pci_bridge_update_mappings(PCI_BRIDGE(br));
}

static const VMStateDescription vmstate_dec_pci_bridge = {
    .name = "dec-pci-bridge",
    .priority = MIG_PRI_PCI_BUS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, PCIBridge),
        VMSTATE_END_OF_LIST()
    }
};

static void dec_pci_bridge_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_DEC;
    k->device_id = PCI_DEVICE_ID_DEC_21154;
    k->revision = 0x02;
    k->realize = dec_pci_bridge_realize;
    k->exit = pci_bridge_exitfn;
    k->config_write = pci_bridge_write_config;
    dc->desc = "DEC 21154 PCI-to-PCI Bridge";
    dc->vmsd = &vmstate_dec_pci_bridge;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    device_class_set_legacy_reset(dc, pci_bridge_reset);
}

static const TypeInfo dec_pci_bridge_info = {
    .name          = TYPE_DEC_PCI_BRIDGE,
    .parent        = TYPE_PCI_BRIDGE,
    .class_init    = dec_pci_bridge_class_init,
    .instance_size = sizeof(DECPCIBridge),
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void dec_pci_register_types(void)
{
    type_register_static(&dec_pci_bridge_info);
}

type_init(dec_pci_register_types)
