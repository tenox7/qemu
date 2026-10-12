/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * COMPAQ AlphaServer ES40 ("Clipper") emulation model.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/target_page.h"
#include "net/net.h"
#include "hw/alpha/tsunami.h"
#include "hw/alpha/tigcontrol.h"
#include "hw/alpha/am29f016.h"
#include "hw/alpha/rmc.h"
#include "hw/block/fdc.h"
#include "hw/char/parallel-isa.h"
#include "hw/southbridge/m1543.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "hw/block/flash.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci-bridge/dec_pci.h"
#include "hw/core/qdev-clock.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "cpu.h"
#include "internals.h"
#include <zlib.h>

#define TYPE_ES40_MACHINE       MACHINE_TYPE_NAME("es40")
OBJECT_DECLARE_SIMPLE_TYPE(ES40MachineState, ES40_MACHINE)

#define ES40_MAX_CPUS           4
#define ES40_MAX_RAM_SIZE       (32 * GiB)

#define PALCODE_ROM_FILENAME    "palcode.bin"
#define PALCODE_ROM_SIZE        (2 * MiB)

#define SRM_LOAD_ADDRESS        0x8000

struct ES40MachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    int pchip_count;
    bool srm_loaded;
};

static void create_pflash_am29f016(TigBus *tb, const char *name)
{
    DriveInfo *dinfo;
    DeviceState *dev;

    dev = qdev_new(TYPE_PFLASH_AM29F016);
    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint8(dev, "cs", 0);
    if ((dinfo = drive_get(IF_PFLASH, 0, 0)) != NULL) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }

    tigbus_realize_and_unref(TIG_BUS_DEVICE(dev), tb, &error_fatal);
}

static void create_rmc(TigBus *tb, uint32_t num_cpus, uint64_t freq_hz)
{
    DeviceState *dev;

    dev = qdev_new(TYPE_ES40_RMC);
    qdev_prop_set_uint32(dev, "num-cpus", num_cpus);
    qdev_prop_set_uint32(dev, "clock-frequency", freq_hz);
    qdev_prop_set_uint8(dev, "cs", 1);
    tigbus_realize_and_unref(TIG_BUS_DEVICE(dev), tb, &error_fatal);
}

static int z_uncompress(void *dst, size_t *dst_len, const void *src,
                        size_t src_len)
{
    z_stream zs;
    int err;

    memset(&zs, 0, sizeof(zs));
    zs.next_in = (unsigned char *)src;
    zs.avail_in = src_len;
    zs.next_out = dst;
    zs.avail_out = *dst_len;

    if ((err = inflateInit2(&zs, -MAX_WBITS)) != Z_OK) {
        return err;
    }

    if ((err = inflate(&zs, Z_FINISH)) != Z_STREAM_END) {
        inflateEnd(&zs);
        printf("%d\n", err);
        return err == Z_OK ? Z_BUF_ERROR : err;
    }

    *dst_len = zs.total_out;
    return inflateEnd(&zs);
}

static ssize_t es40_decompress_srm(ES40MachineState *ms, const char *filename,
                                   Error **errp)
{
    g_autoptr(GError) gerr = NULL;
    g_autofree char *comp_data = NULL;
    g_autofree char *decomp_data = NULL;
    uint32_t *cur, *end;
    size_t decomp_len;
    gsize comp_len;
    bool found = false;

    if (!g_file_get_contents(filename, &comp_data, &comp_len,
                             &gerr)) {
        error_setg(errp, "%s", gerr->message);
        return -1;
    }

    cur = (void *)(comp_data);
    end = (void *)(comp_data + comp_len);
    for (; cur < end; cur++) {
        if (ldl_le_p(cur) == 0x436d6957) {
            found = true;
            break;
        }
    }

    if (unlikely(!found)) {
        error_setg(errp, "signature not found in '%s'", filename);
        return -1;
    }

    decomp_len = comp_len * 4;
    decomp_data = g_malloc0(TARGET_PAGE_ALIGN(decomp_len));
    if (z_uncompress(decomp_data, &decomp_len, cur + 5, comp_len) != Z_OK) {
        error_setg(errp, "failed to decompress '%s'", filename);
        return -1;
    }

    if (ldl_le_p(decomp_data + 0x06b0) == 0xe4c00001 &&
        ldl_le_p(decomp_data + 0x06b4) == 0xc0c00003 &&
        ldl_le_p(decomp_data + 0x06b8) == 0x229f000c &&
        ldl_le_p(decomp_data + 0x06bc) == 0x7e9510c8) {
        /*
         * Certain SRM PALcode versions rely on an incredibly buggy check
         * to make sure RPCC works. Disable it for now.
         */
        warn_report_once("PALcode/CPU mismatch, patching SRM firmware.");
        stl_le_p(decomp_data + 0x06b0, 0x47ff041f);
    }

    rom_add_blob_fixed("es40.bootrom", decomp_data, decomp_len,
                       SRM_LOAD_ADDRESS);

    ms->srm_loaded = true;
    return decomp_len;
}

