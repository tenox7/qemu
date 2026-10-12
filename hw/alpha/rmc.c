
/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "system/cpus.h"
#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/alpha/tigbus.h"
#include "hw/alpha/rmc.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/rtc.h"
#include "cpu.h"
#include "trace.h"

struct ES40RMCState {
    /*< private >*/
    TigBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint8_t mem[0x4000];

    uint32_t num_cpus;
    uint32_t base_address;
    uint64_t freq_hz;

    CharFrontend display;
    bool display_inited;
};

#define RMC_SUCCESS             0x00
#define RMC_ERROR               0x80
#define RMC_INVALID_COMMAND     0x81
#define RMC_INVALID_QUALIFIER   0x82

struct CpuOnInfo {
    uint64_t entry;
};

static void es40_rmc_update_display(ES40RMCState *s)
{
    char text[17];

    memcpy(text, s->mem + 0x3500, sizeof(text) - 1);
    trace_es40_rmc_display(text);
    qemu_chr_fe_printf(&s->display, "\e[H\n\n|\e[32m%-16.16s\e[0m|\r\n\r\n",
                       text);
}

static void es40_rmc_display_event(void *opaque, QEMUChrEvent event)
{
    ES40RMCState *s = opaque;

    if (event == CHR_EVENT_OPENED && !s->display_inited) {
        qemu_chr_fe_printf(&s->display, "\e[H\e[44m%s\e[0m\r\n",
                           "rmc_console");
        qemu_chr_fe_printf(&s->display, "+----------------+\r\n");
        qemu_chr_fe_printf(&s->display, "|                |\r\n");
        qemu_chr_fe_printf(&s->display, "+----------------+\r\n");
        s->display_inited = true;
    }
}

static void es40_rmc_set_cpu_on_async_work(CPUState *cs,
                                           run_on_cpu_data data)
{
    struct CpuOnInfo *info = data.host_ptr;
    AlphaCPU *cpu;
    int cpuid;

    assert(bql_locked());

    cpu = ALPHA_CPU(cs);
    cpuid = cs->cpu_index;
    trace_es40_rmc_set_cpu_on(cpuid, info->entry);

    /* Initialize the cpu we are turning on. */
    cpu_reset(cs);
    cs->halted = 0;
    cs->exception_index = EXCP_NONE;
    cs->stopped = false;

    /* Start the new CPU at the requested address in PAL mode. */
    cpu_set_pc(cs, info->entry | R_PC_PAL_MODE_MASK);

    /* Finally set the power status. */
    cpu->power_state = CPU_POWER_ON;
    qemu_cpu_kick(cs);

    g_free(info);
}

static void es40_rmc_set_cpu_on(ES40RMCState *s, int cpuid, uint64_t entry)
{
    AlphaCPU *cpu;
    CPUState *cs;
    struct CpuOnInfo *info;

    assert(bql_locked());

    /* Retrieve the cpu we are powering up */
    cs = qemu_get_cpu(cpuid);
    if (!cs) {
        /* The cpu was not found */
        return;
    }

    cpu = ALPHA_CPU(cs);
    if (cpu->power_state == CPU_POWER_ON) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: CPU %d is already on\n",
                      __func__, cpuid);
        return;
    }

    /*
     * To avoid racing with a CPU we are just kicking off, we do the
     * final bit of preparation for the work in the target CPUs
     * context.
     */
    info = g_new(struct CpuOnInfo, 1);
    info->entry = entry;
    async_run_on_cpu(cs, es40_rmc_set_cpu_on_async_work,
                     RUN_ON_CPU_HOST_PTR(info));
}

static inline uint8_t es40_rmc_cmd_buffer_size(ES40RMCState *s)
{
    return s->mem[0xf9] + 1;
}

static inline uint16_t es40_rmc_cmd_buffer_address(ES40RMCState *s)
{
    return ((uint16_t)s->mem[0xfb] << 8) |
           ((uint16_t)s->mem[0xfa] << 0);
}

static inline uint8_t es40_rmc_cmd_function(ES40RMCState *s)
{
    return s->mem[0xfe];
}

typedef uint8_t (*CommandHandler)(ES40RMCState *s, uint8_t len, uint16_t addr);

static uint8_t es40_rmc_cmd_erase_fru_data(ES40RMCState *s, uint8_t len,
                                           uint16_t addr)
{
    trace_es40_rmc_command("erase_fru_data", len, addr);
    return RMC_SUCCESS;
}

