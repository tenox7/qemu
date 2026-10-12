/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DECpc AXP 150 / DEC 2000 model 300 (Jensen, PB22H-KB).
 *
 * Address decoding and SROM handoff follow DEC EK-A0638-TD.001,
 * chapters 5, 6, 8 and 10. The serial boot ROM is not emulated;
 * execution starts at the system Flash bootstrap in PAL mode.
 */

#include "qemu/osdep.h"
#include "system/physmem.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/block/flash.h"
#include "hw/block/fdc.h"
#include "hw/dma/i8257.h"
#include "hw/char/serial-mm.h"
#include "hw/input/i8042.h"
#include "hw/intc/i8259.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/timer/i8254.h"
#include "hw/timer/i8254_internal.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/runstate.h"
#include <glib/gstdio.h>
#include "system/system.h"
#include "cpu.h"
#include "internals.h"
#include "trace.h"

#define TYPE_JENSEN_MACHINE MACHINE_TYPE_NAME("jensen")
OBJECT_DECLARE_SIMPLE_TYPE(JensenMachineState, JENSEN_MACHINE)

#define JENSEN_FLASH_SIZE MiB
#define JENSEN_BOOT_SIZE (512 * KiB)
#define JENSEN_RTC_RAM_FIRST 0x0e
#define JENSEN_RTC_RAM_SIZE 66
#define JENSEN_BOOT_BASE 0x4000
#define JENSEN_BOOT_ENTRY (JENSEN_BOOT_BASE + 0x40)
#define JENSEN_INTA_BASE 0x100000000ULL
#define JENSEN_FLASH_BASE 0x180000000ULL
#define JENSEN_FLASH1_BASE 0x1a0000000ULL
#define JENSEN_LOCAL_BASE 0x1c0000000ULL
#define JENSEN_HAE_BASE 0x1d0000000ULL
#define JENSEN_SYSCTL_BASE 0x1e0000000ULL
#define JENSEN_SPARE_BASE 0x1f0000000ULL
#define JENSEN_EISA_MEM_BASE 0x200000000ULL
#define JENSEN_EISA_IO_BASE 0x300000000ULL

struct JensenMachineState {
    MachineState parent_obj;
    AlphaCPU *cpu;
    MemoryRegion isa_io, isa_mem;
    MemoryRegion local, eisa_io, eisa_mem, flash_window[2];
    MemoryRegion inta, hae_reg, sysctl_reg, spare_reg, board_id;
    AddressSpace io_as, mem_as, flash_as[2], rtc_as;
    MemoryRegion rtc_window, kbd_command, vga_rom, chipctrl0;
    AddressSpace kbd_command_as;
    ISAKBDState *kbd;
    ISADevice *pit;
    I8257State *dma[2];
    MemoryRegion isp_ext;
    int64_t pit2_pause_time;
    uint8_t dma_count_high[8], dma_ext_mode[8], dma_chain;
    uint8_t dma_stop[8][3];
    uint8_t nmi_control, nmi_extended, nmi_mask;
    bool keyboard_locked;
    bool s3_928;
    uint32_t s3_vram_mb;
    char *s3_romfile;
    PFlashCFI01 *flash[2];
    uint8_t hae, sysctl, spare, local_irqs;
    uint8_t inta_vector;
    bool inta_second;
    MC146818RtcState *rtc;
    uint8_t rtc_index, rtc_ram[JENSEN_RTC_RAM_SIZE];
    char *rtc_ram_file;
    FILE *rtc_file;
};

static uint64_t jensen_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenMachineState *s = opaque;
    uint8_t value;

    if (addr == 1 && s->rtc_index >= JENSEN_RTC_RAM_FIRST &&
        s->rtc_index < 0x50) {
        value = s->rtc_ram[s->rtc_index - JENSEN_RTC_RAM_FIRST];
    } else if (addr == 1 && s->rtc_index >= 0x50 && s->rtc_index < 0x69) {
        value = 0xff; /* VL82C106 has no RAM at indices 0x50..0x68. */
    } else {
        value = address_space_ldub(&s->rtc_as, s->rtc->io_base + addr,
                                   MEMTXATTRS_UNSPECIFIED, NULL);
    }
    trace_jensen_rtc_read(addr, s->rtc_index, value);
    return value;
}

static void jensen_rtc_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    JensenMachineState *s = opaque;

    if (addr == 0) {
        s->rtc_index = value & 0x7f;
    }
    trace_jensen_rtc_write(addr, s->rtc_index, value);
    if (addr == 1 && s->rtc_index >= JENSEN_RTC_RAM_FIRST &&
        s->rtc_index < 0x50) {
        unsigned offset = s->rtc_index - JENSEN_RTC_RAM_FIRST;

        s->rtc_ram[offset] = value;
        if (s->rtc_file &&
            (fseek(s->rtc_file, offset, SEEK_SET) ||
             fputc(value & 0xff, s->rtc_file) == EOF ||
             fflush(s->rtc_file))) {
            error_report("Jensen: could not write RTC RAM file '%s'",
                         s->rtc_ram_file);
            qemu_system_vmstop_request(RUN_STATE_IO_ERROR);
        }
        return;
    }
    if (addr == 1 && s->rtc_index >= 0x50 && s->rtc_index < 0x69) {
        return;
    }
    address_space_stb(&s->rtc_as, s->rtc->io_base + addr, value,
                      MEMTXATTRS_UNSPECIFIED, NULL);
}

