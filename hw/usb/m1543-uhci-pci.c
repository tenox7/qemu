/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ALi M1543c (Aladdin IV) UHCI controller emulation.
 */

#include "qemu/osdep.h"
#include "hw/southbridge/m1543.h"
#include "hw/core/irq.h"
#include "hcd-uhci.h"

static void m1543_usb_uhci_realize(PCIDevice *dev, Error **errp)
{
    UHCIState *s = UHCI(dev);
    uint8_t *pci_conf = s->dev.config;

    /* Minimum Grant */
    pci_set_byte(pci_conf + PCI_MIN_GNT, 0x00);
    /* Maximum Latency */
    pci_set_byte(pci_conf + PCI_MAX_LAT, 0x50);

    usb_uhci_common_realize(dev, errp);

    /* IRQ is routed to ISA IRQ 10. */
    object_unref(s->irq);
    qdev_init_gpio_out(DEVICE(dev), &s->irq, 1);
}

static UHCIInfo uhci_info[] = {
    {
        .name      = TYPE_M1543_USB_UHCI,
        .vendor_id = PCI_VENDOR_ID_AL,
        .device_id = PCI_DEVICE_ID_AL_M5237,
        .revision  = 0x03,
        .irq_pin   = 0,
        .realize   = m1543_usb_uhci_realize,
        .unplug    = true,
        /* Reason: only works as USB function of the M1543 chipset */
        .notuser   = true,
    }
};

static const TypeInfo m1543_usb_uhci_type_info = {
    .parent         = TYPE_UHCI,
    .name           = TYPE_M1543_USB_UHCI,
    .class_init     = uhci_data_class_init,
    .class_data     = uhci_info,
};

static void m1543_usb_uhci_register_types(void)
{
    type_register_static(&m1543_usb_uhci_type_info);
}

type_init(m1543_usb_uhci_register_types)