static uint8_t es40_rmc_cmd_write_fru_data(ES40RMCState *s, uint8_t len,
                                           uint16_t addr)
{
    uint8_t hi, lo, i;

    trace_es40_rmc_command("write_fru_data", len, addr);

    hi = s->mem[0xfb];
    lo = s->mem[0xfa];
    switch (hi) {
    case 0x21: /* CPU0 */
    case 0x22: /* CPU1 */
    case 0x23: /* CPU2 */
    case 0x24: /* CPU3 */
        if ((hi - 0x20) > s->num_cpus) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: writing FRU data for nonexistent CPU %d\n",
                          __func__, hi - 0x20);
            return 0x80;
        }
        QEMU_FALLTHROUGH;

    case 0x01 ... 0x08: /* MMB0 DIMMs */
    case 0x09 ... 0x10: /* MMB1 DIMMs */
    case 0x11 ... 0x18: /* MMB2 DIMMs */
    case 0x19 ... 0x20: /* MMB3 DIMMs */
    case 0x25: /* MMB0 */
    case 0x26: /* MMB1 */
    case 0x27: /* MMB2 */
    case 0x28: /* MMB3 */
    case 0x29: /* CPB (PCI backplane) */
    case 0x2a: /* CSB (motherboard) */
    case 0x31: /* PSU0 */
    case 0x32: /* PSU1 */
    case 0x33: /* PSU2 */
    case 0x3b: /* SCSI0 */
    case 0x3c: /* SCSI1 */
    case 0x3d: /* PSU0 (continued) */
    case 0x3e: /* PSU1 (continued) */
    case 0x3f: /* PSU2 (continued) */
        for (i = 0; i < len; i++) {
            uint16_t dst = addr + i;
            uint16_t src = 0x3500 + lo + i;
            s->mem[dst] = s->mem[src];
        }

        return RMC_SUCCESS;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unknown FRU index (0x%02x:0x%04x)\n",
                      __func__, len, addr);
        return RMC_ERROR;
    }
}

static uint8_t es40_rmc_cmd_write_baud_rate(ES40RMCState *s, uint8_t len,
                                            uint16_t addr)
{
    trace_es40_rmc_command("write_baud_rate", len, addr);
    return RMC_SUCCESS;
}

static uint8_t es40_rmc_cmd_write_ocp(ES40RMCState *s, uint8_t len,
                                      uint16_t addr)
{
    trace_es40_rmc_command("write_ocp", len, addr);
    es40_rmc_update_display(s);
    return RMC_SUCCESS;
}

static uint8_t es40_rmc_cmd_write_flash(ES40RMCState *s, uint8_t len,
                                        uint16_t addr)
{
    trace_es40_rmc_command("write_flash", len, addr);
    return RMC_SUCCESS;
}

static void es40_rmc_handle_command(ES40RMCState *s)
{
    static const CommandHandler handlers[256] = {
        [0x00] = es40_rmc_cmd_erase_fru_data,
        [0x01] = es40_rmc_cmd_write_fru_data,
        [0x02] = es40_rmc_cmd_write_baud_rate,
        [0x03] = es40_rmc_cmd_write_ocp,
        [0xF0] = es40_rmc_cmd_write_flash,
    };
    uint8_t cmd = es40_rmc_cmd_function(s);
    uint8_t len = es40_rmc_cmd_buffer_size(s);
    uint16_t addr = es40_rmc_cmd_buffer_address(s);
    int status;

    if (handlers[cmd]) {
        status = (*handlers[cmd])(s, len, addr);
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented RMC command (0x%02x:0x%02x:0x%04x)\n",
                      __func__, cmd, len, addr);
        status = RMC_INVALID_COMMAND;
    }

    s->mem[0xfc] = status;
}

static uint64_t es40_rmc_read(void *opaque, hwaddr addr, unsigned size)
{
    ES40RMCState *s = opaque;
    uint64_t ret;

    ret = s->mem[addr & 0x3fff];
    trace_es40_rmc_read(addr, ret);
    return ret;
}

