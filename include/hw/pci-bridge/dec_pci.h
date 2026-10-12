#ifndef _PCI_DEC_PCI_H
#define _PCI_DEC_PCI_H

#include "hw/pci/pci_bridge.h"
#include "qom/object.h"

struct DECPCIBridge {
    /*< private >*/
    PCIBridge parent_obj;
};

#define TYPE_DEC_PCI_BRIDGE "dec-pci-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(DECPCIBridge, DEC_PCI_BRIDGE)

#endif /* _PCI_DEC_PCI_H */