static const MemoryRegionOps jensen_rtc_ops = {
    .read = jensen_rtc_read,
    .write = jensen_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* Jensen wires P17 high and P16 to the front-panel key-lock switch (1-11). */
static uint64_t jensen_kbd_command_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    JensenMachineState *s = opaque;

    return address_space_ldub(&s->kbd_command_as, 0x64,
                              MEMTXATTRS_UNSPECIFIED, NULL);
}

static void jensen_kbd_command_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    JensenMachineState *s = opaque;

    address_space_stb(&s->kbd_command_as, 0x64, value,
                      MEMTXATTRS_UNSPECIFIED, NULL);
    if (value == 0xc0) {
        /* Replace the input-port reply queued by the common i8042 model. */
        s->kbd->kbd.cbdata = s->keyboard_locked ? 0x80 : 0xc0;
    }
}

static const MemoryRegionOps jensen_kbd_command_ops = {
    .read = jensen_kbd_command_read,
    .write = jensen_kbd_command_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/*
 * 82357 ISP additions, DEC EK-A0638-TD.001 chapters 16 and 18.
 * The common DMA model still performs legacy transfers. Extended counts,
 * buffer chaining and ring-buffer transfers are not implemented here.
 */
/*
 * The common PIT leaves mode-0/4 gate inhibition unimplemented. Keep
 * channel 2's time origin paused on Jensen while its counting gate is low.
 * Modes 1 and 5 use the gate only as a trigger and must keep counting.
 */
static void jensen_pit2_sync(JensenMachineState *s)
{
    PITChannelState *ch = &PIT_COMMON(s->pit)->channels[2];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!ch->gate && (ch->mode == 0 || ch->mode == 4)) {
        ch->count_load_time = MIN(ch->count_load_time +
                                 now - s->pit2_pause_time, now);
    }
    s->pit2_pause_time = now;
}

static void jensen_nmi_update(JensenMachineState *s)
{
    bool pending = (s->nmi_extended & 0xe0) || (s->nmi_control & 0xc0);

    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu), ALPHA_CPU_INPUT_IRQ2),
                 pending && !s->nmi_mask);
}

static uint64_t jensen_isp_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenMachineState *s = opaque;
    PITChannelInfo info;
    unsigned channel;

    jensen_pit2_sync(s);

    switch (addr) {
    case 0x61:
        pit_get_channel_info(PIT_COMMON(s->pit), 2, &info);
        /* Refresh status follows the existing PIT refresh counter. */
        {
            PITChannelInfo refresh;
            pit_get_channel_info(PIT_COMMON(s->pit), 1, &refresh);
            return s->nmi_control | (info.out << 5) | (refresh.out << 4);
        }
    case 0x70:
        return s->nmi_mask << 7;
    case 0x461:
        return s->nmi_extended;
    case 0x4d4:
        return s->dma_chain & 0xef;
    case 0x40a:
        return 0; /* No chain-buffer interrupts without chained transfers. */
    }
    if (addr >= 0x401 && addr <= 0x407 && (addr & 1)) {
        return s->dma_count_high[(addr - 0x401) / 2];
    }
    if (addr == 0x4c6 || addr == 0x4ca || addr == 0x4ce) {
        return s->dma_count_high[4 + (addr - 0x4c2) / 4];
    }
    channel = (addr - 0x4e0) / 4;
    if (addr >= 0x4e0 && addr <= 0x4fe && channel != 4 &&
        (addr & 3) < 3) {
        return s->dma_stop[channel][addr & 3];
    }
    return 0xff;
}

static void jensen_isp_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    JensenMachineState *s = opaque;
    unsigned channel;

    switch (addr) {
    case 0x61:
        s->nmi_control = value & 0x0f;
        pit_set_gate(PIT_COMMON(s->pit), 2, value & 1);
        jensen_nmi_update(s);
        return;
    case 0x70:
        s->nmi_mask = (value >> 7) & 1;
        jensen_nmi_update(s);
        return;
    case 0x461:
        /* Disabling each source clears its latched pending indication. */
        s->nmi_extended = (s->nmi_extended & ((value & 0x0e) << 4)) |
                          (value & 0x0f);
        jensen_nmi_update(s);
        return;
    case 0x462:
        if (s->nmi_extended & 2) {
            s->nmi_extended |= 0x20;
        }
        jensen_nmi_update(s);
        return;
    case 0x40a:
    case 0x4d4:
        channel = (value & 3) + (addr == 0x4d4 ? 4 : 0);
        s->dma_chain = deposit32(s->dma_chain, channel, 1, !!(value & 4));
        if (value & 4) {
            qemu_log_mask(LOG_UNIMP,
                          "Jensen: chained DMA transfers unsupported\n");
        }
        return;
    case 0x40b:
    case 0x4d6:
        channel = (value & 3) + (addr == 0x4d6 ? 4 : 0);
        s->dma_ext_mode[channel] = value & 0xfc;
        s->dma_chain &= ~(1 << channel);
        return;
    }
    if (addr >= 0x401 && addr <= 0x407 && (addr & 1)) {
        s->dma_count_high[(addr - 0x401) / 2] = value;
    } else if (addr == 0x4c6 || addr == 0x4ca || addr == 0x4ce) {
        s->dma_count_high[4 + (addr - 0x4c2) / 4] = value;
    } else if (addr >= 0x4e0 && addr <= 0x4fe &&
               (addr - 0x4e0) / 4 != 4 && (addr & 3) < 3) {
        s->dma_stop[(addr - 0x4e0) / 4][addr & 3] =
            value & ((addr & 3) == 0 ? 0xfc : 0xff);
    }
}