static void es40_rmc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    ES40RMCState *s = opaque;
    uint64_t nip;

    trace_es40_rmc_write(addr, val);

    s->mem[addr] = val;
    switch (addr) {
    case 0xff:
        /* Start of RMC command. */
        s->mem[0xfd] = s->mem[0xff];
        es40_rmc_handle_command(s);
        break;
    case 0xfd:
        /* End of RMC command. */
        s->mem[0xff] = s->mem[0xfd];
        break;
    case 0x3428:
        nip = ldq_le_p(s->mem + 0x3420);
        es40_rmc_set_cpu_on(s, 1, nip);
        break;
    case 0x3438:
        nip = ldq_le_p(s->mem + 0x3430);
        es40_rmc_set_cpu_on(s, 2, nip);
        break;
    case 0x3448:
        nip = ldq_le_p(s->mem + 0x3440);
        es40_rmc_set_cpu_on(s, 3, nip);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps es40_rmc_ops = {
    .read = es40_rmc_read,
    .write = es40_rmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void es40_rmc_reset(DeviceState *dev)
{
    ES40RMCState *s = ES40_RMC(dev);
    struct tm tm;
    int i;

#define S(r, v)     (s)->mem[(r)] = (v)

    S(0x00, 0x01);  /* BIST OK */
    S(0x01, 0x80);  /* CPU 0 is main */
    S(0x02, 0x01);  /* STR ok */
    S(0x03, 0x01);  /* CSC ok */
    S(0x04, 0x01);  /* Pchip0 ok */
    S(0x05, 0x01);  /* Pchip1 ok */
    S(0x06, 0x01);  /* DIMx ok */
    S(0x07, 0x01);  /* TIGbus ok */
    S(0x08, 0xdd);  /* DPRAM test started */
    S(0x09, 0x01);  /* DPRAM ok */
    S(0x0a, 0xff);  /* CPU speed ok */
    S(0x0b, (s->freq_hz / 1000000) & 0x00ff);
    S(0x0c, (s->freq_hz / 1000000) & 0xff00);

    /* Power On Time Stamp */
    qemu_get_timedate(&tm, 0);
    S(0x10, to_bcd(tm.tm_hour));
    S(0x11, to_bcd(tm.tm_min));
    S(0x12, to_bcd(tm.tm_sec));
    S(0x13, to_bcd(tm.tm_mday));
    S(0x14, to_bcd(tm.tm_mon + 1));
    S(0x15, to_bcd(tm.tm_year - 100));

    S(0x16, 0x00);  /* Power On Indicator */
    S(0x1e, 0x80);  /* Last "sync state" reached: GOOD */
    S(0x1f, 0x08);  /* Bcache size */

    /* Mirror CPU 0's state to all other present processors. */
    for (i = 1; i < s->num_cpus; i++) {
        memcpy(s->mem + (0x20 * i), s->mem, 0x1f);
    }

    S(0x90, 0xff);  /* Power Supply/VTERM */
    S(0x91, 0x00);  /* Power Supply/PS_OK */
    S(0x92, 0x07);  /* AC Input Value */

    /* Temperature from CPU(x) in BCD */
    for (i = 0; i < s->num_cpus; i++) {
        S(0x93 + i, to_bcd(40));
    }

    /* Temperature Zone(x) from 3 PCI temp sensors */
    S(0x97, to_bcd(40));
    S(0x98, to_bcd(40));
    S(0x99, to_bcd(40));

    S(0x9a, 0x8b);  /* Fan status; Raw fan speed value */
    S(0x9b, 0x8b);
    S(0x9c, 0x8b);
    S(0x9d, 0x8b);
    S(0x9e, 0x8b);
    S(0x9f, 0x8b);

    for (i = 0xa0; i < 0xaa; i++) {
        S(i, 0);    /* Machine check logout frame */
    }

    S(0xaa, 0);     /* Fan status */
    S(0xab, 0);     /* MMB0 DIMM I2C status */
    S(0xac, 0);     /* MMB1 DIMM I2C status */
    S(0xad, 0);     /* MMB2 DIMM I2C status */
    S(0xae, 0);     /* MMB3 DIMM I2C status */
    S(0xaf, 0);     /* MMB and CPU I2C bus status */

    /* MMB and CPU I2C bus status */
    switch (s->num_cpus) {
    case 1:
        S(0xaf, 0x0e);
        break;
    case 2:
        S(0xaf, 0x0c);
        break;
    case 3:
        S(0xaf, 0x08);
        break;
    case 4:
        S(0xaf, 0x00);
        break;
    default:
        g_assert_not_reached();
    }

    S(0xb0, 0);     /* CPB (PCI backplane) I2C */
    S(0xb1, 0);     /* CSB (motherboard) I2C EEROM */
    S(0xb2, 0);     /* SCSI backplane I2C */

    S(0xba, 0xba);  /* I2C done */
    S(0xbb, 0);     /* RMC flash ok */
    S(0xbc, 0);     /* RMC flash ok */

    S(0xbd, 0x07);  /* PS Input Value */
    S(0xbe, 0);     /* SPC fault value. */
    S(0xbf, 0);     /* Reason for system failure. */

    S(0xd9, 0);     /* RMC baud rate. */
    S(0xda, 0xaa);  /* TIG loaded. */
    for (i = 0; i < 3; i++) {
        S(0xdb + i * 9, 0xf4 + i);  /* Fan/Temp info from PS1 */
        S(0xdc + i * 9, 0x45);      /* 3.3V current */
        S(0xdd + i * 9, 0x51);      /* 5.0V current */
        S(0xde + i * 9, 0x37);      /* 12V current */
        S(0xdf + i * 9, 0x8b);      /* Fan speeds */
        S(0xe0 + i * 9, 0x6b);      /* AC voltage */
        S(0xe1 + i * 9, 0x49);      /* Internal temperature */
        S(0xe2 + i * 9, 0x4b);      /* Inlet temperature */
        S(0xe3 + i * 9, 0x00);
    }

    S(0xf9, 0);     /* Buffer size. */
    S(0xfa, 0);     /* Command address qualifier. */
    S(0xfb, 0);     /* Command address qualifier. */
    S(0xfc, 0);     /* Command status. */
    S(0xfd, 1);     /* Command ID. */
    S(0xff, 1);     /* Command code. */

    for (i = 0x2900; i < 0x2a00; i++) {
        S(i, 0);    /* PCI backplane FRU. */
    }
    for (i = 0x2a00; i < 0x2b00; i++) {
        S(i, 0);    /* Motherboard FRU. */
    }
    for (i = 0x2b00; i < 0x2c00; i++) {
        S(i, 0);    /* Last correctable error. */
    }
    for (i = 0x2c00; i < 0x2d00; i++) {
        S(i, 0);    /* Last redundant error. */
    }
    for (i = 0x2d00; i < 0x2e00; i++) {
        S(i, 0);    /* Last system failure. */
    }

    /* SROM version. */
    pstrcpy((char *)s->mem + 0x3000, 8, "V2.22G");

    /* RMC ROM version. */
    S(0x3009, 'V'); /* RMC ROM version */
    S(0x300a, '1');
    S(0x300b, '0');
    S(0x300c, 'V'); /* RMC flash version */
    S(0x300d, '1');
    S(0x300e, '0');

    S(0x3400, 8);   /* Size of Bcache */
    S(0x3401, 8);   /* Flash ROM valid */
    S(0x3402, 0);   /* System errors */

    /* CPU state */
    for (i = 0; i < s->num_cpus; i++) {
        S(0x3418 + 0x10 * i, 0xff);
    }

    /* SROM Array 0 to DIMM ID translation */
    for (i = 0; i < 0x20; i++) {
        S(0x34a0 + i, i);
    }
#undef S
}

static const Property es40_rmc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", ES40RMCState, num_cpus, 0),
    DEFINE_PROP_UINT32("base-address", ES40RMCState, base_address, 0x400000),
    DEFINE_PROP_UINT64("clock-frequency", ES40RMCState, freq_hz,
                       500000000ULL),
};

static int es40_rmc_post_load(void *opaque, int version_id)
{
    ES40RMCState *s = opaque;

    es40_rmc_update_display(s);
    return 0;
}

static const VMStateDescription vmstate_es40_rmc = {
    .name = "es40-rmc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = es40_rmc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mem, ES40RMCState, 0x4000),
        VMSTATE_END_OF_LIST()
    }
};

