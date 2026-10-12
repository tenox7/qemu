/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DEC 21272 ("Tsunami") PCI host bridge emulation.
 */

#ifndef _ALPHA_TSUNAMI_INTERNAL_H
#define _ALPHA_TSUNAMI_INTERNAL_H

#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/alpha/tsunami.h"
#include "hw/alpha/tsunami-regs.h"
#include "hw/alpha/tigbus.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/core/sysbus.h"

typedef struct TsunamiIOTLBEntry TsunamiIOTLBEntry;
typedef struct TsunamiCPUState TsunamiCPUState;
typedef struct TsunamiDMAWindow TsunamiDMAWindow;
typedef struct TsunamiCchipState TsunamiCchipState;
typedef struct TsunamiDchipState TsunamiDchipState;
typedef struct TsunamiPchipState TsunamiPchipState;
typedef struct TsunamiTigBusState TsunamiTigBusState;

enum tsunami_irq_cause {
    TSUNAMI_IRQ_MAX             = 64,
    TSUNAMI_IRQ_INTERNAL        = 63,
    TSUNAMI_IRQ_PCI0_DEVERROR   = 62,
    TSUNAMI_IRQ_PCI1_DEVERROR   = 61,
    TSUNAMI_IRQ_PCI0_SIO_INT    = 55,
    TSUNAMI_IRQ_PCI0_SMI_INT    = 54,
    TSUNAMI_IRQ_PCI0_NMI        = 53,
};

#define TSUNAMI_PCI_NIRQS       48
#define TSUNAMI_DRAM_SIZE_MAX   32768   /* MiB */
#define TSUNAMI_DRAM_SIZE_MIN   64      /* MiB */
#define TSUNAMI_CPU_MAX         4
#define TSUNAMI_CPU_MASK        (TSUNAMI_CPU_MAX - 1)

/*
 * The bitmap APIs are generally modelled on the generic bitmap.h functions
 * (which are unsuitable here because they use 'unsigned long' as the
 * underlying storage type, which is very awkward when you need to
 * access the data as raw 64-bit values.)
 */
#define TSUNAMI_BIT_MASK(nr)            (1ULL << ((nr) % 64))
#define TSUNAMI_BIT_WORD(nr)            ((nr) / 64)

static inline void tsunami_bmp_set_bit(int nr, uint64_t *addr)
{
    uint64_t mask = TSUNAMI_BIT_MASK(nr);
    uint64_t *p = addr + TSUNAMI_BIT_WORD(nr);

    *p |= mask;
}

static inline void tsunami_bmp_clear_bit(int nr, uint64_t *addr)
{
    uint64_t mask = TSUNAMI_BIT_MASK(nr);
    uint64_t *p = addr + TSUNAMI_BIT_WORD(nr);

    *p &= ~mask;
}

static inline bool tsunami_bmp_test_bit(int nr, const uint64_t *addr)
{
    return 1ULL & (addr[TSUNAMI_BIT_WORD(nr)] >> (nr & 63));
}

static inline void tsunami_bmp_replace_bit(int nr, uint64_t *addr, bool val)
{
    uint64_t mask = TSUNAMI_BIT_MASK(nr);
    uint64_t *p = addr + TSUNAMI_BIT_WORD(nr);

    *p &= ~mask;
    *p |= (val & 1ULL) << (nr % 64);
}

#define TSUNAMI_BITMAP_ACCESSORS(PREFIX, NAME, FIELD)                   \
    static inline void PREFIX##_##NAME##_set(TsunamiState *s, int irq)  \
    {                                                                   \
        tsunami_bmp_set_bit(irq, &s->FIELD);                            \
    }                                                                   \
    static inline bool PREFIX##_##NAME##_test(TsunamiState *s, int irq) \
    {                                                                   \
        return tsunami_bmp_test_bit(irq, &s->FIELD);                    \
    }                                                                   \
    static inline void PREFIX##_##NAME##_clear(TsunamiState *s,         \
                                               int irq)                 \
    {                                                                   \
        tsunami_bmp_clear_bit(irq, &s->FIELD);                          \
    }                                                                   \
    static inline void PREFIX##_##NAME##_replace(TsunamiState *s,       \
                                                 int irq, bool value)   \
    {                                                                   \
        tsunami_bmp_replace_bit(irq, &s->FIELD, value);                 \
    }