static const MemoryRegionOps jensen_isp_ops = {
    .read = jensen_isp_read,
    .write = jensen_isp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* Side effects on the extended registers when legacy DMA is programmed. */
static void jensen_dma_legacy_write(JensenMachineState *s, hwaddr port,
                                     uint8_t value)
{
    static const int page_channel[16] = {
        -1, 2, 3, 1, -1, -1, -1, 0, -1, 6, 7, 5, -1, -1, -1, -1
    };
    int channel = -1, controller = -1;
    unsigned reg = 0;

    jensen_pit2_sync(s);

    if (port < 8) {
        channel = port / 2;
        reg = port & 1;
    } else if (port >= 0xc0 && port < 0xd0 && !(port & 1)) {
        channel = 4 + (port - 0xc0) / 4;
        reg = (port >> 1) & 1;
    } else if (port >= 0x80 && port < 0x90) {
        channel = page_channel[port & 15];
    }
    if (channel >= 0) {
        if (reg) {
            s->dma_count_high[channel] = 0;
        } else {
            s->dma[channel / 4]->regs[channel & 3].pageh = 0;
        }
    }
    if (port == 0x0d || port == 0xda) {
        controller = port == 0xda;
        memset(s->dma_count_high + controller * 4, 0, 4);
        s->dma_chain &= ~(0x0f << (controller * 4));
        for (channel = 0; channel < 4; channel++) {
            s->dma[controller]->regs[channel].pageh = 0;
        }
    } else if (port == 0x0b || port == 0xd6) {
        channel = (value & 3) + (port == 0xd6 ? 4 : 0);
        s->dma_chain &= ~(1 << channel);
    }
}

static uint16_t jensen_pit2_stopped_count(JensenMachineState *s)
{
    PITChannelState *ch = &PIT_COMMON(s->pit)->channels[2];
    int64_t elapsed = MAX(s->pit2_pause_time - ch->count_load_time, 0);
    uint64_t ticks = muldiv64(elapsed, PIT_FREQ, NANOSECONDS_PER_SECOND);

    return ch->count - ticks;
}

static uint8_t jensen_io_read_byte(JensenMachineState *s, hwaddr port)
{
    PITChannelState *ch = &PIT_COMMON(s->pit)->channels[2];
    uint16_t count;
    MemTxResult result;
    uint8_t value;

    jensen_pit2_sync(s);
    if (port == 0x0f || port == 0xde) {
        return s->dma[port == 0xde]->mask & 0x0f;
    }
    if (port == 0x42 && !ch->gate && (ch->mode == 0 || ch->mode == 4) &&
        !ch->status_latched && !ch->count_latched) {
        count = jensen_pit2_stopped_count(s);
        switch (ch->read_state) {
        case 2: /* MSB */
            return count >> 8;
        case 3: /* LSB, then MSB */
            ch->read_state = 4;
            return count;
        case 4: /* MSB, then LSB */
            ch->read_state = 3;
            return count >> 8;
        default:
            return count;
        }
    }
    value = address_space_ldub(&s->io_as, port,
                               MEMTXATTRS_UNSPECIFIED, &result);
    return result == MEMTX_OK ? value : 0xff;
}

static void jensen_io_write_byte(JensenMachineState *s, hwaddr port,
                                   uint8_t value)
{
    PITChannelState *ch = &PIT_COMMON(s->pit)->channels[2];
    bool loads_count = port == 0x42 && ch->write_state != 3;
    bool was_latched = ch->count_latched;

    jensen_dma_legacy_write(s, port, value);
    address_space_stb(&s->io_as, port, value, MEMTXATTRS_UNSPECIFIED, NULL);
    if (!ch->gate && (ch->mode == 0 || ch->mode == 4)) {
        if (loads_count) {
            /* A newly loaded stopped counter has consumed no clocks. */
            ch->count_load_time = s->pit2_pause_time;
        }
        if (port == 0x43 && !was_latched && ch->count_latched) {
            ch->latched_count = jensen_pit2_stopped_count(s);
        }
    }
}

static uint64_t jensen_local_read(void *opaque, hwaddr addr, unsigned size)
{
    return jensen_io_read_byte(opaque, (addr >> 9) & 0xffff);
}

static void jensen_local_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    jensen_io_write_byte(opaque, (addr >> 9) & 0xffff, value);
}

static const MemoryRegionOps jensen_local_ops = {
    .read = jensen_local_read,
    .write = jensen_local_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static uint64_t jensen_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    AddressSpace *as = opaque;
    return address_space_ldub(as, addr >> 9,
                              MEMTXATTRS_UNSPECIFIED, NULL);
}

static void jensen_flash_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    AddressSpace *as = opaque;
    address_space_stb(as, addr >> 9, value,
                      MEMTXATTRS_UNSPECIFIED, NULL);
}

