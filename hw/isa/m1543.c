/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ALi M1543c (Aladdin IV) PCI to ISA bridge emulation.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/range.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/acpi/acpi.h"
#include "hw/block/fdc.h"
#include "hw/char/parallel-isa.h"
#include "hw/char/serial-isa.h"
#include "hw/dma/i8257.h"
#include "hw/i2c/pm_smbus.h"
#include "hw/ide/pci.h"
#include "hw/intc/i8259.h"
#include "hw/core/irq.h"
#include "hw/isa/apm.h"
#include "hw/isa/isa.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/southbridge/m1543.h"
#include "hw/timer/i8254.h"
#include "hw/usb/hcd-uhci.h"
#include "hw/audio/pcspk.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "trace.h"

#define M1543_CONFIG_ON_KEY1            0x51
#define M1543_CONFIG_ON_KEY2            0x23
#define M1543_CONFIG_OFF                0xbb

#define M1543_SIO_RESET                 0x02
#define M1543_SIO_INDEX                 0x03
#define M1543_DEVICE_SELECT             0x07
#define M1543_DEVICE_ID                 0x20
#define M1543_DEVICE_REV                0x21
#define M1543_DEVICE_PWR_CTRL           0x22
#define M1543_DEVICE_PWR_MGMT           0x23
#define M1543_DEVICE_OSC                0x24
#define M1543_DEVICE_STATUS             0x26
#define M1543_DEVICE_UART_SEL           0x2d
#define M1543_DEVICE_ACTIVE             0x30
#define M1543_DEVICE_ADDR               0x60
#define M1543_DEVICE_ADDR_HI            0x60
#define M1543_DEVICE_ADDR_LOW           0x61
#define M1543_DEVICE_IRQ1               0x70
#define M1543_DEVICE_IRQ2               0x72
#define M1543_DEVICE_DMA                0x74
#define M1543_DEVICE_CFG0               0xf0
#define M1543_DEVICE_CFG1               0xf1
#define M1543_DEVICE_CFG2               0xf2
#define M1543_DEVICE_CFG3               0xf3
#define M1543_DEVICE_CFG4               0xf4

#define M1543_LDEV_FDC                  0x00
#define M1543_LDEV_PARALLEL             0x03
#define M1543_LDEV_COM1                 0x04
#define M1543_LDEV_COM2                 0x05
#define M1543_LDEV_KBD                  0x07
#define M1543_LDEV_AUX                  0x08
#define M1543_LDEV_COM3                 0x0b
#define M1543_LDEV_HOTKEY               0x0c

#define M1543_DEV_STATUS_FDC            BIT(M1543_LDEV_FDC)
#define M1543_DEV_STATUS_PARALLEL       BIT(M1543_LDEV_PARALLEL)
#define M1543_DEV_STATUS_COM1           BIT(M1543_LDEV_COM1)
#define M1543_DEV_STATUS_COM2           BIT(M1543_LDEV_COM2)
#define M1543_DEV_STATUS_KBC            BIT(M1543_LDEV_KBD)

enum cfg_state {
    CFG_KEY1,
    CFG_KEY2,
    CFG_UNLOCK,
};

/* UARTs (compatible with NS16450 or PC16550) */
static uint16_t get_serial_iobase(ISASuperIODevice *sio, uint8_t index)
{
    switch (index) {
    case 0:
        return 0x3f8;
    case 1:
        return 0x2f8;
    case 2:
        return 0x3e8;
    default:
        g_assert_not_reached();
    }
}

static unsigned int get_serial_irq(ISASuperIODevice *sio, uint8_t index)
{
    switch (index) {
    case 0:
        return 4;
    case 1:
        return 3;
    case 2:
        /*
         * Should be 9 but that conflicts with ACPI, and QEMU does not
         * support IRQ reconfiguration.
         */
        return 4;
    default:
        g_assert_not_reached();
    }
}

/* Parallel port */
static uint16_t get_parallel_iobase(ISASuperIODevice *sio, uint8_t index)
{
    assert(!index);
    return 0x378;
}

static unsigned int get_parallel_irq(ISASuperIODevice *sio, uint8_t index)
{
    assert(!index);
    /* Should be 5, but QEMU does not support IRQ reconfiguration. */
    return 7;
}

static unsigned int get_parallel_dma(ISASuperIODevice *sio, uint8_t index)
{
    assert(!index);
    return 3;
}

/* Diskette controller (Software compatible with the Intel PC8477) */
static uint16_t get_fdc_iobase(ISASuperIODevice *sio, uint8_t index)
{
    assert(!index);
    return 0x3f0;
}

static unsigned int get_fdc_irq(ISASuperIODevice *sio, uint8_t index)
{
    assert(!index);
    return 6;
}

static unsigned int get_fdc_dma(ISASuperIODevice *sio, uint8_t index)
{
    assert(!index);
    return 2;
}

static uint8_t m1543_superio_uart_index(M1543SuperIOState *s, int index)
{
    bool select = s->regs[M1543_DEVICE_UART_SEL] & 0x20;

    switch (index) {
    case 0:
        return 0;
    case 1:
        return select ? 2 : 1;
    case 2:
        return select ? 1 : 2;
    default:
        g_assert_not_reached();
    }
}

static inline bool m1543_superio_device_active(M1543SuperIOState *s,
                                               uint8_t dev)
{
    switch (dev) {
    case M1543_LDEV_FDC:
    case M1543_LDEV_PARALLEL:
    case M1543_LDEV_COM1:
    case M1543_LDEV_COM2:
    case M1543_LDEV_KBD:
    case M1543_LDEV_AUX:
    case M1543_LDEV_COM3:
    case M1543_LDEV_HOTKEY:
        return s->dev_regs[dev][M1543_DEVICE_ACTIVE];
    default:
        g_assert_not_reached();
    }
}