#define TSUNAMI_PCPU_BITMAP_ACCESSORS(PREFIX, NAME, FIELD, REG)         \
    static inline void PREFIX##_##NAME##_set(TsunamiState *s, int irq)  \
    {                                                                   \
        int bit = (irq & TSUNAMI_CPU_MASK) + R_##REG##_SHIFT;           \
        tsunami_bmp_set_bit(bit, &s->FIELD);                            \
    }                                                                   \
    static inline bool PREFIX##_##NAME##_test(TsunamiState *s, int irq) \
    {                                                                   \
        int bit = (irq & TSUNAMI_CPU_MASK) + R_##REG##_SHIFT;           \
        return tsunami_bmp_test_bit(bit, &s->FIELD);                    \
    }                                                                   \
    static inline void PREFIX##_##NAME##_clear(TsunamiState *s,         \
                                               int irq)                 \
    {                                                                   \
        int bit = (irq & TSUNAMI_CPU_MASK) + R_##REG##_SHIFT;           \
        tsunami_bmp_clear_bit(bit, &s->FIELD);                          \
    }                                                                   \
    static inline void PREFIX##_##NAME##_replace(TsunamiState *s,       \
                                                 int irq, bool value)   \
    {                                                                   \
        int bit = (irq & TSUNAMI_CPU_MASK) + R_##REG##_SHIFT;           \
        tsunami_bmp_replace_bit(bit, &s->FIELD, value);                 \
    }                                                                   \
    static inline bool PREFIX##_##NAME##_bittest(uint64_t val,          \
                                                 int irq)               \
    {                                                                   \
        int bit = (irq & TSUNAMI_CPU_MASK) + R_##REG##_SHIFT;           \
        return extract64(val, bit, 1);                                  \
    }

#define MERGE_FIELD(dst, src, wmask)                                    \
    do {                                                                \
        (dst) = (((dst) & ~UINT64_C(wmask)) |                           \
                 ((src) & UINT64_C(wmask)));                            \
    } while (0)

#define SET_FIELD(storage, reg, field, val)                             \
    do {                                                                \
        (storage) = FIELD_DP64(storage, reg, field, (val));             \
    } while (0)

#define INIT_FIELD(storage, reg, field, val)                            \
    do {                                                                \
        if (val) {                                                      \
            SET_FIELD(storage, reg, field, val);                        \
        }                                                               \
    } while (0)

/*
 * IOMMU definitions.
 */

#define TYPE_TSUNAMI_IOMMU_MEMORY_REGION "tsunami-iommu-memory-region"
DECLARE_INSTANCE_CHECKER(IOMMUMemoryRegion, TSUNAMI_IOMMU_MEMORY_REGION,
                         TYPE_TSUNAMI_IOMMU_MEMORY_REGION)

#define TSUNAMI_PAGE_SHIFT      13
#define TSUNAMI_PAGE_SIZE       (1ULL << TSUNAMI_PAGE_SHIFT)
#define TSUNAMI_PAGE_SHIFT_4K   12
#define TSUNAMI_PAGE_MASK_4K    MAKE_64BIT_MASK(0, TSUNAMI_PAGE_SHIFT_4K)
#define TSUNAMI_PAGE_SHIFT_8K   13
#define TSUNAMI_PAGE_MASK_8K    MAKE_64BIT_MASK(0, TSUNAMI_PAGE_SHIFT_8K)

#define TSUNAMI_PIO_BASE        BIT_ULL(43)

#define TSUNAMI_PTE_V           0x0000000000000001
#define TSUNAMI_PTE_PTP         0x0000000090000000
#define TSUNAMI_PTE_MASK        MAKE_64BIT_MASK(1, 21)
#define TSUNAMI_SG_MASK         MAKE_64BIT_MASK(13, 7)
#define TSUNAMI_SG_DAC_MASK     MAKE_64BIT_MASK(13, 19)
#define TSUNAMI_MWIN_MASK       MAKE_64BIT_MASK(0, 35)

typedef enum TsunamiIOMMUStatus {
    IOMMU_STATUS_ABORT,
    IOMMU_STATUS_SUCCESS,
} TsunamiIOMMUStatus;

struct TsunamiIOTLBEntry {
    IOMMUTLBEntry entry;
};

static inline bool tsunami_pte_valid(uint64_t pte)
{
    return pte & TSUNAMI_PTE_V;
}

static inline hwaddr tsunami_pte_address(uint64_t pte)
{
    dma_addr_t addr;

    addr = (pte & TSUNAMI_PTE_MASK) << TSUNAMI_PAGE_SHIFT_4K;
    if (pte & TSUNAMI_PTE_PTP) {
        addr |= TSUNAMI_PIO_BASE;
    }
    return addr;
}

static inline dma_addr_t tsunami_iotlb_translate(TsunamiIOTLBEntry *ent,
                                                 dma_addr_t addr)
{
    return ent->entry.translated_addr + (addr & ent->entry.addr_mask);
}

/*
 * Device model definitions.
 */

#define TYPE_TSUNAMI_PCI_HOST_BRIDGE "tsunami-pcihost"
#define TYPE_TSUNAMI_PCI_HOST_BRIDGE_PREFIX "tsunami-pcihost-"
OBJECT_DECLARE_SIMPLE_TYPE(TsunamiPchipState, TSUNAMI_PCI_HOST_BRIDGE)

#define TSUNAMI_TIGBUS_SHIFT    6
#define TSUNAMI_TIGBUS_MASK     MAKE_64BIT_MASK(0, TSUNAMI_TIGBUS_SHIFT)

enum {
    TSUNAMI_IRQ_STAT_DEVICE = 0,
    TSUNAMI_IRQ_STAT_DEVICE_LAST = 63,
    TSUNAMI_IRQ_STAT_TIMER,
    TSUNAMI_IRQ_STAT_IPI,
    TSUNAMI_IRQ_STAT_COUNT,
};

