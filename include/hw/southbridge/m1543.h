/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ALi M1543c (Aladdin IV) PCI to ISA bridge emulation.
 */

#ifndef _SOUTHBRIDGE_M1543_H
#define _SOUTHBRIDGE_M1543_H

#include "hw/pci/pci_device.h"
#include "hw/acpi/acpi.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/ide/pci.h"
#include "hw/isa/apm.h"
#include "hw/isa/superio.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/usb/hcd-uhci.h"
#include "hw/core/irq.h"

#define TYPE_M1543_PCI_DEVICE   "m1543-pci"
#define TYPE_M1543_SUPERIO      "m1543-superio"
#define TYPE_M1543_IDE          "m1543-ide"
#define TYPE_M1543_USB_UHCI     "m1543-usb-uhci"
#define TYPE_M1543_PMU          "m1543-pmu"

#define M1543_NUM_PIRQS         4       /* PIRQ[A-D] */

#define M1543_PM1_STS           0x00
#define M1543_PM1_EN            0x02
#define M1543_PM1_CNT           0x04
#define M1543_PM1_TMR           0x08
#define M1543_GPE0_STS          0x18
#define M1543_GPE0_EN           0x1A
#define M1543_GPE0_LEN          16

#define M1543_PIC               0x40
#define M1543_IORC              0x41
#define M1543_ISACI             0x42
#define M1543_ISACII            0x43
#define M1543_IDENRI            0x44
#define M1543_BCSC              0x47
#define M1543_IDEIC             0x58
#define M1543_GPOS              0x5A
#define M1543_SMCCII            0x5F
#define M1543_RAM               0x6D
#define M1543_SCIIR             0x76
#define M1543_USBIDS            0x72
#define M1543_USBIR             0x74
#define M1543_IDENRII           0x75
#define M1543_SMBIR             0x77

/* PIRT[A:D]: PCI IRQx Route Control Registers */
#define M1543_PIRTA             0x48
#define M1543_PIRTB             0x49
#define M1543_PIRTC             0x4A
#define M1543_PIRTD             0x4B

OBJECT_DECLARE_SIMPLE_TYPE(M1543PMUState, M1543_PMU)
struct M1543PMUState {
    /*< private >*/
    PCIDevice parent_obj;

    /*< public >*/
    MemoryRegion io;
    MemoryRegion io_gpe;
    ACPIREGS ar;
    APMState apm;
    PMSMBus smb;
    qemu_irq irq;
    Notifier powerdown_notifier;
};

OBJECT_DECLARE_SIMPLE_TYPE(M1543SuperIOState, M1543_SUPERIO)
struct M1543SuperIOState {
    /*< private >*/
    ISASuperIODevice parent_obj;

    /*< public >*/
    MemoryRegion io;
    uint8_t chip_id;
    uint8_t chip_rev;
    uint8_t step;
    uint8_t regs[48];
    uint8_t dev_regs[13][256];
};

OBJECT_DECLARE_SIMPLE_TYPE(M1543PCIState, M1543_PCI_DEVICE)
struct M1543PCIState {
    /*< private >*/
    PCIDevice parent_obj;

    /*< public >*/
#if ((ISA_NUM_IRQS * M1543_NUM_PIRQS) > 64)
#error "unable to encode pic state in 64bit in pic_levels."
#endif
    uint64_t pic_levels;
    uint8_t outport;

    qemu_irq cpu_intr;
    qemu_irq isa_irqs_in[ISA_NUM_IRQS];

    /* This member isn't used. Just for save/load compatibility */
    int32_t pci_irq_levels_vmstate[M1543_NUM_PIRQS];

    MemoryRegion ioport80;
    MemoryRegion ioport92;
    MC146818RtcState rtc;
    PCIIDEState ide;
    UHCIState uhci;
    M1543PMUState pmu;
    M1543SuperIOState sio;

    bool has_acpi;
    bool has_usb;
    bool has_superio;
    bool alt_pci_irq_routing;
};

#endif /* _SOUTHBRIDGE_M1543_H */