static const MemoryRegionOps jensen_flash_ops = {
    .read = jensen_flash_read,
    .write = jensen_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/* CA<8:5> encodes byte enables; the value occupies its EISA byte lane. */
static uint64_t jensen_sparse_read(JensenMachineState *s, AddressSpace *as,
                                   hwaddr addr)
{
    hwaddr busaddr = (addr >> 7) | ((hwaddr)s->hae << 25);
    unsigned len = ((addr >> 5) & 3) + 1;
    uint8_t data[4] = { 0xff, 0xff, 0xff, 0xff };
    uint64_t value = 0;
    unsigned i;

    if (as == &s->io_as && s->s3_928 && busaddr >= 0x4000 &&
        (busaddr & 0x3fc) == 0x2e8) {
        address_space_read(as, busaddr, MEMTXATTRS_UNSPECIFIED, data, len);
    } else if (as == &s->io_as) {
        for (i = 0; i < len; i++) {
            data[i] = jensen_io_read_byte(s, busaddr + i);
        }
    } else if (address_space_read(as, busaddr, MEMTXATTRS_UNSPECIFIED,
                                  data, len) != MEMTX_OK) {
        memset(data, 0xff, len); /* Empty EISA slots float high. */
    }
    for (i = 0; i < len; i++) {
        value |= (uint64_t)data[i] << (i * 8);
    }
    return value << ((busaddr & 3) * 8);
}

static void jensen_sparse_write(JensenMachineState *s, AddressSpace *as,
                                hwaddr addr, uint64_t value)
{
    hwaddr busaddr = (addr >> 7) | ((hwaddr)s->hae << 25);
    unsigned len = ((addr >> 5) & 3) + 1;
    uint8_t data[4];
    unsigned i;

    value >>= (busaddr & 3) * 8;
    for (i = 0; i < len; i++) {
        data[i] = value >> (i * 8);
        if (as == &s->io_as && !(s->s3_928 && busaddr >= 0x4000 &&
                               (busaddr & 0x3fc) == 0x2e8)) {
            jensen_io_write_byte(s, busaddr + i, data[i]);
        }
    }
    if (as != &s->io_as || (s->s3_928 && busaddr >= 0x4000 &&
                               (busaddr & 0x3fc) == 0x2e8)) {
        address_space_write(as, busaddr, MEMTXATTRS_UNSPECIFIED, data, len);
    }
}

static uint64_t jensen_eisa_io_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenMachineState *s = opaque;
    return jensen_sparse_read(s, &s->io_as, addr);
}

static void jensen_eisa_io_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    JensenMachineState *s = opaque;
    jensen_sparse_write(s, &s->io_as, addr, value);
}

static const MemoryRegionOps jensen_eisa_io_ops = {
    .read = jensen_eisa_io_read,
    .write = jensen_eisa_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static uint64_t jensen_eisa_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenMachineState *s = opaque;
    return jensen_sparse_read(s, &s->mem_as, addr);
}

static void jensen_eisa_mem_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    JensenMachineState *s = opaque;
    jensen_sparse_write(s, &s->mem_as, addr, value);
}

static const MemoryRegionOps jensen_eisa_mem_ops = {
    .read = jensen_eisa_mem_read,
    .write = jensen_eisa_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

/* System-board EISA ID DEC2400, as specified by !DEC2400.CFG. */
static uint64_t jensen_board_id_read(void *opaque, hwaddr addr, unsigned size)
{
    static const uint8_t id[] = { 0x10, 0xa3, 0x24, 0x00 };

    return id[addr];
}

static void jensen_board_id_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned size)
{
    /* Product identification registers are read-only. */
}

static const MemoryRegionOps jensen_board_id_ops = {
    .read = jensen_board_id_read,
    .write = jensen_board_id_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static uint64_t jensen_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    return *(uint8_t *)opaque;
}

static void jensen_reg_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    *(uint8_t *)opaque = value;
}

static const MemoryRegionOps jensen_reg_ops = {
    .read = jensen_reg_read,
    .write = jensen_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static uint64_t jensen_inta_read(void *opaque, hwaddr addr, unsigned size)
{
    JensenMachineState *s = opaque;
    unsigned vector;

    /* 82357 INTA uses two bus cycles (EK-A0638-TD.001, 17-8/17-9). */
    if (!s->inta_second) {
        s->inta_vector = pic_read_irq(isa_pic);
        vector = 0xff; /* The first cycle does not drive the vector. */
    } else {
        vector = s->inta_vector;
    }
    s->inta_second = !s->inta_second;
    trace_jensen_inta_read(addr, size, vector);
    return vector;
}

static void jensen_inta_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    /* INTA only responds to reads; writes have no effect. */
}

static const MemoryRegionOps jensen_inta_ops = {
    .read = jensen_inta_read,
    .write = jensen_inta_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void jensen_local_irq(void *opaque, int n, int level)
{
    JensenMachineState *s = opaque;
    s->local_irqs = deposit32(s->local_irqs, n, 1, level);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu), ALPHA_CPU_INPUT_IRQ5),
                 (s->local_irqs & 3) != 0);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu), ALPHA_CPU_INPUT_IRQ3),
                 (s->local_irqs & 12) != 0);
}