static inline uint16_t m1543_superio_device_iobase(M1543SuperIOState *s,
                                                   uint8_t dev)
{
    switch (dev) {
    case M1543_LDEV_FDC:
    case M1543_LDEV_PARALLEL:
    case M1543_LDEV_COM1:
    case M1543_LDEV_COM2:
    case M1543_LDEV_KBD:
    case M1543_LDEV_AUX:
    case M1543_LDEV_COM3:
    case M1543_LDEV_HOTKEY:
        return lduw_be_p(s->dev_regs[dev] + M1543_DEVICE_ADDR);
    default:
        g_assert_not_reached();
    }
}

static void m1543_superio_cfg_commit(M1543SuperIOState *s)
{
    ISASuperIOClass *ic = ISA_SUPERIO_GET_CLASS(s);
    ISASuperIODevice *sio = ISA_SUPERIO(s);
    uint8_t status = M1543_DEV_STATUS_KBC;
    uint16_t iobase;
    bool active;
    int i;

    active = m1543_superio_device_active(s, M1543_LDEV_PARALLEL);
    iobase = m1543_superio_device_iobase(s, M1543_LDEV_PARALLEL);
    if (active) {
        status |= M1543_DEV_STATUS_PARALLEL;
    }
    trace_m1543_superio_cfg_commit("parallel", 0, M1543_LDEV_PARALLEL,
                                   iobase, active);

    isa_parallel_set_enabled(sio->parallel[0], active);
    isa_parallel_set_iobase(sio->parallel[0], iobase);

    for (i = 0; i < ic->serial.count; i++) {
        static const uint8_t serial_devs[] = {
            M1543_LDEV_COM1,
            M1543_LDEV_COM2,
            M1543_LDEV_COM3,
        };
        static const uint8_t serial_status[] = {
            M1543_DEV_STATUS_COM1,
            M1543_DEV_STATUS_COM2,
            0,
        };
        uint8_t sel = m1543_superio_uart_index(s, i);

        active = m1543_superio_device_active(s, serial_devs[sel]);
        iobase = m1543_superio_device_iobase(s, serial_devs[sel]);
        if (active) {
            status |= serial_status[sel];
        }
        trace_m1543_superio_cfg_commit("serial", sel, serial_devs[sel],
                                       iobase, active);

        isa_serial_set_enabled(sio->serial[sel], active);
        isa_serial_set_iobase(sio->serial[sel], iobase);
    }

    active = m1543_superio_device_active(s, M1543_LDEV_FDC);
    iobase = m1543_superio_device_iobase(s, M1543_LDEV_FDC);
    if (active) {
        status |= M1543_DEV_STATUS_FDC;
    }
    trace_m1543_superio_cfg_commit("floppy", 0, M1543_LDEV_FDC,
                                   iobase, active);

    isa_fdc_set_enabled(sio->floppy, active);
    isa_fdc_set_iobase(sio->floppy, iobase);

    s->regs[M1543_DEVICE_STATUS] = status;
}

static uint8_t m1543_superio_cfg_read(M1543SuperIOState *s, uint8_t dev,
                                      uint8_t index)
{
    uint8_t ret;

    switch (dev) {
    case M1543_LDEV_FDC:
    case M1543_LDEV_PARALLEL:
    case M1543_LDEV_COM1:
    case M1543_LDEV_COM2:
    case M1543_LDEV_KBD:
    case M1543_LDEV_AUX:
    case M1543_LDEV_COM3:
    case M1543_LDEV_HOTKEY:
        ret = s->dev_regs[dev][index];
        break;
    default:
        ret = 0;
        break;
    }

    trace_m1543_superio_cfg_read(dev, index, ret);
    return ret;
}

static void m1543_superio_cfg_write(M1543SuperIOState *s, uint8_t dev,
                                    uint8_t index, uint8_t val)
{
    trace_m1543_superio_cfg_write(dev, index, val);

    switch (dev) {
    case M1543_LDEV_FDC:
    case M1543_LDEV_PARALLEL:
    case M1543_LDEV_COM1:
    case M1543_LDEV_COM2:
    case M1543_LDEV_KBD:
    case M1543_LDEV_AUX:
    case M1543_LDEV_COM3:
    case M1543_LDEV_HOTKEY:
        s->dev_regs[dev][index] = val;

        /* Reconfigure if required. */
        if (index == M1543_DEVICE_ACTIVE) {
            m1543_superio_cfg_commit(s);
        }
        break;
    default:
        break;
    }
}

static uint64_t m1543_superio_read(void *opaque, hwaddr addr, unsigned width)
{
    M1543SuperIOState *s = opaque;
    uint8_t device = s->regs[M1543_DEVICE_SELECT];
    uint8_t index = s->regs[M1543_SIO_INDEX];
    uint8_t ret = 0;

    if ((addr & 1) == 0) {
        if (s->step != CFG_UNLOCK) {
            ret = 0;
        } else {
            ret = s->regs[M1543_SIO_INDEX];
        }
    } else {
        if (s->step == CFG_UNLOCK) {
            switch (index) {
            case M1543_DEVICE_SELECT:
            case M1543_DEVICE_ID:
            case M1543_DEVICE_REV:
            case M1543_DEVICE_PWR_CTRL:
            case M1543_DEVICE_PWR_MGMT:
            case M1543_DEVICE_OSC:
            case M1543_DEVICE_STATUS:
            case M1543_DEVICE_UART_SEL:
                ret = s->regs[index];
                break;
            case 0x30 ... 0xff:
                ret = m1543_superio_cfg_read(s, device, index);
                break;
            }
        }
    }

    trace_m1543_superio_read(addr, ret);
    return ret;
}

