/*
 * IBM RS/6000 PowerPC I/O Channel Controller (Micro Channel bridge)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PPC_RS6K_IOCC_H
#define HW_PPC_RS6K_IOCC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RS6K_IOCC "rs6k-iocc"
OBJECT_DECLARE_SIMPLE_TYPE(Rs6kIOCCState, RS6K_IOCC)

#define TYPE_RS6K_IOCC_IOMMU "rs6k-iocc-iommu"

#define IOCC_SLOTS      16
#define IOCC_LEVELS     16
#define IOCC_REGS_SIZE  0x10000
#define IOCC_RAM_SIZE   (1 * MiB)

typedef struct Rs6kIOCCSlot {
    uint16_t id;
    uint8_t pos[8];
    void (*pos_write)(void *opaque, int reg, uint8_t val);
    void *opaque;
} Rs6kIOCCSlot;

/* Interrupt presentation: level is asserted at prio, or withdrawn */
typedef void (*Rs6kIOCCPresent)(void *opaque, int level, uint8_t prio,
                                bool asserted);

struct Rs6kIOCCState {
    SysBusDevice parent_obj;

    MemoryRegion ctl;
    MemoryRegion ctl_ram;
    MemoryRegion ctl_regs;
    MemoryRegion ctl_pos;
    MemoryRegion ctl_unassigned;
    MemoryRegion bus;
    MemoryRegion bus_io;
    MemoryRegion bus_regs;
    MemoryRegion bus_unassigned;
    AddressSpace ctl_as;
    AddressSpace bus_as;
    IOMMUMemoryRegion iommu;
    AddressSpace dma_as;

    Rs6kIOCCSlot slot[IOCC_SLOTS];
    uint32_t regs[IOCC_REGS_SIZE / 4];
    uint16_t lines;
    uint16_t masked;
    Rs6kIOCCPresent present;
    void *present_opaque;
};

void rs6k_iocc_set_slot(Rs6kIOCCState *s, int slot, uint16_t id,
                        void (*pos_write)(void *opaque, int reg, uint8_t val),
                        void *opaque);
void rs6k_iocc_set_presenter(Rs6kIOCCState *s, Rs6kIOCCPresent fn,
                             void *opaque);
void rs6k_iocc_accept(Rs6kIOCCState *s, int level);
void rs6k_iocc_eoi(Rs6kIOCCState *s, int level);
int rs6k_iocc_dma_count(Rs6kIOCCState *s, int arb);
int rs6k_iocc_dma(Rs6kIOCCState *s, int arb, uint8_t *buf, int len,
                  bool to_device, bool *tc);

#endif