static void es40_cpu_reset(void *opaque)
{
    AlphaCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu_set_pc(cs, SRM_LOAD_ADDRESS | R_PC_PAL_MODE_MASK);
}

static void es40_machine_init(MachineState *machine)
{
    ES40MachineState *ms = ES40_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    AlphaCPU *cpus[ES40_MAX_CPUS] = {};
    Clock *cpu_refclk;
    PCIBus *pci_bus;
    TigBus *tig_bus;
    DeviceState *tsunami, *m1543c, *rtc, *dev;
    const char *firmware;
    g_autofree char *filename = NULL;
    int size, n;

    if (machine->ram_size > ES40_MAX_RAM_SIZE) {
        g_autofree char *sz = size_to_str(ES40_MAX_RAM_SIZE);
        error_report("can't model more than %s of RAM", sz);
        exit(EXIT_FAILURE);
    }

    /*
     * CPU objects (unlike devices) are not automatically reset on system
     * reset, so we must always register a handler to do so.
     */
    cpu_refclk = clock_new(OBJECT(machine), "system-refclk");
    clock_set_hz(cpu_refclk, 500000000);

    for (n = 0; n < machine->smp.cpus; n++) {
        AlphaCPU *cpu = ALPHA_CPU(object_new(machine->cpu_type));
        qdev_prop_set_bit(DEVICE(cpu), "start-powered-off", n != 0);
        qdev_connect_clock_in(DEVICE(cpu), "cpu-refclk", cpu_refclk);
        qdev_realize_and_unref(DEVICE(cpu), NULL, &error_fatal);
        qemu_register_reset(es40_cpu_reset, cpu);
        cpus[n] = cpu;
    }

    /*
     * Create and attach all of our devices.
     */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    ms->pchip_count = 2;
    tsunami = qdev_new(TYPE_TSUNAMI_CHIPSET);
    qdev_prop_set_uint32(tsunami, "ram-size", machine->ram_size / MiB);
    qdev_prop_set_uint32(tsunami, "num-cpus", machine->smp.cpus);
    qdev_prop_set_uint32(tsunami, "pchip-count", ms->pchip_count);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(tsunami), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(tsunami), 0, TSUNAMI_IO_BASE);

    for (n = 0; n < machine->smp.cpus; n++) {
        DeviceState *cpudev = DEVICE(cpus[n]);

        qdev_connect_gpio_out_named(tsunami, "cpu-mchk", n,
                            qdev_get_gpio_in(cpudev, ALPHA_CPU_INPUT_IRQ0));
        qdev_connect_gpio_out_named(tsunami, "cpu-device", n,
                            qdev_get_gpio_in(cpudev, ALPHA_CPU_INPUT_IRQ1));
        qdev_connect_gpio_out_named(tsunami, "cpu-timer", n,
                            qdev_get_gpio_in(cpudev, ALPHA_CPU_INPUT_IRQ2));
        qdev_connect_gpio_out_named(tsunami, "cpu-ipi", n,
                            qdev_get_gpio_in(cpudev, ALPHA_CPU_INPUT_IRQ3));
    }

    if (ms->pchip_count == 2) {
        dev = DEVICE(pci_new(PCI_DEVFN(6, 0), TYPE_DEC_PCI_BRIDGE));
        dev->id = g_strdup(TYPE_DEC_PCI_BRIDGE);
        pci_realize_and_unref(PCI_DEVICE(dev),
                              tsunami_get_pci_bus(tsunami, 1), &error_fatal);
    }

    /* Chipset setup. */
    pci_bus = tsunami_get_pci_bus(tsunami, 0);
    m1543c = DEVICE(pci_new(PCI_DEVFN(7, 0), TYPE_M1543_PCI_DEVICE));
    pci_realize_and_unref(PCI_DEVICE(m1543c), pci_bus, &error_fatal);
    qdev_connect_gpio_out_named(m1543c, "intr", 0,
                qdev_get_gpio_in_named(tsunami, "external", 0));

    dev = DEVICE(object_resolve_path_component(OBJECT(m1543c), "ide"));
    pci_ide_create_devs(PCI_DEVICE(dev));

    /* RTC setup. */
    rtc = DEVICE(object_resolve_path_component(OBJECT(m1543c), "rtc"));
    qdev_connect_gpio_out(rtc, 1,
                qdev_get_gpio_in_named(tsunami, "timer", 0));
    mc146818rtc_set_cmos_data(MC146818_RTC(rtc), 0x21, 0xde);
    if (machine->enable_graphics && vga_interface_type != VGA_NONE) {
        mc146818rtc_set_cmos_data(MC146818_RTC(rtc), 0x17, 0x01);
    }

    /* TIG bus setup. */
    tig_bus = tsunami_get_tig_bus(tsunami);
    create_pflash_am29f016(tig_bus, "pflash0");
    create_rmc(tig_bus, machine->smp.cpus, clock_get_hz(cpu_refclk));
    tigbus_create_device(tig_bus, TYPE_TIG_CONTROL);

    /* SCSI disk setup. */
    if (drive_get_max_bus(IF_SCSI) >= 0) {
        dev = DEVICE(pci_create_simple(pci_bus, -1, "lsi53c895a"));
        lsi53c8xx_handle_legacy_cmdline(dev);
    }

    /* VGA device. */
    pci_vga_init(pci_bus);

    /* Networking devices. */
    pci_init_nic_in_slot(pci_bus, mc->default_nic, NULL, "3");
    pci_init_nic_devices(pci_bus, mc->default_nic);

    /* Load the PALcode image. */
    if ((firmware = machine->firmware) == NULL) {
        firmware = PALCODE_ROM_FILENAME;
    }

    if ((filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware)) != NULL) {
        if ((size = es40_decompress_srm(ms, filename, NULL)) < 0) {
            size = load_image_targphys(filename, 0, PALCODE_ROM_SIZE, NULL);
        }
    } else {
        size = -1;
    }

    if (size < 0) {
        error_report("could not load SROM image '%s'", firmware);
        exit(1);
    }
}