static void m1543_superio_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned width)
{
    M1543SuperIOState *s = opaque;

    trace_m1543_superio_write(addr, val);
    if ((addr & 1) == 0) {
        switch (s->step) {
        case CFG_KEY1:
            if (val == M1543_CONFIG_ON_KEY1) {
                s->step = CFG_KEY2;
            }
            break;
        case CFG_KEY2:
            if (val == M1543_CONFIG_ON_KEY2) {
                s->step = CFG_UNLOCK;
            } else {
                s->step = CFG_KEY1;
            }
            break;
        case CFG_UNLOCK:
            if (val == M1543_CONFIG_OFF) {
                s->step = CFG_KEY1;
            } else {
                s->regs[M1543_SIO_INDEX] = val;
            }
            break;
        }
    } else {
        uint8_t index = s->regs[M1543_SIO_INDEX];

        if (s->step == CFG_UNLOCK) {
            switch (index) {
            case M1543_SIO_RESET:
                device_cold_reset(DEVICE(s));
                break;
            case M1543_DEVICE_SELECT:
            case M1543_DEVICE_PWR_CTRL:
            case M1543_DEVICE_PWR_MGMT:
            case M1543_DEVICE_OSC:
            case M1543_DEVICE_UART_SEL:
                s->regs[index] = val;
                break;
            case 0x30 ... 0xff:
                m1543_superio_cfg_write(s, s->regs[M1543_DEVICE_SELECT],
                                        index, val);
                break;
            }
        }
    }
}

static const MemoryRegionOps m1543_superio_cfg_ops = {
    .read = m1543_superio_read,
    .write = m1543_superio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static int m1543_superio_post_load(void *opaque, int version_id)
{
    M1543SuperIOState *s = opaque;

    m1543_superio_cfg_commit(s);
    return 0;
}

static const VMStateDescription vmstate_m1543_superio = {
    .name = "m1543-superio",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = m1543_superio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(step, M1543SuperIOState),
        VMSTATE_UINT8_ARRAY(regs, M1543SuperIOState, 48),
        VMSTATE_UINT8_2DARRAY(dev_regs, M1543SuperIOState, 13, 256),
        VMSTATE_END_OF_LIST()
    },
};

#define STB(d, o, v)    stb_p(s->dev_regs[(M1543_LDEV_##d)] + (o), (v))
#define STW(d, o, v)    stw_be_p(s->dev_regs[(M1543_LDEV_##d)] + (o), (v))

static void m1543_superio_reset(DeviceState *dev)
{
    M1543SuperIOState *s = M1543_SUPERIO(dev);
    ISASuperIODevice *sio = ISA_SUPERIO(dev);
    ISASuperIOClass *ic = ISA_SUPERIO_GET_CLASS(s);
    uint8_t kbd_irq, mouse_irq;

    kbd_irq = object_property_get_uint(OBJECT(sio->kbc), "kbd-irq",
                                       &error_fatal);
    mouse_irq = object_property_get_uint(OBJECT(sio->kbc), "mouse-irq",
                                         &error_fatal);

    s->step = CFG_KEY1;
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->dev_regs, 0, sizeof(s->dev_regs));

    stb_p(s->regs + M1543_DEVICE_UART_SEL, 0x20);
    stb_p(s->regs + M1543_DEVICE_ID, s->chip_id);
    stb_p(s->regs + M1543_DEVICE_REV, s->chip_rev);

    STB(FDC, M1543_DEVICE_ACTIVE, 0);
    STW(FDC, M1543_DEVICE_ADDR, (*ic->floppy.get_iobase)(sio, 0));
    STB(FDC, M1543_DEVICE_IRQ1, (*ic->floppy.get_irq)(sio, 0));
    STB(FDC, M1543_DEVICE_DMA, (*ic->floppy.get_dma)(sio, 0));
    STB(FDC, M1543_DEVICE_CFG0, 0x08);
    STB(FDC, M1543_DEVICE_CFG1, 0x00);
    STB(FDC, M1543_DEVICE_CFG2, 0xff);

    STB(PARALLEL, M1543_DEVICE_ACTIVE, 0);
    STW(PARALLEL, M1543_DEVICE_ADDR, (*ic->parallel.get_iobase)(sio, 0));
    STB(PARALLEL, M1543_DEVICE_IRQ1, (*ic->parallel.get_irq)(sio, 0));
    STB(PARALLEL, M1543_DEVICE_DMA, (*ic->parallel.get_dma)(sio, 0));
    STB(PARALLEL, M1543_DEVICE_CFG0, 0x8c);
    STB(PARALLEL, M1543_DEVICE_CFG1, 0x85);

    STB(COM1, M1543_DEVICE_ACTIVE, 0);
    STW(COM1, M1543_DEVICE_ADDR, (*ic->serial.get_iobase)(sio, 0));
    STB(COM1, M1543_DEVICE_IRQ1, (*ic->serial.get_irq)(sio, 0));
    STB(COM1, M1543_DEVICE_CFG0, 0x00);
    STB(COM1, M1543_DEVICE_CFG1, 0x00);
    STB(COM1, M1543_DEVICE_CFG2, 0x0c);

    STB(COM2, M1543_DEVICE_ACTIVE, 0);
    STW(COM2, M1543_DEVICE_ADDR, (*ic->serial.get_iobase)(sio, 1));
    STB(COM2, M1543_DEVICE_IRQ1, (*ic->serial.get_irq)(sio, 1));
    STB(COM2, M1543_DEVICE_CFG0, 0x80);
    STB(COM2, M1543_DEVICE_CFG1, 0x00);
    STB(COM2, M1543_DEVICE_CFG2, 0x0c);

    STB(KBD, M1543_DEVICE_ACTIVE, 1);
    STB(KBD, M1543_DEVICE_IRQ1, kbd_irq);
    STB(KBD, M1543_DEVICE_IRQ2, mouse_irq);
    STB(KBD, M1543_DEVICE_CFG0, 0x40);

    STB(COM3, M1543_DEVICE_ACTIVE, 0);
    STW(COM3, M1543_DEVICE_ADDR, (*ic->serial.get_iobase)(sio, 2));
    STB(COM3, M1543_DEVICE_IRQ1, (*ic->serial.get_irq)(sio, 2));
    STB(COM3, M1543_DEVICE_CFG0, 0x00);
    STB(COM3, M1543_DEVICE_CFG1, 0x00);
    STB(COM3, M1543_DEVICE_CFG2, 0x0c);

    STB(HOTKEY, M1543_DEVICE_CFG0, 0x35);
    STB(HOTKEY, M1543_DEVICE_CFG1, 0x14);
    STB(HOTKEY, M1543_DEVICE_CFG2, 0x11);
    STB(HOTKEY, M1543_DEVICE_CFG3, 0x71);
    STB(HOTKEY, M1543_DEVICE_CFG4, 0x42);

    m1543_superio_cfg_commit(s);
}