/* Complete the interrupt-controller handoff normally done by SROM$357_INIT. */
static void jensen_srom_init_pic(JensenMachineState *s)
{
    static const struct {
        uint16_t port;
        uint8_t value;
    } init[] = {
        /* Initialize the slave first (EK-A0638-TD.001, 17-11). */
        { 0xa0, 0x11 }, { 0xa1, 0x10 }, { 0xa1, 2 }, { 0xa1, 1 },
        { 0xa1, 0xff },
        { 0x20, 0x11 }, { 0x21, 0x08 }, { 0x21, 4 }, { 0x21, 1 },
        { 0x21, 0xff },
    };

    /* SRM PALcode maps vectors 8..23 to the sixteen EISA IRQs. */
    for (unsigned i = 0; i < ARRAY_SIZE(init); i++) {
        address_space_stb(&s->io_as, init[i].port, init[i].value,
                          MEMTXATTRS_UNSPECIFIED, NULL);
    }
}

static void jensen_reset(MachineState *ms, ResetType type)
{
    JensenMachineState *s = JENSEN_MACHINE(ms);
    CPUAlphaState *env = &s->cpu->env;
    g_autofree uint8_t *scratch = g_malloc0(3 * MiB);

    /* SROM runs after hardware reset; device reset must not undo its setup. */
    qemu_devices_reset(type);

    /* Ensure fresh console scratch RAM for the SROM handoff (10-12). */
    physical_memory_write(0, scratch, 3 * MiB);
    /* Reload the SROM handoff from the current Flash on every reset. */
    physical_memory_write(JENSEN_BOOT_BASE,
        memory_region_get_ram_ptr(pflash_cfi01_get_memory(s->flash[1])),
        JENSEN_BOOT_SIZE);
    cpu_reset(CPU(s->cpu));
    s->hae = 0;
    s->spare = 0;
    s->local_irqs = 0;
    s->inta_second = false;
    s->inta_vector = 0;
    s->rtc_index = 0;
    memset(s->dma_count_high, 0, sizeof(s->dma_count_high));
    memset(s->dma_ext_mode, 0, sizeof(s->dma_ext_mode));
    memset(s->dma_stop, 0, sizeof(s->dma_stop));
    s->dma_chain = 0;
    s->nmi_control = s->nmi_extended = s->nmi_mask = 0;
    jensen_nmi_update(s);
    jensen_srom_init_pic(s);
    pit_set_gate(PIT_COMMON(s->pit), 2, 0);
    s->pit2_pause_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    mc146818rtc_set_cmos_data(s->rtc, 0x69, 0x9d);
    mc146818rtc_set_cmos_data(s->rtc, 0x6a, 0x07);
    s->sysctl = ms->ram_size == 128 * MiB ? 0xa0 : 0x20;
    env->ipr.pal_base = JENSEN_BOOT_BASE;
    alpha_cpu_set_reg_value(env, 20, ms->ram_size / MiB);
    /* V2.2 SROM handoff uses R24 = 6600 for the 150 MHz CPU timing. */
    alpha_cpu_set_reg_value(env, 24, 6600);
    cpu_set_pc(CPU(s->cpu), JENSEN_BOOT_ENTRY | R_PC_PAL_MODE_MASK);
}

