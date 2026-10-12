/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DEC 21272 ("Tsunami") PCI host bridge emulation.
 */

#ifndef _ALPHA_TSUNAMI_H
#define _ALPHA_TSUNAMI_H

#include "hw/alpha/tigbus.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "qom/object.h"

#define TYPE_TSUNAMI_CHIPSET "tsunami-chipset"
OBJECT_DECLARE_SIMPLE_TYPE(TsunamiState, TSUNAMI_CHIPSET)

int tsunami_pci_root_bus_index(PCIBus *b);
PCIBus *tsunami_get_pci_bus(DeviceState *dev, int n);
TigBus *tsunami_get_tig_bus(DeviceState *dev);

/*
 * Tsunami base address definitions.
 */

#define TSUNAMI_MEM_BASE                0x00000000000
#define TSUNAMI_IO_BASE                 0x80000000000

#define TSUNAMI_TIGBUS                  0x80100000000
#define TSUNAMI_CCHIP                   0x801a0000000
#define TSUNAMI_DCHIP                   0x801b0000000
#define TSUNAMI_PCHIP0                  0x80180000000
#define TSUNAMI_PCHIP1                  0x80380000000

#define TSUNAMI_PCI0_MEM                0x80000000000
#define TSUNAMI_PCI0_INTACK             0x801f8000000
#define TSUNAMI_PCI0_IO                 0x801fc000000
#define TSUNAMI_PCI0_CONFIG             0x801fe000000
#define TSUNAMI_PCI1_MEM                0x80200000000
#define TSUNAMI_PCI1_INTACK             0x803f8000000
#define TSUNAMI_PCI1_IO                 0x803fc000000
#define TSUNAMI_PCI1_CONFIG             0x803fe000000

#define TSUNAMI_REGION(region)          (TSUNAMI_##region - TSUNAMI_IO_BASE)

#endif /* _ALPHA_TSUNAMI_H */