static void m1543_superio_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    M1543SuperIOState *s = M1543_SUPERIO(dev);
    ISASuperIOClass *ic = ISA_SUPERIO_GET_CLASS(s);
    Error *local_err = NULL;

    (*ic->parent_realize)(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    memory_region_set_enabled(&s->io, true);
    isa_register_ioport(ISA_DEVICE(s), &s->io, 0x370);
}

static const Property m1543_superio_properties[] = {
    DEFINE_PROP_UINT8("chip-id", M1543SuperIOState, chip_id, 0x43),
    DEFINE_PROP_UINT8("chip-rev", M1543SuperIOState, chip_rev, 0x15),
};

static void m1543_superio_initfn(Object *obj)
{
    M1543SuperIOState *s = M1543_SUPERIO(obj);

    memory_region_init_io(&s->io, obj, &m1543_superio_cfg_ops, s,
                          "m1543-superio", 2);
}

static void m1543_superio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISASuperIOClass *sc = ISA_SUPERIO_CLASS(klass);

    sc->parallel = (ISASuperIOFuncs) {
        .count = 1,
        .get_iobase = get_parallel_iobase,
        .get_irq    = get_parallel_irq,
        .get_dma    = get_parallel_dma,
    };
    sc->serial = (ISASuperIOFuncs) {
        .count = 3,
        .get_iobase = get_serial_iobase,
        .get_irq    = get_serial_irq,
    };
    sc->floppy = (ISASuperIOFuncs) {
        .count = 1,
        .get_iobase = get_fdc_iobase,
        .get_irq    = get_fdc_irq,
        .get_dma    = get_fdc_dma,
    };
    sc->ide.count = 0;
    dc->vmsd = &vmstate_m1543_superio;
    device_class_set_legacy_reset(dc, m1543_superio_reset);
    device_class_set_parent_realize(dc, m1543_superio_realize,
                                    &sc->parent_realize);
    device_class_set_props(dc, m1543_superio_properties);
}

static const TypeInfo m1543_superio_info = {
    .name          = TYPE_M1543_SUPERIO,
    .parent        = TYPE_ISA_SUPERIO,
    .class_size    = sizeof(M1543SuperIOState),
    .instance_init = m1543_superio_initfn,
    .class_init    = m1543_superio_class_init,
};

#define ACPI_ENABLE     0xf1
#define ACPI_DISABLE    0xf0

#define VMSTATE_GPE_ARRAY(_field, _state)                            \
 {                                                                   \
     .name       = (stringify(_field)),                              \
     .version_id = 0,                                                \
     .info       = &vmstate_info_uint16,                             \
     .size       = sizeof(uint16_t),                                 \
     .flags      = VMS_SINGLE | VMS_POINTER,                         \
     .offset     = vmstate_offset_pointer(_state, _field, uint8_t),  \
 }

static const VMStateDescription vmstate_acpi = {
    .name = "m1543_pmu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, M1543PMUState),
        VMSTATE_UINT16(ar.pm1.evt.sts, M1543PMUState),
        VMSTATE_UINT16(ar.pm1.evt.en, M1543PMUState),
        VMSTATE_UINT16(ar.pm1.cnt.cnt, M1543PMUState),
        VMSTATE_STRUCT(apm, M1543PMUState, 0, vmstate_apm, APMState),
        VMSTATE_STRUCT(smb, M1543PMUState, 1, pmsmb_vmstate, PMSMBus),
        VMSTATE_TIMER_PTR(ar.tmr.timer, M1543PMUState),
        VMSTATE_INT64(ar.tmr.overflow_time, M1543PMUState),
        VMSTATE_GPE_ARRAY(ar.gpe.sts, M1543PMUState),
        VMSTATE_GPE_ARRAY(ar.gpe.en, M1543PMUState),
        VMSTATE_END_OF_LIST()
    }
};

static void m1543_pmu_write_config(PCIDevice *d, uint32_t addr, uint32_t val,
                                   int len)
{
    trace_m1543_pm_write(addr, val, len);
    pci_default_write_config(d, addr, val, len);
}

static void m1543_pmu_powerdown_req(Notifier *n, void *opaque)
{
    M1543PMUState *pm = container_of(n, M1543PMUState, powerdown_notifier);

    acpi_pm1_evt_power_down(&pm->ar);
}

static void m1543_pmu_update_sci(ACPIREGS *regs)
{
    M1543PMUState *pm = container_of(regs, M1543PMUState, ar);

    acpi_update_sci(&pm->ar, pm->irq);
}

static uint64_t m1543_gpe_readb(void *opaque, hwaddr addr, unsigned width)
{
    M1543PMUState *pm = opaque;

    return acpi_gpe_ioport_readb(&pm->ar, addr);
}

static void m1543_gpe_writeb(void *opaque, hwaddr addr, uint64_t val,
                             unsigned width)
{
    M1543PMUState *pm = opaque;

    acpi_gpe_ioport_writeb(&pm->ar, addr, val);
    acpi_update_sci(&pm->ar, pm->irq);
}