static void es40_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        ALPHA_CPU_TYPE_NAME("ev6"),
        ALPHA_CPU_TYPE_NAME("ev67"),
        ALPHA_CPU_TYPE_NAME("ev68al"),
        ALPHA_CPU_TYPE_NAME("ev68cb"),
        NULL
    };
    static GlobalProperty isa_defaults[] = {
        { "isa-i8259", "edge-irq-withdrawal", "on" },
        { "isa-fdc", "strict-pio", "on" },
        { "mc146818rtc", "pf-without-pie", "on" },
        { "i8042", "selftest-reply-first", "on" },
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    compat_props_add(mc->compat_props, isa_defaults,
                     G_N_ELEMENTS(isa_defaults));
    mc->desc = "Compaq AlphaServer ES40";
    mc->init = es40_machine_init;
    mc->block_default_type = IF_IDE;
    mc->default_cpu_type = ALPHA_CPU_TYPE_NAME("ev67");
    mc->default_ram_id = "es40.ram";
    mc->default_ram_size = 1 * GiB;
    mc->default_nic = "tulip"; /* Should really be "i82559a". */
    mc->is_default = true;
    mc->max_cpus = ES40_MAX_CPUS;
    mc->valid_cpu_types = valid_cpu_types;
    mc->no_floppy = !module_object_class_by_name(TYPE_ISA_FDC);
    mc->no_parallel = !module_object_class_by_name(TYPE_ISA_PARALLEL);

    machine_add_audiodev_property(mc);
}

static const TypeInfo es40_machine_info = {
    .name          = TYPE_ES40_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = es40_class_init,
    .instance_size = sizeof(ES40MachineState),
};

static void es40_register_types(void)
{
    type_register_static(&es40_machine_info);
}

type_init(es40_register_types)