static void jensen_map(JensenMachineState *s, MemoryRegion *mr,
                       const MemoryRegionOps *ops, void *opaque,
                       const char *name, hwaddr base, uint64_t size)
{
    memory_region_init_io(mr, OBJECT(s), ops, opaque, name, size);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void jensen_rtc_ram_init(JensenMachineState *s)
{
    bool create = false;

    if (!s->rtc_ram_file) {
        return;
    }
    s->rtc_file = g_fopen(s->rtc_ram_file, "r+b");
    if (!s->rtc_file && errno == ENOENT) {
        s->rtc_file = g_fopen(s->rtc_ram_file, "w+b");
        create = true;
    }
    if (!s->rtc_file) {
        error_report("Jensen: cannot open RTC RAM file '%s': %s",
                     s->rtc_ram_file, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (create) {
        if (fwrite(s->rtc_ram, 1, sizeof(s->rtc_ram), s->rtc_file) !=
            sizeof(s->rtc_ram) || fflush(s->rtc_file)) {
            error_report("Jensen: cannot initialize RTC RAM file '%s'",
                         s->rtc_ram_file);
            exit(EXIT_FAILURE);
        }
    } else {
        if (fseek(s->rtc_file, 0, SEEK_END) ||
            ftell(s->rtc_file) != sizeof(s->rtc_ram) ||
            fseek(s->rtc_file, 0, SEEK_SET) ||
            fread(s->rtc_ram, 1, sizeof(s->rtc_ram), s->rtc_file) !=
            sizeof(s->rtc_ram)) {
            error_report("Jensen: RTC RAM file must contain exactly %u bytes",
                         JENSEN_RTC_RAM_SIZE);
            exit(EXIT_FAILURE);
        }
    }
}

static void jensen_init(MachineState *machine)
{
    JensenMachineState *s = JENSEN_MACHINE(machine);
    Clock *refclk;
    ISABus *bus;
    ISADevice *rtc, *kbd, *fdc, *vga = NULL;
    DriveInfo *floppy_drive[MAX_FD];
    unsigned i;
    qemu_irq *pic;
    DriveInfo *flash_drive[2];
    const char *bios = machine->firmware ?: "jensen.bin";
    g_autofree char *path = NULL;
    g_autofree char *image = NULL;
    gsize image_len;

    if (machine->ram_size != 64 * MiB && machine->ram_size != 128 * MiB) {
        error_report("Jensen currently supports 64M or 128M of RAM");
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < 2; i++) {
        flash_drive[i] = drive_get(IF_PFLASH, 0, i);
    }
    if (machine->firmware || (!flash_drive[0] && !flash_drive[1])) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios);
        if (!path || !g_file_get_contents(path, &image, &image_len, NULL) ||
            image_len != JENSEN_FLASH_SIZE) {
            error_report("Jensen requires a raw 1 MiB system ROM (-bios); "
                         "extract it using scripts/jensen-extract.py");
            exit(EXIT_FAILURE);
        }
    }

    jensen_rtc_ram_init(s);

    refclk = clock_new(OBJECT(machine), "system-refclk");
    clock_set_hz(refclk, 150000000);
    s->cpu = ALPHA_CPU(object_new(machine->cpu_type));
    qdev_connect_clock_in(DEVICE(s->cpu), "cpu-refclk", refclk);
    qdev_realize_and_unref(DEVICE(s->cpu), NULL, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    memory_region_init(&s->isa_io, OBJECT(s), "jensen-isa-io", 0x10000);
    memory_region_init(&s->isa_mem, OBJECT(s), "jensen-isa-memory", 4 * GiB);
    address_space_init(&s->io_as, &s->isa_io, "jensen-io");
    address_space_init(&s->mem_as, &s->isa_mem, "jensen-memory");
    memory_region_init_io(&s->board_id, OBJECT(s), &jensen_board_id_ops, s,
                          "jensen-system-board-id", 4);
    memory_region_add_subregion(&s->isa_io, 0xc80, &s->board_id);
    bus = isa_bus_new(NULL, &s->isa_mem, &s->isa_io, &error_fatal);
    pic = i8259_init(bus, qdev_get_gpio_in(DEVICE(s->cpu),
                                         ALPHA_CPU_INPUT_IRQ1));
    isa_bus_register_input_irqs(bus, pic);
    s->pit = i8254_pit_init(bus, 0x40, 0, NULL);
    /* PC/AT-compatible DMA subset of the 82357, including high page ports. */
    i8257_dma_init(OBJECT(machine), bus, true);
    s->dma[0] = I8257(bus->dma[0]);
    s->dma[1] = I8257(bus->dma[1]);
    memory_region_init_io(&s->isp_ext, OBJECT(s), &jensen_isp_ops, s,
                          "jensen-isp-extended", 0x500);
    /* The common DMA high-page and PIC ELCR mappings take precedence. */
    memory_region_add_subregion_overlap(&s->isa_io, 0, &s->isp_ext, -1);
    fdc = isa_new(TYPE_ISA_FDC);
    qdev_prop_set_bit(DEVICE(fdc), "strict-pio", true);
    for (i = 0; i < MAX_FD; i++) {
        floppy_drive[i] = drive_get(IF_FLOPPY, 0, i);
    }
    isa_realize_and_unref(fdc, bus, &error_fatal);
    isa_fdc_init_drives(fdc, floppy_drive);

    rtc = isa_new(TYPE_MC146818_RTC);
    qdev_prop_set_uint16(DEVICE(rtc), "iobase", 0x170);
    qdev_prop_set_int32(DEVICE(rtc), "base_year", 1980);
    isa_realize_and_unref(rtc, bus, &error_fatal);
    qdev_connect_gpio_out(DEVICE(rtc), 0,
                          qdev_get_gpio_in(DEVICE(s->cpu),
                                           ALPHA_CPU_INPUT_IRQ0));
    s->rtc = MC146818_RTC(rtc);
    address_space_init(&s->rtc_as, &s->rtc->io, "jensen-rtc-device");
    memory_region_init_io(&s->rtc_window, OBJECT(s), &jensen_rtc_ops, s,
                          "jensen-vl82c106-rtc", 2);
    memory_region_add_subregion_overlap(&s->isa_io, 0x170, &s->rtc_window, 1);
    /* VL82C106 control register 0 is also accessible at port 0x102 (24-9). */
    memory_region_init_io(&s->chipctrl0, OBJECT(s), &jensen_reg_ops,
                          &s->rtc->cmos_data[0x69], "jensen-chipctrl0", 1);
    memory_region_add_subregion(&s->isa_io, 0x102, &s->chipctrl0);
    kbd = isa_create_simple(bus, TYPE_I8042);
    s->kbd = I8042(kbd);
    address_space_init(&s->kbd_command_as, &s->kbd->io[1],
                       "jensen-kbd-command-device");
    memory_region_init_io(&s->kbd_command, OBJECT(s), &jensen_kbd_command_ops,
                          s, "jensen-kbd-command", 1);
    memory_region_add_subregion_overlap(&s->isa_io, 0x64, &s->kbd_command, 1);
    qdev_connect_gpio_out(DEVICE(kbd), 0,
                          qemu_allocate_irq(jensen_local_irq, s, 2));
    qdev_connect_gpio_out(DEVICE(kbd), 1,
                          qemu_allocate_irq(jensen_local_irq, s, 3));
    serial_mm_init(&s->isa_io, 0x3f8, 0,
                   qemu_allocate_irq(jensen_local_irq, s, 0), 1843200 / 16,
                   serial_hd(0), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(&s->isa_io, 0x2f8, 0,
                   qemu_allocate_irq(jensen_local_irq, s, 1), 1843200 / 16,
                   serial_hd(1), DEVICE_LITTLE_ENDIAN);
    if (s->s3_romfile && !s->s3_928) {
        error_report("s3-romfile requires s3-928=on");
        exit(EXIT_FAILURE);
    }
    if (machine->enable_graphics) {
        if (s->s3_928) {
            vga = isa_new("jensen-s3-928");
            qdev_prop_set_uint32(DEVICE(vga), "vram-size-mb", s->s3_vram_mb);
            isa_realize_and_unref(vga, bus, &error_fatal);
        } else {
            vga = isa_vga_init(bus);
        }
    }
    if (vga) {
        const char *vga_bios = !s->s3_928 && vga_interface_type == VGA_CIRRUS ?
                               "vgabios-cirrus.bin" : "vgabios.bin";

        if (s->s3_928 && s->s3_romfile) {
            vga_bios = s->s3_romfile;
        }

        /* ARC reads the x86 option ROM through the private EISA bus. */
        memory_region_init_rom(&s->vga_rom, OBJECT(vga), "jensen-vga-rom",
                               64 * KiB, &error_fatal);
        memory_region_add_subregion(&s->isa_mem, 0xc0000, &s->vga_rom);
        if (rom_add_file_mr(vga_bios, &s->vga_rom, -1) < 0) {
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < 2; i++) {
        DeviceState *flash = qdev_new(TYPE_PFLASH_CFI01);
        MemoryRegion *mr;

        qdev_prop_set_string(flash, "name",
                             i ? "jensen.system" : "jensen.bootstrap");
        qdev_prop_set_uint32(flash, "num-blocks", 16);
        qdev_prop_set_uint64(flash, "sector-length", 64 * KiB);
        qdev_prop_set_uint8(flash, "width", 1);
        qdev_prop_set_uint8(flash, "device-width", 1);
        qdev_prop_set_uint16(flash, "id0", 0x89);
        qdev_prop_set_uint16(flash, "id1", 0xa2);
        pflash_cfi01_legacy_drive(PFLASH_CFI01(flash), flash_drive[i]);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
        s->flash[i] = PFLASH_CFI01(flash);
        mr = pflash_cfi01_get_memory(s->flash[i]);
        /* A realized drive already contains its image; never overwrite it. */
        if (!flash_drive[i]) {
            if (image) {
                memcpy(memory_region_get_ram_ptr(mr), image, image_len);
            } else if (i == 1) {
                memcpy(memory_region_get_ram_ptr(mr),
                    memory_region_get_ram_ptr(
                        pflash_cfi01_get_memory(s->flash[0])),
                    JENSEN_FLASH_SIZE);
            } else {
                memset(memory_region_get_ram_ptr(mr), 0xff, JENSEN_FLASH_SIZE);
            }
        }
        address_space_init(&s->flash_as[i], mr,
                           i ? "jensen-system" : "jensen-bootstrap");
    }
    if (!flash_drive[0] && !image) {
        memcpy(memory_region_get_ram_ptr(pflash_cfi01_get_memory(s->flash[0])),
               memory_region_get_ram_ptr(pflash_cfi01_get_memory(s->flash[1])),
               JENSEN_FLASH_SIZE);
    }
    /*
     * FEPROM1 holds both system code and configuration sectors. FEPROM0
     * is a provisional bootstrap mirror while the SROM/failsafe path is
     * bypassed. The original V2.2 image must never be blanked as "NVRAM".
     *
     * V2.2 extends the bootstrap image beyond the original 256 KiB ROM.
     * Load the first 512 KiB bank; copying the whole Flash would
     * overwrite the SRM BSS area above the decompressed image.
     */

    jensen_map(s, &s->flash_window[0], &jensen_flash_ops,
                &s->flash_as[0], "jensen-feprom0",
                JENSEN_FLASH_BASE, 512 * MiB);
    jensen_map(s, &s->flash_window[1], &jensen_flash_ops,
                &s->flash_as[1], "jensen-feprom1",
                JENSEN_FLASH1_BASE, 512 * MiB);
    jensen_map(s, &s->local, &jensen_local_ops, s, "jensen-local",
                JENSEN_LOCAL_BASE, 256 * MiB);
    jensen_map(s, &s->eisa_io, &jensen_eisa_io_ops, s, "jensen-eisa-io",
                JENSEN_EISA_IO_BASE, 4 * GiB);
    jensen_map(s, &s->eisa_mem, &jensen_eisa_mem_ops, s, "jensen-eisa-memory",
                JENSEN_EISA_MEM_BASE, 4 * GiB);
    jensen_map(s, &s->inta, &jensen_inta_ops, s, "jensen-inta",
                JENSEN_INTA_BASE, 256 * MiB);
    jensen_map(s, &s->hae_reg, &jensen_reg_ops, &s->hae, "jensen-hae",
                JENSEN_HAE_BASE, 256 * MiB);
    jensen_map(s, &s->sysctl_reg, &jensen_reg_ops, &s->sysctl, "jensen-sysctl",
                JENSEN_SYSCTL_BASE, 256 * MiB);
    jensen_map(s, &s->spare_reg, &jensen_reg_ops, &s->spare, "jensen-spare",
                JENSEN_SPARE_BASE, 256 * MiB);
}

static char *jensen_get_rtc_ram_file(Object *obj, Error **errp)
{
    return g_strdup(JENSEN_MACHINE(obj)->rtc_ram_file ?: "");
}

static void jensen_set_rtc_ram_file(Object *obj, const char *value,
                                    Error **errp)
{
    JensenMachineState *s = JENSEN_MACHINE(obj);

    if (s->rtc) {
        error_setg(errp,
                   "rtc-ram-file must be set before machine initialization");
        return;
    }
    g_free(s->rtc_ram_file);
    s->rtc_ram_file = *value ? g_strdup(value) : NULL;
}

static bool jensen_get_keyboard_locked(Object *obj, Error **errp)
{
    return JENSEN_MACHINE(obj)->keyboard_locked;
}

static void jensen_set_keyboard_locked(Object *obj, bool value, Error **errp)
{
    JENSEN_MACHINE(obj)->keyboard_locked = value;
}

static bool jensen_get_s3_928(Object *obj, Error **errp)
{
    return JENSEN_MACHINE(obj)->s3_928;
}

static void jensen_set_s3_928(Object *obj, bool value, Error **errp)
{
    JENSEN_MACHINE(obj)->s3_928 = value;
}

static void jensen_get_s3_vram_mb(Object *obj, Visitor *v,
                                  const char *name, void *opaque, Error **errp)
{
    uint32_t value = JENSEN_MACHINE(obj)->s3_vram_mb;

    visit_type_uint32(v, name, &value, errp);
}

static void jensen_set_s3_vram_mb(Object *obj, Visitor *v,
                                  const char *name, void *opaque, Error **errp)
{
    JensenMachineState *s = JENSEN_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (s->cpu) {
        error_setg(errp, "s3-vram-mb must be set before machine initialization");
        return;
    }
    if (value != 2 && value != 4) {
        error_setg(errp, "s3-vram-mb must be 2 or 4");
        return;
    }
    s->s3_vram_mb = value;
}

static char *jensen_get_s3_romfile(Object *obj, Error **errp)
{
    return g_strdup(JENSEN_MACHINE(obj)->s3_romfile ?: "");
}

static void jensen_set_s3_romfile(Object *obj, const char *value, Error **errp)
{
    JensenMachineState *s = JENSEN_MACHINE(obj);

    if (s->cpu) {
        error_setg(errp,
                   "s3-romfile must be set before machine initialization");
        return;
    }
    g_free(s->s3_romfile);
    s->s3_romfile = *value ? g_strdup(value) : NULL;
}

static void jensen_instance_init(Object *obj)
{
    JENSEN_MACHINE(obj)->s3_vram_mb = 2;
}

static void jensen_finalize(Object *obj)
{
    JensenMachineState *s = JENSEN_MACHINE(obj);

    if (s->rtc_file) {
        fclose(s->rtc_file);
    }
    g_free(s->rtc_ram_file);
    g_free(s->s3_romfile);
}

static void jensen_class_init(ObjectClass *oc, const void *data)
{
    static const char * const cpu_types[] = {
        ALPHA_CPU_TYPE_NAME("ev4"), NULL
    };
    static GlobalProperty isa_defaults[] = {
        { "isa-i8259", "edge-irq-withdrawal", "on" },
        { "isa-i8259", "ltim-supported", "off" },
        { "mc146818rtc", "pf-without-pie", "on" },
        { "i8042", "selftest-reply-first", "on" },
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    compat_props_add(mc->compat_props, isa_defaults,
                     G_N_ELEMENTS(isa_defaults));
    mc->desc = "DECpc AXP 150 / DEC 2000 (Jensen, experimental)";
    mc->init = jensen_init;
    mc->reset = jensen_reset;
    mc->default_cpu_type = ALPHA_CPU_TYPE_NAME("ev4");
    mc->valid_cpu_types = cpu_types;
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "jensen.ram";
    mc->max_cpus = 1;
    mc->no_floppy = false;
    mc->no_parallel = true;
    object_class_property_add_bool(oc, "s3-928", jensen_get_s3_928,
                                   jensen_set_s3_928);
    object_class_property_set_description(oc, "s3-928",
        "Experimental ISA S3 86C928 graphics (default: off)");
    object_class_property_add(oc, "s3-vram-mb", "uint32",
                              jensen_get_s3_vram_mb, jensen_set_s3_vram_mb,
                              NULL, NULL);
    object_class_property_set_description(oc, "s3-vram-mb",
        "S3 86C928 video memory in MiB: 2 (default) or 4");
    object_class_property_add_str(oc, "s3-romfile", jensen_get_s3_romfile,
                                  jensen_set_s3_romfile);
    object_class_property_set_description(oc, "s3-romfile",
        "Optional original 86C928 VGA option ROM (requires s3-928=on)");
    object_class_property_add_bool(oc, "keyboard-locked",
                                   jensen_get_keyboard_locked,
                                   jensen_set_keyboard_locked);
    object_class_property_set_description(oc, "keyboard-locked",
        "Front-panel key-lock switch (default: unlocked)");
    object_class_property_add_str(oc, "rtc-ram-file", jensen_get_rtc_ram_file,
                                  jensen_set_rtc_ram_file);
    object_class_property_set_description(oc, "rtc-ram-file",
        "File containing 66 bytes of battery-backed VL82C106 RAM");
}

static const TypeInfo jensen_machine_info = {
    .name = TYPE_JENSEN_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(JensenMachineState),
    .instance_init = jensen_instance_init,
    .class_init = jensen_class_init,
    .instance_finalize = jensen_finalize,
};

static void jensen_register_types(void)
{
    type_register_static(&jensen_machine_info);
}

type_init(jensen_register_types)