static const MemoryRegionOps m1543_gpe_ops = {
    .read = m1543_gpe_readb,
    .write = m1543_gpe_writeb,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void m1543_pmu_apm_ctrl_changed(uint32_t val, void *arg)
{
    M1543PMUState *s = arg;

    acpi_pm1_cnt_update(&s->ar, val == ACPI_ENABLE, val == ACPI_DISABLE);
}

static void m1543_pmu_reset(DeviceState *dev)
{
    M1543PMUState *s = M1543_PMU(dev);
    PCIDevice *d = PCI_DEVICE(s);
    uint8_t *pci_conf = d->config;
    uint8_t *pci_wmask = d->wmask;

    /* Initialize the PCI header. */
    pci_set_word(pci_conf + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    memset(pci_conf + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);

    /* Set wmask values. */
    memset(pci_wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    pci_set_long(pci_wmask + 0x40, 0x0000101f);
    pci_set_long(pci_wmask + 0x44, 0xff189fff);
    pci_set_long(pci_wmask + 0x48, 0xff000000);
    pci_set_long(pci_wmask + 0x4c, 0x00000105);
    pci_set_long(pci_wmask + 0x50, 0x0000ffff);
    pci_set_long(pci_wmask + 0x54, 0xffff7fff);
    pci_set_long(pci_wmask + 0x58, 0x87ff1fff);
    pci_set_long(pci_wmask + 0x5c, 0xffffffff);
    pci_set_long(pci_wmask + 0x60, 0x07f113ff);
    pci_set_long(pci_wmask + 0x64, 0xffff11ff);
    pci_set_long(pci_wmask + 0x68, 0xffffff07);
    pci_set_long(pci_wmask + 0x6c, 0x1ebfffff);
    pci_set_long(pci_wmask + 0x70, 0xff0f3fff);
    pci_set_long(pci_wmask + 0x74, 0xff7fff00);
    pci_set_long(pci_wmask + 0x78, 0xff070fff);
    pci_set_long(pci_wmask + 0x7c, 0xffffffff);
    pci_set_long(pci_wmask + 0x80, 0xff01f0ff);
    pci_set_long(pci_wmask + 0x84, 0xffffffff);
    pci_set_long(pci_wmask + 0x88, 0xffffffff);
    pci_set_long(pci_wmask + 0x8c, 0xffff0f0f);
    pci_set_long(pci_wmask + 0x90, 0xffff020f);
    pci_set_long(pci_wmask + 0x94, 0xfffffff0);
    pci_set_long(pci_wmask + 0x98, 0xffffffff);
    pci_set_long(pci_wmask + 0x9c, 0xffffffff);
    pci_set_long(pci_wmask + 0xa0, 0xffffffff);
    pci_set_long(pci_wmask + 0xa4, 0xffffffff);
    pci_set_long(pci_wmask + 0xa8, 0xffffffff);
    pci_set_long(pci_wmask + 0xac, 0xffffffff);
    pci_set_long(pci_wmask + 0xb0, 0x7f01ffff);
    pci_set_long(pci_wmask + 0xb4, 0x0fff0f7c);
    pci_set_long(pci_wmask + 0xb8, 0xffffffff);
    pci_set_long(pci_wmask + 0xbc, 0xff030f00);
    pci_set_long(pci_wmask + 0xc4, 0x061c120f);
    pci_set_long(pci_wmask + 0xc8, 0xff06ffff);
    pci_set_long(pci_wmask + 0xcc, 0x00000101);

    acpi_pm1_evt_reset(&s->ar);
    acpi_pm1_cnt_reset(&s->ar);
    acpi_pm_tmr_reset(&s->ar);
    acpi_gpe_reset(&s->ar);
    acpi_update_sci(&s->ar, s->irq);
}

static void m1543_pmu_realize(PCIDevice *dev, Error **errp)
{
    M1543PMUState *pm = M1543_PMU(dev);

    memory_region_init(&pm->io, OBJECT(dev), "m1543-pmu", 64);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &pm->io);

    apm_init(dev, &pm->apm, m1543_pmu_apm_ctrl_changed, pm);

    acpi_pm_tmr_init(&pm->ar, m1543_pmu_update_sci, &pm->io);
    acpi_pm1_evt_init(&pm->ar, m1543_pmu_update_sci, &pm->io);
    acpi_pm1_cnt_init(&pm->ar, &pm->io, false, false, 2, false);
    acpi_gpe_init(&pm->ar, M1543_GPE0_LEN);

    memory_region_init_io(&pm->io_gpe, OBJECT(dev), &m1543_gpe_ops, pm,
                          "acpi-gpe0", M1543_GPE0_LEN);
    memory_region_add_subregion(&pm->io, M1543_GPE0_STS, &pm->io_gpe);

    pm_smbus_init(DEVICE(dev), &pm->smb, true);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &pm->smb.io);

    pm->powerdown_notifier.notify = m1543_pmu_powerdown_req;
    qemu_register_powerdown_notifier(&pm->powerdown_notifier);
}

static void m1543_pmu_init(Object *obj)
{
    M1543PMUState *s = M1543_PMU(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
}

static void m1543_pmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = m1543_pmu_realize;
    k->config_write = m1543_pmu_write_config;
    k->vendor_id = PCI_VENDOR_ID_AL;
    k->device_id = PCI_DEVICE_ID_AL_M7101;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
    k->revision = 0x00;
    device_class_set_legacy_reset(dc, m1543_pmu_reset);
    /* Reason: part of M1543 chipset, does not exist stand alone */
    dc->user_creatable = false;
    dc->hotpluggable = false;
    dc->vmsd = &vmstate_acpi;
}

static const TypeInfo m1543_pmu_info = {
    .name          = TYPE_M1543_PMU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_init = m1543_pmu_init,
    .instance_size = sizeof(M1543PMUState),
    .class_init    = m1543_pmu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

#define PCI_IRQ_DISABLED    -1

/* PIRTI - PCI Interrupt to ISA IRQ Routing Table I */
static int m1543_pirq_assignments[ISA_NUM_IRQS] = {
    [0b0000] = PCI_IRQ_DISABLED,
    [0b0001] = 9,
    [0b0010] = 3,
    [0b0011] = 10,
    [0b0100] = 4,
    [0b0101] = 5,
    [0b0110] = 7,
    [0b0111] = 6,
    [0b1000] = 1,
    [0b1001] = 11,
    [0b1010] = PCI_IRQ_DISABLED,
    [0b1011] = 12,
    [0b1100] = PCI_IRQ_DISABLED,
    [0b1101] = 14,
    [0b1110] = PCI_IRQ_DISABLED,
    [0b1111] = 15,
};

static void ioport80_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    trace_m1543_ioport80_write(data);
}

static uint64_t ioport80_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xffffffffffffffffULL;
}