struct TsunamiCPUState {
    TsunamiState *parent;
    CPUState *cpu;

    qemu_irq mchk_irq;
    qemu_irq device_irq;
    qemu_irq timer_irq;
    qemu_irq ipi_irq;
};

struct TsunamiDMAWindow {
    uint64_t wsba;
    uint64_t wsm;
    uint64_t tba;
};

struct TsunamiCchipState {
    MemoryRegion region;
    struct {
        uint64_t csc;
        uint64_t mtr;
        uint64_t misc;
        uint64_t mpd;
        uint64_t aar[4];
        uint64_t dim[4];
        uint64_t mpr[4];
        uint64_t iic[4];
        uint64_t drir;
        uint64_t ttr;
        uint64_t tdr;
        uint64_t pwr;
        uint64_t prbe;
        uint64_t cmonctl[2];
        uint64_t cmoncnt[2];
    } regs;
};

struct TsunamiDchipState {
    MemoryRegion region;
    struct {
        uint64_t dsc;
        uint64_t str;
        uint64_t drev;
        uint64_t dsc2;
    } regs;
};

struct TsunamiPchipState {
    /*< private >*/
    PCIHostState parent_obj;

    /*< public >*/
    TsunamiState *upstream;
    MemoryRegion reg_csr;
    MemoryRegion reg_iack;
    MemoryRegion reg_conf;
    MemoryRegion reg_mem;
    MemoryRegion reg_io;
    IOMMUMemoryRegion iommu;
    AddressSpace iommu_as;
    char bus_path[8];
    uint8_t bus_nr;

    struct {
        TsunamiDMAWindow win[4];
        uint64_t pctl;
        uint64_t plat;
        uint64_t perror;
        uint64_t perrmask;
        uint64_t perrset;
        uint64_t pmonctl;
        uint64_t pmoncnt;
    } regs;
};

struct TsunamiTigBusState {
    MemoryRegion region;
    TigBus *bus;
};

struct TsunamiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    TsunamiCchipState cchip;
    TsunamiDchipState dchip;
    TsunamiPchipState pchip[2];
    TsunamiTigBusState tigbus;
    MemoryRegion iomem;
    MemoryRegion container;
    MemoryRegion blackhole;
    MemoryRegion *aliases;
    TsunamiCPUState *cpus;
    bitbang_i2c_interface i2c_bitbang;
    QEMUTimer *itrigger_timer;
    uint64_t irq_counts[TSUNAMI_IRQ_STAT_COUNT];
    uint32_t first_slot;
    uint32_t ram_size;
    uint32_t num_cpus;
    uint8_t dchip_count;
    uint8_t pchip_count;
};

TSUNAMI_BITMAP_ACCESSORS(cchip, drir, cchip.regs.drir)
TSUNAMI_PCPU_BITMAP_ACCESSORS(cchip, devsup, cchip.regs.misc, MISC_DEVSUP)
TSUNAMI_PCPU_BITMAP_ACCESSORS(cchip, itintr, cchip.regs.misc, MISC_ITINTR)
TSUNAMI_PCPU_BITMAP_ACCESSORS(cchip, ipintr, cchip.regs.misc, MISC_IPINTR)
TSUNAMI_PCPU_BITMAP_ACCESSORS(cchip, ipreq, cchip.regs.misc, MISC_IPREQ)

static inline bool compare_masked(uint64_t x, uint64_t mask)
{
    return (x & mask) == mask;
}

static inline uint64_t dchip_replicate_mask(TsunamiState *s)
{
    switch (s->dchip_count) {
    case 1:
        return UINT64_C(0x0000000000000001);
    case 2:
        return UINT64_C(0x0000000000000101);
    case 4:
        return UINT64_C(0x0000000001010101);
    case 8:
        return UINT64_C(0x0101010101010101);
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t dchip_replicate(TsunamiState *s, uint8_t val)
{
    return (uint64_t)val * dchip_replicate_mask(s);
}

static inline int cchip_aar_mapping(int size)
{
    switch (size) {
    case 16:
        return 0b0001;
    case 32:
        return 0b0010;
    case 64:
        return 0b0011;
    case 128:
        return 0b0100;
    case 256:
        return 0b0101;
    case 512:
        return 0b0110;
    case 1024:
        return 0b0111;
    case 2048:
        return 0b1000;
    case 4096:
        return 0b1001;
    case 8192:
        return 0b1010;
    case 16384:
        return 0b1011;
    case 32768:
        return 0b1100;
    case 65536:
        return 0b1101;
    case 131072:
        return 0b1110;
    case 262144:
        return 0b1111;
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t cchip_iic_decrement(uint64_t iic)
{
    return ((iic - 1) & (R_IIC_ICNT_MASK | R_IIC_OF_MASK)) |
            (iic & R_IIC_OF_MASK);
}

#endif /* _ALPHA_TSUNAMI_INTERNAL_H */