static void es40_rmc_realize(DeviceState *dev, Error **errp)
{
    ES40RMCState *s = ES40_RMC(dev);
    TigBusDevice *tbd = TIG_BUS_DEVICE(dev);
    MemoryRegion *iomem;
    Chardev *chr;

    if (unlikely(s->num_cpus < 1 || s->num_cpus > 4)) {
        error_setg(errp, "%s: unsupported cpu count (%u)",
                   __func__, s->num_cpus);
        error_append_hint(errp, "num-cpus count must be between 1 and 4\n");
        return;
    }

    iomem = tigbus_address_space(tigbus_from_device(tbd));
    memory_region_add_subregion(iomem, s->base_address, &s->iomem);

    chr = qemu_chr_new("rmc", "vc:320x200", NULL);
    qemu_chr_fe_init(&s->display, chr, NULL);
    qemu_chr_fe_set_handlers(&s->display, NULL, NULL,
                             es40_rmc_display_event, NULL, s, NULL, true);
}

static void es40_rmc_init(Object *obj)
{
    ES40RMCState *s = ES40_RMC(obj);

    memory_region_init_io(&s->iomem, obj, &es40_rmc_ops, s,
                          "es40-rmc.dpram", sizeof(s->mem));
}

static void es40_rmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = es40_rmc_realize;
    dc->vmsd = &vmstate_es40_rmc;
    /* Reason: needs to be wired up by board code. */
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, es40_rmc_reset);
    device_class_set_props(dc, es40_rmc_properties);
}

static const TypeInfo es40_rmc_info = {
    .name          = TYPE_ES40_RMC,
    .parent        = TYPE_TIG_BUS_DEVICE,
    .instance_size = sizeof(ES40RMCState),
    .instance_init = es40_rmc_init,
    .class_init    = es40_rmc_class_init,
};

static void es40_rmc_register_types(void)
{
    type_register_static(&es40_rmc_info);
}

type_init(es40_rmc_register_types)