static const MemoryRegionOps ioport80_io_ops = {
    .write = ioport80_write,
    .read = ioport80_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void ioport92_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    M1543PCIState *s = opaque;
    int oldval = s->outport;

    trace_m1543_ioport92_write(val);
    s->outport = val;
    if ((val & 1) && !(oldval & 1)) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t ioport92_read(void *opaque, hwaddr addr,
                            unsigned size)
{
    M1543PCIState *s = opaque;
    uint32_t ret = s->outport;

    trace_m1543_ioport92_read(ret);
    return ret;
}

static const MemoryRegionOps ioport92_io_ops = {
    .read = ioport92_read,
    .write = ioport92_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void m1543_update_memory_mappings(M1543PCIState *s)
{
    PCIDevice *pd = PCI_DEVICE(s);
    bool port92_en = pd->config[M1543_ISACII] & BIT(7);

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->ioport92, port92_en);
    memory_region_transaction_commit();
}

static void m1543_request_i8259_irq(void *opaque, int irq, int level)
{
    M1543PCIState *s = opaque;

    trace_m1543_request_i8259_irq(level);
    qemu_set_irq(s->cpu_intr, level);
}

static int m1543_get_pci_irq(M1543PCIState *s, int pin)
{
    PCIDevice *d = PCI_DEVICE(s);

    switch (pin) {
    case 0:
        return d->config[M1543_PIRTA] & 0x0f;
    case 1:
        return d->config[M1543_PIRTA] >> 4;
    case 2:
        return d->config[M1543_PIRTB] & 0x0f;
    case 3:
        return d->config[M1543_PIRTB] >> 4;
    }
    return 0;
}

static void m1543_set_irq_pic(M1543PCIState *s, int pic_irq)
{
    int level = !!(s->pic_levels &
                    ((MAKE_64BIT_MASK(0, M1543_NUM_PIRQS)) <<
                     (pic_irq * M1543_NUM_PIRQS)));

    trace_m1543_set_irq_pic(pic_irq, level);
    qemu_set_irq(s->isa_irqs_in[pic_irq], level);
}

static void m1543_set_pci_irq_level_internal(M1543PCIState *s, int pirq,
                                             int level)
{
    int pic_irq;
    uint64_t mask;

    pic_irq = m1543_pirq_assignments[m1543_get_pci_irq(s, pirq)];
    if (unlikely(pic_irq >= ISA_NUM_IRQS || pic_irq == PCI_IRQ_DISABLED)) {
        return;
    }

    mask = 1ULL << ((pic_irq * M1543_NUM_PIRQS) + pirq);
    s->pic_levels &= ~mask;
    s->pic_levels |= mask * !!level;
}

static void m1543_set_pci_irq_level(M1543PCIState *s, int pirq, int level)
{
    int pic_irq;

    pic_irq = m1543_pirq_assignments[m1543_get_pci_irq(s, pirq)];
    if (unlikely(pic_irq >= ISA_NUM_IRQS || pic_irq == PCI_IRQ_DISABLED)) {
        return;
    }

    trace_m1543_set_irq(pic_irq, level);
    m1543_set_pci_irq_level_internal(s, pirq, level);
    m1543_set_irq_pic(s, pic_irq);
}

static void m1543_set_pci_irq(void *opaque, int pirq, int level)
{
    M1543PCIState *s = opaque;

    m1543_set_pci_irq_level(s, pirq, level);
}

static PCIINTxRoute m1543_route_intx_pin_to_irq(void *opaque, int pin)
{
    M1543PCIState *s = opaque;
    PCIINTxRoute route;
    int pic_irq;

    pic_irq = m1543_pirq_assignments[m1543_get_pci_irq(s, pin)];
    if (unlikely(pic_irq >= ISA_NUM_IRQS || pic_irq == PCI_IRQ_DISABLED)) {
        route.mode = PCI_INTX_DISABLED;
        route.irq = -1;
    } else {
        route.mode = PCI_INTX_ENABLED;
        route.irq = pic_irq;
    }
    trace_m1543_intx_pin_to_irq(pin, pic_irq);
    return route;
}

static void m1543_update_pci_irq_levels(M1543PCIState *s)
{
    PCIBus *bus = pci_get_bus(&s->parent_obj);
    int pirq;

    s->pic_levels = 0;
    for (pirq = 0; pirq < M1543_NUM_PIRQS; pirq++) {
        m1543_set_pci_irq_level(s, pirq, pci_bus_get_irq_level(bus, pirq));
    }
}

static void m1543_pci_init(Object *obj)
{
    M1543PCIState *s = M1543_PCI_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_out_named(dev, &s->cpu_intr, "intr", 1);
    qdev_init_gpio_out_named(DEVICE(obj), s->isa_irqs_in, "isa-irqs",
                             ARRAY_SIZE(s->isa_irqs_in));

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_MC146818_RTC);
    object_initialize_child(obj, "ide", &s->ide, TYPE_M1543_IDE);
}

static void m1543_pci_realize(PCIDevice *d, Error **errp)
{
    M1543PCIState *s = M1543_PCI_DEVICE(d);
    DeviceState *dev = DEVICE(d);
    PCIBus *pci_bus = pci_get_bus(d);
    ISADevice *pit, *pcspk;
    ISABus *isa_bus;
    qemu_irq *i8259, i8259_irq;
    uint32_t irq;
    int i;

    /* ISA bus */
    isa_bus = isa_bus_new(dev, pci_address_space(d), pci_address_space_io(d),
                          errp);
    if (!isa_bus) {
        return;
    }

    /* POST code device */
    memory_region_init_io(&s->ioport80, OBJECT(s), &ioport80_io_ops, s,
                          "port80", 1);
    memory_region_add_subregion_overlap(pci_address_space_io(d),
                                        0x80, &s->ioport80, 1);

    /* Port92h device */
    memory_region_init_io(&s->ioport92, OBJECT(s), &ioport92_io_ops, s,
                          "port92", 1);
    memory_region_add_subregion_overlap(pci_address_space_io(d),
                                        0x92, &s->ioport92, 1);
    memory_region_set_enabled(&s->ioport92, false);

    /* PIC */
    i8259_irq = qemu_allocate_irq(m1543_request_i8259_irq, s, 0);
    i8259 = i8259_init(isa_bus, i8259_irq);
    for (i = 0; i < ARRAY_SIZE(s->isa_irqs_in); i++) {
        s->isa_irqs_in[i] = i8259[i];
    }
    g_free(i8259);
    isa_bus_register_input_irqs(isa_bus, s->isa_irqs_in);

    /* PIT */
    pit = i8254_pit_init(isa_bus, 0x40, 0, NULL);
    pcspk = isa_new(TYPE_PC_SPEAKER);
    object_property_set_link(OBJECT(pcspk), "pit", OBJECT(pit), &error_fatal);
    if (!isa_realize_and_unref(pcspk, isa_bus, errp)) {
        return;
    }

    /* DMA */
    i8257_dma_init(OBJECT(d), isa_bus, 0);

    /* RTC */
    qdev_prop_set_int32(DEVICE(&s->rtc), "base_year", 1980);
    if (!qdev_realize(DEVICE(&s->rtc), BUS(isa_bus), errp)) {
        return;
    }
    irq = object_property_get_uint(OBJECT(&s->rtc), "irq", &error_fatal);
    isa_connect_gpio_out(ISA_DEVICE(&s->rtc), 0, irq);

    /* IDE */
    qdev_prop_set_int32(DEVICE(&s->ide), "addr", PCI_DEVFN(15, 0));
    if (!qdev_realize(DEVICE(&s->ide), BUS(pci_bus), errp)) {
        return;
    }
    for (i = 0; i < 2; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->ide), "isa-irq", i,
                                    isa_bus_get_irq(isa_bus, 14 + i));
    }

    /* USB */
    if (s->has_usb) {
        object_initialize_child(OBJECT(d), "uhci", &s->uhci,
                                TYPE_M1543_USB_UHCI);
        qdev_prop_set_int32(DEVICE(&s->uhci), "addr", PCI_DEVFN(19, 0));
        if (!qdev_realize(DEVICE(&s->uhci), BUS(pci_bus), errp)) {
            return;
        }
        qdev_connect_gpio_out(DEVICE(&s->uhci), 0,
                              isa_bus_get_irq(isa_bus, 10));
    }

    /* Power Management */
    if (s->has_acpi) {
        object_initialize_child(OBJECT(s), "pmu", &s->pmu, TYPE_M1543_PMU);
        qdev_prop_set_int32(DEVICE(&s->pmu), "addr", PCI_DEVFN(17, 0));
        if (!qdev_realize(DEVICE(&s->pmu), BUS(pci_bus), errp)) {
            return;
        }
        qdev_connect_gpio_out(DEVICE(&s->pmu), 0,
                              isa_bus_get_irq(isa_bus, 9));
    }

    /* Super IO */
    if (s->has_superio) {
        object_initialize_child(OBJECT(s), "superio", &s->sio,
                                TYPE_M1543_SUPERIO);
        if (!qdev_realize(DEVICE(&s->sio), BUS(isa_bus), errp)) {
            return;
        }
    }

    if (s->alt_pci_irq_routing) {
        pci_bus_irqs(pci_bus, m1543_set_pci_irq, d, M1543_NUM_PIRQS);
        pci_bus_set_route_irq_fn(pci_bus, m1543_route_intx_pin_to_irq);
    }
}

static void m1543_pci_write_config(PCIDevice *dev, uint32_t address,
                                   uint32_t val, int len)
{
    M1543PCIState *s = M1543_PCI_DEVICE(dev);
    int pic_irq;

    pci_default_write_config(dev, address, val, len);
    if (ranges_overlap(address, len, M1543_PIRTA, 4)) {
        pci_bus_fire_intx_routing_notifier(pci_get_bus(&s->parent_obj));
        m1543_update_pci_irq_levels(s);
        for (pic_irq = 0; pic_irq < ISA_NUM_IRQS; pic_irq++) {
            m1543_set_irq_pic(s, pic_irq);
        }
    }
    if (range_covers_byte(address, len, M1543_ISACII)) {
        m1543_update_memory_mappings(s);
    }
}

static void m1543_pci_reset(DeviceState *dev)
{
    M1543PCIState *s = M1543_PCI_DEVICE(dev);
    PCIDevice *d = PCI_DEVICE(dev);
    uint8_t *pci_conf = d->config;
    uint8_t *pci_wmask = d->wmask;

    s->pic_levels = 0;
    s->outport &= ~1;

    /* Set default values. */
    pci_set_word(pci_conf + PCI_COMMAND, PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                 PCI_COMMAND_MASTER | PCI_COMMAND_SPECIAL);
    pci_set_word(pci_conf + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_byte(pci_conf + M1543_SMBIR, 0x01);     /* SCI -> IRQ 9 */
    pci_set_byte(pci_conf + M1543_USBIR, 0x03);     /* USB -> IRQ 10 */
    pci_set_byte(pci_conf + M1543_IDENRI, 0x0d);    /* IDE0 -> IRQ 14 */
    pci_set_byte(pci_conf + M1543_IDENRII, 0x0f);   /* IDE1 -> IRQ 15 */

    /* Set wmask values. */
    memset(pci_wmask + PCI_CONFIG_HEADER_SIZE, 0,
           PCI_CONFIG_SPACE_SIZE - PCI_CONFIG_HEADER_SIZE);
    pci_set_long(pci_wmask + 0x40, 0xffcfff7f);
    pci_set_long(pci_wmask + 0x44, 0xff00cbdf);
    pci_set_long(pci_wmask + 0x48, 0xffffffff);
    pci_set_long(pci_wmask + 0x4c, 0x000000ff);
    pci_set_long(pci_wmask + 0x50, 0xcfff8fff);
    pci_set_long(pci_wmask + 0x54, 0xe0ffff00);
    pci_set_long(pci_wmask + 0x58, 0x020f0d7f);
    pci_set_long(pci_wmask + 0x5c, 0xffe0027f);
    pci_set_long(pci_wmask + 0x60, 0x00000000);
    pci_set_long(pci_wmask + 0x64, 0x00000000);
    pci_set_long(pci_wmask + 0x68, 0x00000000);
    pci_set_long(pci_wmask + 0x6c, 0x00ffbf00);
    pci_set_long(pci_wmask + 0x70, 0xffefefff);
    pci_set_long(pci_wmask + 0x74, 0x1fcf1fdf);
    pci_set_long(pci_wmask + 0x78, 0x00000073);
    pci_set_long(pci_wmask + 0x7c, 0x00000000);

    /* Clear unsupported bits or bits we don't support. */
    pci_byte_test_and_clear_mask(pci_wmask + M1543_IDENRI,  0b00011111);
    pci_byte_test_and_clear_mask(pci_wmask + M1543_IDEIC,   0b01110011);
    pci_byte_test_and_clear_mask(pci_wmask + M1543_SMCCII,  0b00000010);
    pci_byte_test_and_clear_mask(pci_wmask + M1543_USBIDS,  0b00001111);
    pci_byte_test_and_clear_mask(pci_wmask + M1543_USBIR,   0b00011111);
    pci_byte_test_and_clear_mask(pci_wmask + M1543_IDENRII, 0b00011111);
    pci_byte_test_and_clear_mask(pci_wmask + M1543_SMBIR,   0b00011111);
}

static int m1543_post_load(void *opaque, int version_id)
{
    M1543PCIState *s = opaque;
    int pirq;

    /*
     * Because the i8259 has not been deserialized yet, qemu_irq_raise
     * might bring the system to a different state than the saved one;
     * for example, the interrupt could be masked but the i8259 would
     * not know that yet and would trigger an interrupt in the CPU.
     *
     * Here, we update irq levels without raising the interrupt.
     * Interrupt state will be deserialized separately through the i8259.
     */
    s->pic_levels = 0;
    for (pirq = 0; pirq < M1543_NUM_PIRQS; pirq++) {
        m1543_set_pci_irq_level_internal(s, pirq,
                pci_bus_get_irq_level(pci_get_bus(&s->parent_obj), pirq));
    }
    m1543_update_memory_mappings(s);

    return 0;
}

static int m1543_pre_save(void *opaque)
{
    M1543PCIState *s = opaque;
    int i;

    for (i = 0; i < ARRAY_SIZE(s->pci_irq_levels_vmstate); i++) {
        s->pci_irq_levels_vmstate[i] =
                pci_bus_get_irq_level(pci_get_bus(&s->parent_obj), i);
    }
    return 0;
}

static bool ioport92_outport_needed(void *opaque)
{
    M1543PCIState *s = opaque;

    return s->outport != 0;
}

static const VMStateDescription vmstate_m1543_ioport92 = {
    .name = "m1543_pci/ioport92",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = ioport92_outport_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(outport, M1543PCIState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_m1543_pci = {
    .name = "m1543_pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = m1543_post_load,
    .pre_save = m1543_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, M1543PCIState),
        VMSTATE_INT32_ARRAY_V(pci_irq_levels_vmstate, M1543PCIState,
                              M1543_NUM_PIRQS, 3),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_m1543_ioport92,
        NULL
    },
};

static const Property m1543_pci_props[] = {
    DEFINE_PROP_BOOL("has-acpi", M1543PCIState, has_acpi, true),
    DEFINE_PROP_BOOL("has-usb", M1543PCIState, has_usb, true),
    DEFINE_PROP_BOOL("has-superio", M1543PCIState, has_superio, true),
    DEFINE_PROP_BOOL("x-alternate-pci-irq-routing",
                     M1543PCIState, alt_pci_irq_routing, false),
};

static void m1543_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = m1543_pci_realize;
    k->config_write = m1543_pci_write_config;
    k->vendor_id = PCI_VENDOR_ID_AL;
    k->device_id = PCI_DEVICE_ID_AL_M1533;
    k->class_id = PCI_CLASS_BRIDGE_ISA;
    k->revision = 0xc3;
    device_class_set_legacy_reset(dc, m1543_pci_reset);
    dc->vmsd = &vmstate_m1543_pci;
    /* Reason: part of M1543 chipset, needs to be wired up */
    dc->user_creatable = false;
    dc->hotpluggable = false;
    device_class_set_props(dc, m1543_pci_props);
}

static const TypeInfo m1543_pci_info = {
    .name          = TYPE_M1543_PCI_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(M1543PCIState),
    .instance_init = m1543_pci_init,
    .class_init    = m1543_pci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void m1543_register_types(void)
{
    type_register_static(&m1543_superio_info);
    type_register_static(&m1543_pmu_info);
    type_register_static(&m1543_pci_info);
}

type_init(m1543_register_types)
