/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DEC 21272 ("Tsunami") PCI host bridge emulation.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/intc/intc.h"
#include "hw/intc/i8259.h"
#include "system/cpus.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bus.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "tsunami-internal.h"
#include "trace.h"

/*
 * Chipset Implementation
 */

static const Property tsunami_chipset_properties[] = {
    DEFINE_PROP_UINT32("first-slot", TsunamiState, first_slot, 1),
    DEFINE_PROP_UINT32("ram-size", TsunamiState, ram_size, 0), /* MiB */
    DEFINE_PROP_UINT32("num-cpus", TsunamiState, num_cpus, 0),
    DEFINE_PROP_UINT8("dchip-count", TsunamiState, dchip_count, 2),
    DEFINE_PROP_UINT8("pchip-count", TsunamiState, pchip_count, 1),
};

static const struct pchip_base {
    MemMapEntry csr;
    MemMapEntry pci_iack;
    MemMapEntry pci_config;
    MemMapEntry pci_mem;
    MemMapEntry pci_io;
} pchip_memmap[] = {
    {
        .csr        = { TSUNAMI_REGION(PCHIP0),        256 * MiB },
        .pci_iack   = { TSUNAMI_REGION(PCI0_INTACK),    64 * MiB },
        .pci_config = { TSUNAMI_REGION(PCI0_CONFIG),    16 * MiB },
        .pci_mem    = { TSUNAMI_REGION(PCI0_MEM),        4 * GiB },
        .pci_io     = { TSUNAMI_REGION(PCI0_IO),        32 * MiB },
    },
    {
        .csr        = { TSUNAMI_REGION(PCHIP1),        256 * MiB },
        .pci_iack   = { TSUNAMI_REGION(PCI1_INTACK),    64 * MiB },
        .pci_config = { TSUNAMI_REGION(PCI1_CONFIG),    16 * MiB },
        .pci_mem    = { TSUNAMI_REGION(PCI1_MEM),        4 * GiB },
        .pci_io     = { TSUNAMI_REGION(PCI1_IO),        32 * MiB },
    },
};

static void cchip_cpu_irq_update(TsunamiState *s)
{
    int i;

    for (i = 0; i < s->num_cpus; i++) {
        if (s->cchip.regs.dim[i] & s->cchip.regs.drir) {
            qemu_irq_raise(s->cpus[i].device_irq);
        } else {
            qemu_irq_lower(s->cpus[i].device_irq);
        }
    }
}

static void cchip_timer_irq_update(TsunamiState *s, bool level)
{
    int i;

    if (!level) {
        return;
    }

    for (i = 0; i < s->num_cpus; i++) {
        uint64_t iic = cchip_iic_decrement(s->cchip.regs.iic[i]);

        if (FIELD_EX64(iic, IIC, OF)) {
            /* Set the ITI bit for this cpu. */
            cchip_itintr_set(s, i);

            /* And signal the interrupt. */
            trace_tsunami_timer_irq_update(i, 1);
            qemu_irq_raise(s->cpus[i].timer_irq);

            s->irq_counts[TSUNAMI_IRQ_STAT_TIMER]++;
        }

        s->cchip.regs.iic[i] = iic;
    }
}

static void cchip_device_irq_update(TsunamiState *s, int irq, bool level)
{
    int last = cchip_drir_test(s, irq);

    /*
     * 6.3.1 Device and Error Interrupt Delivery:
     *
     * As interrupts are read into the Cchip through the TIGbus, the
     * corresponding bits are set in DRIR. These bits are ANDed with the
     * mask bits in DIMn and then placed in DIRn. If any bits are set
     * in DIRn<55:0>, then CPUn is interrupted using CPU pin b_irq<1>.
     */
    if (last != level) {
        trace_tsunami_device_irq_update(irq, level);
    }
    cchip_drir_replace(s, irq, level);
    cchip_cpu_irq_update(s);

    if (level) {
        s->irq_counts[irq]++;
    }
}

static void cchip_misc_update(TsunamiState *s, uint64_t val)
{
    uint64_t w1c = UINT64_C(0x0000000010000ff0);
    uint64_t w1s = UINT64_C(0x0000000000f00000);
    uint64_t wo = UINT64_C(0x00000f000100f000);
    int i, cpuid = 0;

    if (current_cpu) {
        cpuid = current_cpu->cpu_index;
    }

    s->cchip.regs.misc |= (val & w1s);      /* W1S */
    s->cchip.regs.misc &= ~(val & w1c);     /* W1C */
    s->cchip.regs.misc &= ~(val & wo);      /* WO */

    if (FIELD_EX64(val, MISC, ACL)) {
        trace_tsunami_cchip_arbitration_clear(cpuid);

        /* ACL clears ABT and ABW. */
        SET_FIELD(s->cchip.regs.misc, MISC, ABW, 0);
        SET_FIELD(s->cchip.regs.misc, MISC, ABT, 0);
    }

    if (val & R_MISC_ABW_MASK) {
        if ((s->cchip.regs.misc & (R_MISC_ABW_MASK)) == 0) {
            /* ABW field is W1S iff zero */
            s->cchip.regs.misc |= (val & R_MISC_ABW_MASK);
            trace_tsunami_cchip_arbitration_try(cpuid, "won");
        } else {
            trace_tsunami_cchip_arbitration_try(cpuid, "lost");
        }
    }

    /* Pass on changes to IPI and ITI state.  */
    for (i = 0; i < s->num_cpus; i++) {
        /* ITI can only be cleared by the write. */
        if (val & R_MISC_ITINTR_MASK) {
            if (cchip_itintr_bittest(val, i)) {
                trace_tsunami_timer_irq_update(i, 0);
                qemu_irq_lower(s->cpus[i].timer_irq);
            }
        }

        /* IPI can be either cleared or set by the write. */
        if (val & R_MISC_IPINTR_MASK) {
            if (cchip_ipintr_bittest(val, i)) {
                trace_tsunami_ipi_irq_update(i, 0);
                qemu_irq_lower(s->cpus[i].ipi_irq);
            }
        }
        if (val & R_MISC_IPREQ_MASK) {
            if (cchip_ipreq_bittest(val, i)) {
                trace_tsunami_ipi_irq_update(i, 1);
                qemu_irq_raise(s->cpus[i].ipi_irq);

                s->irq_counts[TSUNAMI_IRQ_STAT_IPI]++;
            }
        }

        /* Handle DEVSUP bits. */
        if (val & R_MISC_DEVSUP_MASK) {
            if (cchip_devsup_bittest(val, i)) {
                uint64_t target = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10;

                trace_tsunami_devsup_irq_set(i);
                qemu_irq_lower(s->cpus[i].device_irq);
                timer_mod(s->itrigger_timer, target);
            }
        }
    }
}

static void cchip_mpd_update(TsunamiState *s, uint64_t val)
{
    bitbang_i2c_interface *i2c = &s->i2c_bitbang;
    bool scl = FIELD_EX64(val, MPD, CKS);
    bool sda = FIELD_EX64(val, MPD, DS);

    SET_FIELD(s->cchip.regs.mpd, MPD, CKR,
              bitbang_i2c_set(i2c, BITBANG_I2C_SCL, scl));
    SET_FIELD(s->cchip.regs.mpd, MPD, DR,
              bitbang_i2c_set(i2c, BITBANG_I2C_SDA, sda));
}

static void cchip_prben_update(TsunamiState *s, int cpu, bool value)
{
    int bit;

    /*
     * 10.2.2.9 Probe Enable Register (PRBEN – RW)
     *
     * This register is special in that reads do not return the value of the
     * register, but rather cause the probe enable bit for the requesting CPU
     * to be cleared. The return data is UNPREDICTABLE. Writing to this
     * register causes the probe enable bit for the requesting CPU to be set,
     * regardless of the value written.
     */
    bit = R_PRBE_PRBEN0_SHIFT + (cpu & TSUNAMI_CPU_MASK);
    s->cchip.regs.prbe = deposit64(s->cchip.regs.prbe, bit, 1, value);
}

static void cchip_reset(TsunamiState *s)
{
    ram_addr_t ram_size, ram_base;
    int i, rev, nbanks = 0;

    rev = s->num_cpus > 2 ? MISC_REV_TYPHOON : MISC_REV_TSUNAMI;

    /*
     * Initialization for CSC. (Table 10-10)
     */
    INIT_FIELD(s->cchip.regs.csc, CSC, P1W, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, P0W, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, PBQMAX, 1);
    INIT_FIELD(s->cchip.regs.csc, CSC, PRQMAX, 2);
    INIT_FIELD(s->cchip.regs.csc, CSC, PDTMAX, 1);
    INIT_FIELD(s->cchip.regs.csc, CSC, FPQCMAX, 1);
    INIT_FIELD(s->cchip.regs.csc, CSC, TPQMMAX, 1);
    INIT_FIELD(s->cchip.regs.csc, CSC, B3D, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, B2D, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, B1D, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, FTI, CSC_FTI_DISABLED);
    INIT_FIELD(s->cchip.regs.csc, CSC, EFT, CSC_EFT_1_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, QDI, CSC_QDI_DISABLE_DRAINING);
    INIT_FIELD(s->cchip.regs.csc, CSC, QPM, CSC_QPM_ROUND_ROBIN);
    INIT_FIELD(s->cchip.regs.csc, CSC, PME, CSC_PME_DISABLED);
    INIT_FIELD(s->cchip.regs.csc, CSC, DRTP, CSC_DRTP_5_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, DWFP, CSC_DWFP_5_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, DWTP, CSC_DWTP_5_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, P1P, s->pchip_count == 2);
    INIT_FIELD(s->cchip.regs.csc, CSC, IDDW, CSC_IDDW_6_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, IDDR, CSC_IDDR_9_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, AW, CSC_AW_16_BYTES);
    INIT_FIELD(s->cchip.regs.csc, CSC, FW, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, SFD, CSC_SFD_2_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, SED, CSC_SED_2_CYCLES);
    INIT_FIELD(s->cchip.regs.csc, CSC, C0CFP, 0);
    INIT_FIELD(s->cchip.regs.csc, CSC, C1CFP, 0);

    switch (s->dchip_count) {
    case 2:
        /* 2 Dchips, 1 memory bus */
        INIT_FIELD(s->cchip.regs.csc, CSC, BC, CSC_BC_2D_1M);
        break;
    case 4:
        /* 4 Dchips, 2 memory busses */
        INIT_FIELD(s->cchip.regs.csc, CSC, BC, CSC_BC_4D_2M);
        break;
    case 8:
        /* 8 Dchips, 2 memory buses */
        INIT_FIELD(s->cchip.regs.csc, CSC, BC, CSC_BC_8D_2M);
        break;
    default:
        g_assert_not_reached();
    }

    /*
     * Initialization for MTR. (Table 10-11)
     */
    INIT_FIELD(s->cchip.regs.mtr, MTR, MPH, 0);
    INIT_FIELD(s->cchip.regs.mtr, MTR, PHCW, 14);
    INIT_FIELD(s->cchip.regs.mtr, MTR, PHCR, 15);
    INIT_FIELD(s->cchip.regs.mtr, MTR, RI, 0);
    INIT_FIELD(s->cchip.regs.mtr, MTR, MPD, MTR_MPD_NO_DELAY);
    INIT_FIELD(s->cchip.regs.mtr, MTR, RRD, MTR_RRD_2_CYCLES);
    INIT_FIELD(s->cchip.regs.mtr, MTR, RPT, MTR_RPT_2_CYCLES);
    INIT_FIELD(s->cchip.regs.mtr, MTR, RPW, MTR_RPW_4_CYCLES);
    INIT_FIELD(s->cchip.regs.mtr, MTR, IRD, MTR_IRD_0_CYCLES);
    INIT_FIELD(s->cchip.regs.mtr, MTR, CAT, MTR_CAT_2_CYCLES);
    INIT_FIELD(s->cchip.regs.mtr, MTR, RCD, MTR_RCD_2_CYCLES);

    /*
     * Initialization for MISC. (Table 10-12)
     */
    INIT_FIELD(s->cchip.regs.misc, MISC, DEVSUP, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, REV, rev);
    INIT_FIELD(s->cchip.regs.misc, MISC, NXS, MISC_NXS_CPU0);
    INIT_FIELD(s->cchip.regs.misc, MISC, NXM, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, ACL, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, ABT, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, ABW, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, IPREQ, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, IPINTR, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, ITINTR, 0);
    INIT_FIELD(s->cchip.regs.misc, MISC, CPUID, 0);

    /*
     * Initialization for MPD. (Table 10-13)
     */
    INIT_FIELD(s->cchip.regs.mpd, MPD, DR, 1);
    INIT_FIELD(s->cchip.regs.mpd, MPD, CKR, 1);
    INIT_FIELD(s->cchip.regs.mpd, MPD, DS, 1);
    INIT_FIELD(s->cchip.regs.mpd, MPD, CKS, 1);

    /*
     * Initialization for AAR0, AAR1, AAR2, AAR3. (Table 10-15)
     */
    ram_size = s->ram_size;
    ram_base = 0;
    while (ram_size >= 4 && nbanks < 4) {
        ram_addr_t len = BIT(MIN(31 - clz32(ram_size), 13));
        int map = cchip_aar_mapping(len);

        s->cchip.regs.aar[nbanks] |= (ram_base << 20) & R_AAR_ADDR_MASK;
        INIT_FIELD(s->cchip.regs.aar[nbanks], AAR, ASIZ, map);
        INIT_FIELD(s->cchip.regs.aar[nbanks], AAR, DBG, AAR_DBG_DISABLED);
        INIT_FIELD(s->cchip.regs.aar[nbanks], AAR, TSA, AAR_TSA_DISABLED);
        INIT_FIELD(s->cchip.regs.aar[nbanks], AAR, SA, AAR_SA_DISABLED);
        INIT_FIELD(s->cchip.regs.aar[nbanks], AAR, ROWS, AAR_ROWS_11_BITS);
        INIT_FIELD(s->cchip.regs.aar[nbanks], AAR, BNKS, AAR_BNKS_1_BITS);
        ram_size -= len;
        ram_base += len;
        nbanks++;
    }

    /*
     * Initialization for DIM0, DIM1, DIM2, DIM3. (Table 10-16)
     */
    for (i = 0; i < ARRAY_SIZE(s->cchip.regs.dim); i++) {
        s->cchip.regs.dim[i] = 0;
    }

    /*
     * Initialization for DRIR. (Table 10-18)
     */
    INIT_FIELD(s->cchip.regs.drir, DIR, DEV, 0);

    /*
     * Initialization for PRBEN. (Table 10-19)
     */
    INIT_FIELD(s->cchip.regs.prbe, PRBE, PRBEN0, PRBE_PRBEN_DISABLED);
    INIT_FIELD(s->cchip.regs.prbe, PRBE, PRBEN1, PRBE_PRBEN_DISABLED);
    INIT_FIELD(s->cchip.regs.prbe, PRBE, PRBEN2, PRBE_PRBEN_DISABLED);
    INIT_FIELD(s->cchip.regs.prbe, PRBE, PRBEN3, PRBE_PRBEN_DISABLED);

    /*
     * Initialization for IIC0, IIC1, IIC2, IIC3. (Table 10-20)
     */
    for (i = 0; i < ARRAY_SIZE(s->cchip.regs.iic); i++) {
        INIT_FIELD(s->cchip.regs.iic[i], IIC, OF, IIC_OF_POSITIVE);
    }

    /*
     * Initialization for MPR0, MPR1, MPR2, MPR3 (Table 10-22)
     */
    for (i = 0; i < ARRAY_SIZE(s->cchip.regs.mpr); i++) {
        s->cchip.regs.mpr[i] = 0;
    }

    /*
     * Initialization for TTR. (Table 10-23)
     */
    INIT_FIELD(s->cchip.regs.ttr, TTR, ID, 7);
    INIT_FIELD(s->cchip.regs.ttr, TTR, IRT, TTR_IRT_4_CYCLES);
    INIT_FIELD(s->cchip.regs.ttr, TTR, IS, TTR_IS_4_CYCLES);
    INIT_FIELD(s->cchip.regs.ttr, TTR, AH, TTR_AH_1_CYCLES);
    INIT_FIELD(s->cchip.regs.ttr, TTR, AS, TTR_AS_1_CYCLES);

    /*
     * Initialization for TDR. (Table 10-24)
     */
    INIT_FIELD(s->cchip.regs.tdr, TDR, WH3, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WP3, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WS3, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, RA3, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WH2, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WP2, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WS2, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, RA2, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WH1, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WP1, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WS1, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, RA1, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WH0, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WP0, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, WS0, 0);
    INIT_FIELD(s->cchip.regs.tdr, TDR, RA0, 0);

    /*
     * Initialization for PWR. (Table 10-25)
     */
    INIT_FIELD(s->cchip.regs.pwr, PWR, SR, PWR_SR_NORMAL);

    /*
     * Initialization for CMONCTLA. (Table 10-26)
     */
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, MSK23, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, MSK01, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, STKDIS3,
               CMONCTLA_STKDIS_ALL_ONES);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, STKDIS2,
               CMONCTLA_STKDIS_ALL_ONES);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, STKDIS1,
               CMONCTLA_STKDIS_ALL_ONES);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, STKDIS0,
               CMONCTLA_STKDIS_ALL_ONES);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, SLCTMBL,
               CMONCTLA_SLCTMBL_MGROUP0);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, SLCT3, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, SLCT2, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, SLCT1, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[0], CMONCTLA, SLCT1, 0);

    /*
     * Initialization for CMONCTLB. (Table 10-27)
     */
    INIT_FIELD(s->cchip.regs.cmonctl[1], CMONCTLB, MTE3, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[1], CMONCTLB, MTE2, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[1], CMONCTLB, MTE1, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[1], CMONCTLB, MTE0, 0);
    INIT_FIELD(s->cchip.regs.cmonctl[1], CMONCTLB, DIS, CMONCTLB_DIS_IN_USE);

    /*
     * Initialization for CMONCNT01. (Table 10-29)
     */
    INIT_FIELD(s->cchip.regs.cmoncnt[0], CMONCNT01, ECNT1, 0);
    INIT_FIELD(s->cchip.regs.cmoncnt[0], CMONCNT01, ECNT0, 0);

    /*
     * Initialization for CMONCNT23. (Table 10-30)
     */
    INIT_FIELD(s->cchip.regs.cmoncnt[0], CMONCNT23, ECNT3, 0);
    INIT_FIELD(s->cchip.regs.cmoncnt[0], CMONCNT23, ECNT2, 0);
}

static uint64_t cchip_read(void *opaque, hwaddr addr, unsigned size)
{
    TsunamiState *s = opaque;
    uint64_t ret;
    int cpuid = 0;

    if (current_cpu) {
        cpuid = current_cpu->cpu_index;
    }

    switch (addr) {
    case A_CCHIP_CSC:
        /* CSC: Cchip System Configuration Register. */
        ret = s->cchip.regs.csc;
        break;

    case A_CCHIP_MTR:
        /* MTR: Memory Timing Register. */
        ret = s->cchip.regs.mtr;
        break;

    case A_CCHIP_MISC:
        /* MISC: Miscellaneous Register. */
        ret = s->cchip.regs.misc;
        SET_FIELD(ret, MISC, CPUID, cpuid & TSUNAMI_CPU_MASK);
        break;

    case A_CCHIP_MPD:
        /* MPD: Memory Presence Detect Register. */
        ret = s->cchip.regs.mpd;
        break;

    case A_CCHIP_AAR0:
        /* AAR: Array Address Register. */
        ret = s->cchip.regs.aar[0];
        break;
    case A_CCHIP_AAR1:
        ret = s->cchip.regs.aar[1];
        break;
    case A_CCHIP_AAR2:
        ret = s->cchip.regs.aar[2];
        break;
    case A_CCHIP_AAR3:
        ret = s->cchip.regs.aar[3];
        break;

    case A_CCHIP_DIM0:
        /* DIM0: Device Interrupt Mask Register. */
        ret = s->cchip.regs.dim[0];
        break;
    case A_CCHIP_DIM1:
        ret = s->cchip.regs.dim[1];
        break;
    case A_CCHIP_DIM2:
        ret = s->cchip.regs.dim[2];
        break;
    case A_CCHIP_DIM3:
        ret = s->cchip.regs.dim[3];
        break;

    case A_CCHIP_DIR0:
        /* DIR0: Device Interrupt Request Register. */
        ret = s->cchip.regs.dim[0] & s->cchip.regs.drir;
        break;
    case A_CCHIP_DIR1:
        ret = s->cchip.regs.dim[1] & s->cchip.regs.drir;
        break;
    case A_CCHIP_DIR2:
        ret = s->cchip.regs.dim[2] & s->cchip.regs.drir;
        break;
    case A_CCHIP_DIR3:
        ret = s->cchip.regs.dim[3] & s->cchip.regs.drir;
        break;

    case A_CCHIP_DRIR:
        /* DRIR: Device Raw Interrupt Request Register. */
        ret = s->cchip.regs.drir;
        break;

    case A_CCHIP_PRBEN:
        /* PRBEN: Probe Enable Register. */
        cchip_prben_update(s, cpuid, 0);
        ret = 0;
        break;

    case A_CCHIP_IIC0:
        /* IIC: Interval Ignore Count Register. */
        ret = s->cchip.regs.iic[0];
        break;
    case A_CCHIP_IIC1:
        ret = s->cchip.regs.iic[1];
        break;
    case A_CCHIP_IIC2:
        ret = s->cchip.regs.iic[2];
        break;
    case A_CCHIP_IIC3:
        ret = s->cchip.regs.iic[3];
        break;

    case A_CCHIP_MPR0:
        /* MPR: Memory Programming Register. */
        ret = s->cchip.regs.mpr[0];
        break;
    case A_CCHIP_MPR1:
        ret = s->cchip.regs.mpr[1];
        break;
    case A_CCHIP_MPR2:
        ret = s->cchip.regs.mpr[2];
        break;
    case A_CCHIP_MPR3:
        ret = s->cchip.regs.mpr[3];
        break;

    case A_CCHIP_TTR:
        /* TTR: TIGbus Timing Register. */
        ret = s->cchip.regs.ttr;
        break;

    case A_CCHIP_TDR:
        /* TDR: TIGbus Device Timing Register. */
        ret = s->cchip.regs.tdr;
        break;

    case A_CCHIP_PWR:
        /* PWR: Power Management Control Register. */
        ret = s->cchip.regs.pwr;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid offset 0x%04" HWADDR_PRIx "\n",
                      __func__, addr);
        ret = 0;
        break;
    }

    trace_tsunami_cchip_read(addr, ret, size);
    return ret;
}

static void cchip_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    TsunamiState *s = opaque;
    int cpuid = 0;

    if (current_cpu) {
        cpuid = current_cpu->cpu_index;
    }

    trace_tsunami_cchip_write(addr, val, size);

    switch (addr) {
    case A_CCHIP_CSC:
        /* CSC: Cchip System Configuration Register. */
        MERGE_FIELD(s->cchip.regs.csc, val, 0x077777ffff3f0000);
        break;

    case A_CCHIP_MISC:
        cchip_misc_update(s, val);
        break;

    case A_CCHIP_MPD:
        cchip_mpd_update(s, val);
        break;

    case A_CCHIP_AAR0:
        /* AAR: Array Address Register. */
        MERGE_FIELD(s->cchip.regs.aar[0], val, 0x00000007ff01f30f);
        break;
    case A_CCHIP_AAR1:
        MERGE_FIELD(s->cchip.regs.aar[1], val, 0x00000007ff01f30f);
        break;
    case A_CCHIP_AAR2:
        MERGE_FIELD(s->cchip.regs.aar[2], val, 0x00000007ff01f30f);
        break;
    case A_CCHIP_AAR3:
        MERGE_FIELD(s->cchip.regs.aar[3], val, 0x00000007ff01f30f);
        break;

    case A_CCHIP_DIM0:
        /* DIM0: Device Interrupt Mask Register. */
        s->cchip.regs.dim[0] = val;
        cchip_cpu_irq_update(s);
        break;
    case A_CCHIP_DIM1:
        s->cchip.regs.dim[1] = val;
        cchip_cpu_irq_update(s);
        break;
    case A_CCHIP_DIM2:
        s->cchip.regs.dim[2] = val;
        cchip_cpu_irq_update(s);
        break;
    case A_CCHIP_DIM3:
        s->cchip.regs.dim[3] = val;
        cchip_cpu_irq_update(s);
        break;

    case A_CCHIP_PRBEN:
        /* PRBEN: Probe Enable Register. */
        cchip_prben_update(s, cpuid, 1);
        break;

    case A_CCHIP_IIC0:
        /* IIC: Interval Ignore Count Register. */
        s->cchip.regs.iic[0] = val & R_IIC_ICNT_MASK;
        break;
    case A_CCHIP_IIC1:
        s->cchip.regs.iic[1] = val & R_IIC_ICNT_MASK;
        break;
    case A_CCHIP_IIC2:
        s->cchip.regs.iic[2] = val & R_IIC_ICNT_MASK;
        break;
    case A_CCHIP_IIC3:
        s->cchip.regs.iic[3] = val & R_IIC_ICNT_MASK;
        break;

    case A_CCHIP_MPR0:
        /* MPR: Memory Programming Register. */
        MERGE_FIELD(s->cchip.regs.mpr[0], val, 0x0000000000003fff);
        break;
    case A_CCHIP_MPR1:
        MERGE_FIELD(s->cchip.regs.mpr[1], val, 0x0000000000003fff);
        break;
    case A_CCHIP_MPR2:
        MERGE_FIELD(s->cchip.regs.mpr[2], val, 0x0000000000003fff);
        break;
    case A_CCHIP_MPR3:
        MERGE_FIELD(s->cchip.regs.mpr[3], val, 0x0000000000003fff);
        break;

    case A_CCHIP_TTR:
        /* TTR: TIGbus Timing Register. */
        MERGE_FIELD(s->cchip.regs.ttr, val, 0x0000000000007333);
        break;

    case A_CCHIP_TDR:
        /* TDR: TIGbus Device Timing Register. */
        MERGE_FIELD(s->cchip.regs.tdr, val, 0xf37ff37ff37ff37f);
        break;

    case A_CCHIP_PWR:
        /* PWR: Power Management Control Register. */
        MERGE_FIELD(s->cchip.regs.pwr, val, 0x0000000000000001);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid offset 0x%04" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps cchip_ops = {
    .read = cchip_read,
    .write = cchip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

static void dchip_reset(TsunamiState *s)
{
    uint64_t dsc = 0;
    uint64_t str = extract64(s->cchip.regs.csc, 8, 6);

    /*
     * 10.2.4.1 Dchip System Configuration Register:
     *
     * This is an 8-bit register that mirrors some information in CSC. It is
     * special, however, in that it is byte-sliced across eight Dchips.
     * Therefore, it is read as a quadword with the same value repeated in
     * all eight bytes.
     */
    INIT_FIELD(dsc, DSC, P1P, FIELD_EX64(s->cchip.regs.csc, CSC, P1P));
    INIT_FIELD(dsc, DSC, BC, FIELD_EX64(s->cchip.regs.csc, CSC, BC));
    INIT_FIELD(dsc, DSC, C0CFP, FIELD_EX64(s->cchip.regs.csc, CSC, C0CFP));
    INIT_FIELD(dsc, DSC, C1CFP, FIELD_EX64(s->cchip.regs.csc, CSC, C1CFP));
    INIT_FIELD(dsc, DSC, C2CFP, 0);
    INIT_FIELD(dsc, DSC, C3CFP, 0);
    s->dchip.regs.dsc = dchip_replicate(s, dsc);

    /*
     * 10.2.4.3 System Timing Register:
     *
     * This is an 8-bit register corresponding to bits CSC<13:8>. It is
     * special, however, in that it must be written to up to eight Dchips
     * simultaneously. Therefore, it is written as a quadword with the same
     * value repeated in all eight bytes. That way, all Dchips are configured
     * properly regardless of system configuration.
     */
    s->dchip.regs.str = dchip_replicate(s, str);

    /* 1 is the latest revision. */
    s->dchip.regs.drev = dchip_replicate(s, 0x01);
}

static uint64_t dchip_read(void *opaque, hwaddr addr, unsigned size)
{
    TsunamiState *s = opaque;
    uint64_t ret;

    switch (addr) {
    case A_DCHIP_DSC:
        /* DSC: Dchip System Configuration Register. */
        ret = s->dchip.regs.dsc;
        break;

    case A_DCHIP_STR:
        /* STR: System Timing Register. */
        ret = s->dchip.regs.str;
        break;

    case A_DCHIP_DREV:
        /* DREV: Dchip Revision Register. */
        ret = s->dchip.regs.drev;
        break;

    case A_DCHIP_DSC2:
        /* DSC2: Dchip System Configuration Register 2. */
        ret = s->dchip.regs.dsc2;
        break;

    default:
        ret = 0;
        break;
    }

    trace_tsunami_dchip_read(addr, ret, size);
    return ret;
}

static void dchip_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    /* Skip this. All registers are read-only. */
    trace_tsunami_dchip_write(addr, val, size);
}

static const MemoryRegionOps dchip_ops = {
    .read = dchip_read,
    .write = dchip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

static inline hwaddr tigbus_io_address(hwaddr addr)
{
    return addr >> TSUNAMI_TIGBUS_SHIFT;
}

static uint64_t tigbus_read(void *opaque, hwaddr addr, unsigned size)
{
    TsunamiState *s = opaque;
    TigBus *tb = s->tigbus.bus;
    uint8_t buf[8];

    if (addr & TSUNAMI_TIGBUS_MASK) {
        return 0;
    }

    address_space_read(&tb->downstream_as, tigbus_io_address(addr),
                       MEMTXATTRS_UNSPECIFIED, buf, size);
    switch (size) {
    case 1:
        return buf[0];
    case 2:
        return lduw_le_p(buf);
    case 4:
        return ldl_le_p(buf);
    case 8:
        return ldq_le_p(buf);
    default:
        g_assert_not_reached();
    }
}

static void tigbus_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    TsunamiState *s = opaque;
    TigBus *tb = s->tigbus.bus;
    uint8_t buf[8];

    if (addr & TSUNAMI_TIGBUS_MASK) {
        return;
    }

    switch (size) {
    case 1:
        buf[0] = val;
        break;
    case 2:
        stw_le_p(buf, val);
        break;
    case 4:
        stl_le_p(buf, val);
        break;
    case 8:
        stq_le_p(buf, val);
        break;
    default:
        g_assert_not_reached();
    }

    address_space_write(&tb->downstream_as, tigbus_io_address(addr),
                        MEMTXATTRS_UNSPECIFIED, buf, size);
}

static const MemoryRegionOps tigbus_ops = {
    .read = tigbus_read,
    .write = tigbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t blackhole_read(void *ptr, hwaddr addr, unsigned size)
{
    return UINT64_C(~0);
}

static void blackhole_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned width)
{
    trace_tsunami_blackhole_write(addr, val, width);
}

static const MemoryRegionOps blackhole_ops = {
    .read = blackhole_read,
    .write = blackhole_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

int tsunami_pci_root_bus_index(PCIBus *bus)
{
    TsunamiPchipState *pcs = TSUNAMI_PCI_HOST_BRIDGE(BUS(bus)->parent);

    assert(pci_bus_is_root(bus));
    return pcs->bus_nr;
}

PCIBus *tsunami_get_pci_bus(DeviceState *dev, int n)
{
    TsunamiState *s = TSUNAMI_CHIPSET(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(&s->pchip[n]);

    assert(n < s->pchip_count);
    return phb->bus;
}

TigBus *tsunami_get_tig_bus(DeviceState *dev)
{
    TsunamiState *s = TSUNAMI_CHIPSET(dev);

    return s->tigbus.bus;
}

static void tsunami_timer_irq_handler(void *opaque, int irq, int level)
{
    TsunamiState *s = opaque;

    cchip_timer_irq_update(s, level);
}

static void tsunami_external_irq_handler(void *opaque, int irq, int level)
{
    TsunamiState *s = opaque;

    cchip_device_irq_update(s, TSUNAMI_IRQ_PCI0_SIO_INT, level);
}

static void tsunami_itrigger_handler(void *opaque)
{
    TsunamiState *s = opaque;

    trace_tsunami_devsup_irq_trigger();
    cchip_cpu_irq_update(s);
}

static void tsunami_chipset_realize(DeviceState *dev, Error **errp)
{
    const hwaddr aliases[] = {
        0x0000000000000000,
        0x0000010000000000,
        0x0000030000000000,
        0x0000070000000000,
    };
    TsunamiState *s = TSUNAMI_CHIPSET(dev);
    ram_addr_t ram_size = s->ram_size;
    int i, nbanks = 0;

    if (unlikely(!s->ram_size)) {
        error_setg(errp, "%s: 'ram-size' property is not set", __func__);
        return;
    }

    while (ram_size >= 4 && nbanks < 4) {
        ram_size -= BIT(MIN(31 - clz32(ram_size), 13));
        nbanks++;
    }

    if (unlikely(ram_size != 0 ||
                 s->ram_size < TSUNAMI_DRAM_SIZE_MIN ||
                 s->ram_size > TSUNAMI_DRAM_SIZE_MAX)) {
        g_autofree char *sz = size_to_str(s->ram_size * MiB);
        g_autofree char *min_sz = size_to_str(TSUNAMI_DRAM_SIZE_MIN * MiB);
        g_autofree char *max_sz = size_to_str(TSUNAMI_DRAM_SIZE_MAX * MiB);

        error_setg(errp, "%s: unsupported DRAM size: %s", __func__, sz);
        error_append_hint(errp,
                          "DRAM size must be evenly divisible and be between "
                          "%s and %s\n", min_sz, max_sz);
        return;
    }

    /*
     * Table 2–1: System Configurations:
     *
     *   Number of    Number of    Number of    Number of CPUs
     *   Cchips       Dchips       Pchips
     *   -----------  -----------  -----------  ----------------
     *   1            2            1            1
     *   1            4            1 or 2       1 or 2
     *   1            4            1 or 2       1 or 2
     *   1            8            1 or 2       1 or 2
     *   1            8            1 or 2       4
     */

    if (unlikely(!is_power_of_2(s->dchip_count) ||
                 s->dchip_count < 2 || s->dchip_count > 8)) {
        error_setg(errp, "%s: unsupported dchip count (%u)",
                   __func__, s->dchip_count);
        error_append_hint(errp, "dchip count must be between 2 and 8\n");
        return;
    }

    if (unlikely(s->pchip_count != 1 && s->pchip_count != 2)) {
        error_setg(errp, "%s: unsupported pchip count (%u)",
                   __func__, s->pchip_count);
        error_append_hint(errp, "pchip count must be between 1 and 2\n");
        return;
    }

    if (unlikely(s->num_cpus < 1 || s->num_cpus > TSUNAMI_CPU_MAX)) {
        error_setg(errp, "%s: unsupported cpu count (%u)",
                   __func__, s->num_cpus);
        error_append_hint(errp, "num-cpus count must be between 1 and %d\n",
                          TSUNAMI_CPU_MAX);
        return;
    }

    s->itrigger_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     tsunami_itrigger_handler, s);

    /* MMIO space. */
    memory_region_init(&s->iomem, OBJECT(s), "tsunami.iomem", 8 * TiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /* Container. */
    memory_region_init(&s->container, OBJECT(s), "tsunami.container",
                       32 * GiB);

    /*
     * Technically, the Tsunami manual states that PA<42:35> are don't
     * cares, but we can't make that many aliases. Make aliases down to
     * PA<42:40> instead. NetBSD and Tru64 seem fine with this.
     */
    s->aliases = g_new0(MemoryRegion, ARRAY_SIZE(aliases));
    for (i = 0; i < ARRAY_SIZE(aliases); i++) {
        memory_region_init_alias(&s->aliases[i], OBJECT(s), "tsunami.alias",
                                 &s->container, 0,
                                 memory_region_size(&s->container));
        memory_region_add_subregion(&s->iomem, aliases[i], &s->aliases[i]);
    }

    /* Pchip initialization. */
    memory_region_init_io(&s->blackhole, OBJECT(s), &blackhole_ops, s,
                          "blackhole", 32 * GiB);
    memory_region_add_subregion_overlap(&s->container, 0, &s->blackhole, -1);

    for (i = 0; i < s->pchip_count; i++) {
        const struct pchip_base *memmap = &pchip_memmap[i];
        TsunamiPchipState *pchip = &s->pchip[i];

        object_initialize_child(OBJECT(dev), "pchip[*]", pchip,
                                TYPE_TSUNAMI_PCI_HOST_BRIDGE);
        object_property_set_int(OBJECT(pchip), "bus_nr", i, &error_fatal);
        object_property_set_link(OBJECT(pchip), "upstream", OBJECT(s),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(pchip), errp)) {
            return;
        }

        /* Map all Pchip memory regions. */
        memory_region_add_subregion_overlap(&s->container,
                                            memmap->csr.base,
                                            &pchip->reg_csr, 0);
        memory_region_add_subregion_overlap(&s->container,
                                            memmap->pci_iack.base,
                                            &pchip->reg_iack, 0);
        memory_region_add_subregion_overlap(&s->container,
                                            memmap->pci_config.base,
                                            &pchip->reg_conf, 0);
        memory_region_add_subregion_overlap(&s->container,
                                            memmap->pci_mem.base,
                                            &pchip->reg_mem, 0);
        memory_region_add_subregion_overlap(&s->container,
                                            memmap->pci_io.base,
                                            &pchip->reg_io, 0);
    }

    /* Cchip CSRs, 256MB. */
    memory_region_init_io(&s->cchip.region, OBJECT(s), &cchip_ops, s,
                          "cchip", 256 * MiB);
    memory_region_add_subregion_overlap(&s->container, TSUNAMI_REGION(CCHIP),
                                        &s->cchip.region, 0);

    /* Dchip CSRs, 256MB. */
    memory_region_init_io(&s->dchip.region, OBJECT(s), &dchip_ops, s,
                          "dchip", 256 * MiB);
    memory_region_add_subregion_overlap(&s->container, TSUNAMI_REGION(DCHIP),
                                        &s->dchip.region, 0);

    /* TIGbus, 1GB. */
    s->tigbus.bus = tigbus_create_bus(dev, "tigbus");
    memory_region_init_io(&s->tigbus.region, OBJECT(s), &tigbus_ops, s,
                          "tigbus", 1024 * MiB);
    memory_region_add_subregion_overlap(&s->container, TSUNAMI_REGION(TIGBUS),
                                        &s->tigbus.region, 0);

    /* Initialize the CPU interfaces. */
    s->cpus = g_new0(TsunamiCPUState, s->num_cpus);
    for (i = 0; i < s->num_cpus; i++) {
        CPUState *cpu = qemu_get_cpu(i);

        s->cpus[i].parent = s;
        s->cpus[i].cpu = cpu;
        qdev_init_gpio_out_named(dev, &s->cpus[i].mchk_irq, "cpu-mchk", 1);
        qdev_init_gpio_out_named(dev, &s->cpus[i].device_irq, "cpu-device", 1);
        qdev_init_gpio_out_named(dev, &s->cpus[i].timer_irq, "cpu-timer", 1);
        qdev_init_gpio_out_named(dev, &s->cpus[i].ipi_irq, "cpu-ipi", 1);
    }

    /* Initialize external interfaces. */
    bitbang_i2c_init(&s->i2c_bitbang, i2c_init_bus(dev, "i2c"));

    qdev_init_gpio_in_named(dev, tsunami_timer_irq_handler, "timer", 1);
    qdev_init_gpio_in_named(dev, tsunami_external_irq_handler, "external", 1);
}

static void tsunami_chipset_reset(DeviceState *dev)
{
    TsunamiState *s = TSUNAMI_CHIPSET(dev);
    int i;

    memset(&s->cchip.regs, 0, sizeof(s->cchip.regs));
    memset(&s->dchip.regs, 0, sizeof(s->dchip.regs));
    cchip_reset(s);
    dchip_reset(s);

    for (i = 0; i < ARRAY_SIZE(s->irq_counts); i++) {
        s->irq_counts[i] = 0;
    }
}

static bool tsunami_chipset_get_statistics(InterruptStatsProvider *obj,
                                           uint64_t **irq_counts,
                                           unsigned int *nb_irqs)
{
    TsunamiState *s = TSUNAMI_CHIPSET(obj);

    *irq_counts = s->irq_counts;
    *nb_irqs = ARRAY_SIZE(s->irq_counts);
    return true;
}

static void tsunami_chipset_print_info(InterruptStatsProvider *obj,
                                       GString *buf)
{
    TsunamiState *s = TSUNAMI_CHIPSET(obj);
    int i;

    g_string_append_printf(buf, "tsunami: request=0x%016" PRIx64 "\n",
                           s->cchip.regs.drir);
    for (i = 0; i < s->num_cpus; i++) {
        g_string_append_printf(buf, "  cpu %d: unmasked=0x%016" PRIx64
                               " pending=0x%016" PRIx64"\n", i,
                               s->cchip.regs.dim[i],
                               s->cchip.regs.dim[i] & s->cchip.regs.drir);
    }
}

static const VMStateDescription vmstate_tsunami_chipset = {
    .name = TYPE_TSUNAMI_CHIPSET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(cchip.regs.csc, TsunamiState),
        VMSTATE_UINT64(cchip.regs.mtr, TsunamiState),
        VMSTATE_UINT64(cchip.regs.misc, TsunamiState),
        VMSTATE_UINT64(cchip.regs.mpd, TsunamiState),
        VMSTATE_UINT64_ARRAY(cchip.regs.aar, TsunamiState, 4),
        VMSTATE_UINT64_ARRAY(cchip.regs.dim, TsunamiState, 4),
        VMSTATE_UINT64_ARRAY(cchip.regs.mpr, TsunamiState, 4),
        VMSTATE_UINT64_ARRAY(cchip.regs.iic, TsunamiState, 4),
        VMSTATE_UINT64(cchip.regs.drir, TsunamiState),
        VMSTATE_UINT64(cchip.regs.ttr, TsunamiState),
        VMSTATE_UINT64(cchip.regs.tdr, TsunamiState),
        VMSTATE_UINT64(cchip.regs.pwr, TsunamiState),
        VMSTATE_UINT64(cchip.regs.prbe, TsunamiState),
        VMSTATE_UINT64_ARRAY(cchip.regs.cmonctl, TsunamiState, 2),
        VMSTATE_UINT64_ARRAY(cchip.regs.cmoncnt, TsunamiState, 2),
        VMSTATE_UINT64(dchip.regs.dsc, TsunamiState),
        VMSTATE_UINT64(dchip.regs.str, TsunamiState),
        VMSTATE_UINT64(dchip.regs.drev, TsunamiState),
        VMSTATE_UINT64(dchip.regs.dsc2, TsunamiState),
        VMSTATE_END_OF_LIST()
    },
};

static void tsunami_chipset_instance_init(Object *obj)
{
    if (object_resolve_path_type("", TYPE_TSUNAMI_CHIPSET, NULL) != NULL) {
        error_report("'%s' can't be instantiated twice!",
                     TYPE_TSUNAMI_CHIPSET);
        return;
    }
}

static void tsunami_chipset_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    InterruptStatsProviderClass *ispc = INTERRUPT_STATS_PROVIDER_CLASS(oc);

    dc->realize = tsunami_chipset_realize;
    /* Reason: part of the Tsunami chipset, does not exist standalone */
    dc->user_creatable = false;
    dc->vmsd = &vmstate_tsunami_chipset;
    ispc->get_statistics = tsunami_chipset_get_statistics;
    ispc->print_info = tsunami_chipset_print_info;
    device_class_set_legacy_reset(dc, tsunami_chipset_reset);
    device_class_set_props(dc, tsunami_chipset_properties);
}

static const TypeInfo tsunami_chipset_info = {
    .name          = TYPE_TSUNAMI_CHIPSET,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TsunamiState),
    .instance_init = tsunami_chipset_instance_init,
    .class_init    = tsunami_chipset_class_init,
    .interfaces = (InterfaceInfo[]) {
         { TYPE_INTERRUPT_STATS_PROVIDER },
         { }
    },
};

/*
 * PCI IOMMU Implementation
 */

static int tsunami_get_pte(AddressSpace *as, dma_addr_t pte_addr,
                           uint64_t *pte)
{
    int ret;

    /* TODO: guarantee 64-bit single-copy atomicity */
    ret = ldq_le_dma(as, pte_addr, pte, MEMTXATTRS_UNSPECIFIED);
    if (ret != MEMTX_OK) {
        return -EINVAL;
    }
    trace_tsunami_iommu_get_pte(pte_addr, *pte);
    return 0;
}

static int tsunami_get_pte_sg(TsunamiDMAWindow *win, AddressSpace *as,
                              dma_addr_t iova, uint64_t *pte)
{
    dma_addr_t pte_addr;

    pte_addr  = win->tba & ~(win->wsm >> 10);
    pte_addr |= (iova & (win->wsm | TSUNAMI_SG_MASK)) >> 10;
    return tsunami_get_pte(as, pte_addr, pte);
}

static int tsunami_get_pte_sg_dac(TsunamiDMAWindow *win, AddressSpace *as,
                                  dma_addr_t iova, uint64_t *pte)
{
    dma_addr_t pte_addr;

    pte_addr  = win->tba & R_TBA_ADDR_DAC_MASK;
    pte_addr |= (iova & TSUNAMI_SG_DAC_MASK) >> 10;
    return tsunami_get_pte(as, pte_addr, pte);
}

static bool tsunami_iommu_window_translate(TsunamiDMAWindow *win,
                                           dma_addr_t iova,
                                           TsunamiIOTLBEntry *tlbe)
{
    uint32_t wsm_ext = win->wsm | ~R_WSBA_ADDR_MASK;

    /* Check for window disable. */
    if ((win->wsba & R_WSBA_ENA_MASK) == 0) {
        goto error;
    }

    /* Check for window hit. */
    if ((iova & ~wsm_ext) != (win->wsba & R_WSBA_ADDR_MASK)) {
        goto error;
    }

    /* Perform the translation. */
    if (win->wsba & R_WSBA_SG_MASK) {
        uint64_t mask = TSUNAMI_PAGE_MASK_8K;
        uint64_t pte;

        /* Scatter-gather translation. */
        if (tsunami_get_pte_sg(win, &address_space_memory, iova, &pte)) {
            goto error;
        }
        if (!tsunami_pte_valid(pte)) {
            trace_tsunami_iommu_invalid_pte(pte);
            goto error;
        }

        /*
         * Table 10–6 Generating PTE Address from PCI DMA Address Via
         * Scatter-Gather Mapping
         *
         * Window Size    SG PTE AREA    WSMn<31:20>     PTE Address <34:3>
         * -------------  -------------  --------------  --------------------
         * 1MB            1KB            0000.0000.0000  TBA<34:10>:ad<19:13>
         * 2MB            2KB            0000.0000.0001  TBA<34:11>:ad<20:13>
         * 4MB            4KB            0000.0000.0011  TBA<34:12>:ad<21:13>
         * 8MB            8KB            0000.0000.0111  TBA<34:13>:ad<22:13>
         * 16MB           16KB           0000.0000.1111  TBA<34:14>:ad<23:13>
         * 32MB           32KB           0000.0001.1111  TBA<34:15>:ad<24:13>
         * 64MB           64KB           0000.0011.1111  TBA<34:16>:ad<25:13>
         * 128MB          128KB          0000.0111.1111  TBA<34:17>:ad<26:13>
         * 256MB          256KB          0000.1111.1111  TBA<34:18>:ad<27:13>
         * 512MB          512KB          0001.1111.1111  TBA<34:19>:ad<28:13>
         * 1GB            1MB            0011.1111.1111  TBA<34:20>:ad<29:13>
         * 2GB            2MB            0111.1111.1111  TBA<34:21>:ad<30:13>
         * 4GB            4MB            N/A             TBA<34:22>:ad<31:13>
         *                                               (Window 3 in DAC
         *                                                mode only)
         */
        tlbe->entry.iova = iova & ~mask;
        tlbe->entry.translated_addr = tsunami_pte_address(pte);
        tlbe->entry.addr_mask = mask;
        tlbe->entry.perm = IOMMU_RW;
    } else {
        /* Direct-mapped translation. */
        tlbe->entry.iova = iova & ~wsm_ext;
        tlbe->entry.translated_addr = win->tba & ~wsm_ext;
        tlbe->entry.addr_mask = wsm_ext;
        tlbe->entry.perm = IOMMU_RW;
    }
    return true;

error:
    return false;
}

static bool tsunami_iommu_window_dac_translate(TsunamiDMAWindow *win,
                                               dma_addr_t iova,
                                               TsunamiIOTLBEntry *tlbe)
{
    uint64_t mask = TSUNAMI_PAGE_MASK_8K;
    uint64_t pte;

    /* Check the fourth window for DAC enable and window enable. */
    if (!compare_masked(win->wsba, R_WSBA3_DAC_MASK | R_WSBA_ENA_MASK)) {
        goto error;
    }

    /* Fetch the PTE. */
    if (tsunami_get_pte_sg_dac(win, &address_space_memory, iova, &pte)) {
        goto error;
    }
    if (!tsunami_pte_valid(pte)) {
        trace_tsunami_iommu_invalid_pte(pte);
        goto error;
    }

    tlbe->entry.iova = iova & ~mask;
    tlbe->entry.translated_addr = tsunami_pte_address(pte);
    tlbe->entry.addr_mask = mask;
    tlbe->entry.perm = IOMMU_RW;
    return true;

error:
    return false;
}

static TsunamiIOMMUStatus tsunami_iommu_walk(TsunamiPchipState *s,
                                             dma_addr_t iova,
                                             IOMMUAccessFlags perm,
                                             TsunamiIOTLBEntry *tlbe)
{
    int i;

    if (iova <= UINT32_C(0xffffffff)) {
        /* Check for the Window Hole, inhibiting matching. */
        if ((s->regs.pctl & R_PCTL_HOLE_MASK) &&
            (iova >= 0x80000 && iova <= 0xfffff)) {
            goto error;
        }

        /* Check the first three windows. */
        for (i = 0; i < 3; ++i) {
            if (tsunami_iommu_window_translate(&s->regs.win[i], iova, tlbe)) {
                return IOMMU_STATUS_SUCCESS;
            }
        }

        /* Check the fourth window for DAC disable. */
        if ((s->regs.win[3].wsba & R_WSBA3_DAC_MASK) == 0 &&
            tsunami_iommu_window_translate(&s->regs.win[3], iova, tlbe)) {
            return IOMMU_STATUS_SUCCESS;
        }
    } else {
        /* Double-address cycle. */
        if (iova >= UINT64_C(0x10000000000) &&
            iova <= UINT64_C(0x1ffffffffff)) {
            /*
             * 10.1.4.4 Monster Window DMA Address Translation
             *
             * In case of a PCI dual-address cycle command, the high-order
             * PCI address bits <63:40> are compared to the constant value
             * 0x0000_01 (that is, bit <40> = 1; all other bits = 0).
             *
             * If these bits match, a monster window hit has occurred and the
             * low-order PCI address bits <34:0> are used unchanged as the
             * system address bits <34:0>. PCI address bits <39:35> are
             * ignored. The high-order 32 PCI address bits are available
             * on b_ad<31:0> in the second cycle of a DAC, and also on
             * b_ad<63:32> in the first cycle of a DAC if b_req64_l is
             * asserted.
             */
            if (s->regs.pctl & R_PCTL_MWIN_MASK) {
                tlbe->entry.iova = 0;
                tlbe->entry.translated_addr = 0;
                tlbe->entry.addr_mask = TSUNAMI_MWIN_MASK;
                tlbe->entry.perm = IOMMU_RW;
                return IOMMU_STATUS_SUCCESS;
            }
        }

        if (iova >= UINT64_C(0x80000000000) &&
            iova <= UINT64_C(0xfffffffffff)) {
            if (tsunami_iommu_window_dac_translate(&s->regs.win[3], iova,
                                                   tlbe)) {
                return IOMMU_STATUS_SUCCESS;
            }
        }
    }

error:
    tlbe->entry.perm = IOMMU_NONE;
    return IOMMU_STATUS_ABORT;
}

/* Entry point to the IOMMU, does everything. */
static IOMMUTLBEntry tsunami_translate_iommu(IOMMUMemoryRegion *mr,
                                             hwaddr addr,
                                             IOMMUAccessFlags flag,
                                             int iommu_idx)
{
    TsunamiPchipState *s = container_of(mr, TsunamiPchipState, iommu);
    TsunamiIOTLBEntry tlbe = {};
    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = addr,
        .addr_mask = ~(hwaddr)0,
        .perm = IOMMU_NONE,
    };

    switch (tsunami_iommu_walk(s, addr, flag, &tlbe)) {
    case IOMMU_STATUS_ABORT:
        trace_tsunami_iommu_translate_abort(mr->parent_obj.name, addr);
        break;
    case IOMMU_STATUS_SUCCESS:
        entry.perm = tlbe.entry.perm;
        entry.addr_mask = tlbe.entry.addr_mask;
        entry.translated_addr = tsunami_iotlb_translate(&tlbe, addr);
        trace_tsunami_iommu_translate_success(mr->parent_obj.name, entry.iova,
                                              entry.translated_addr);
        break;
    default:
        g_assert_not_reached();
    }
    return entry;
}

static AddressSpace *tsunami_pci_dma_iommu_as(PCIBus *bus, void *opaque,
                                              int devfn)
{
    TsunamiPchipState *pcs = opaque;

    return &pcs->iommu_as;
}

static void tsunami_iommu_memory_region_class_init(ObjectClass *klass,
                                                   const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = tsunami_translate_iommu;
}

static const PCIIOMMUOps tsunami_pci_iommu_ops = {
    .get_address_space = tsunami_pci_dma_iommu_as,
};

static const TypeInfo tsunami_iommu_memory_region_info = {
    .parent     = TYPE_IOMMU_MEMORY_REGION,
    .name       = TYPE_TSUNAMI_IOMMU_MEMORY_REGION,
    .class_init = tsunami_iommu_memory_region_class_init,
};

/*
 * PCI Host Bridge Implementation
 */

static const Property tsunami_pcihost_properties[] = {
    DEFINE_PROP_UINT8("bus_nr", TsunamiPchipState, bus_nr, 0),
    DEFINE_PROP_LINK("upstream", TsunamiPchipState, upstream,
                     TYPE_TSUNAMI_CHIPSET, TsunamiState *),
};

static void pchip_reset(TsunamiPchipState *pcs)
{
    TsunamiDMAWindow *win;
    int i;

    /*
     * Initialization for WSBA0, WSBA1, WSBA2. (Table 10-35)
     * Initialization for WSBA3. (Table 10-36)
     */
    for (i = 0; i < ARRAY_SIZE(pcs->regs.win); i++) {
        win = &pcs->regs.win[i];
        switch (i) {
        case 0 ... 2:
            INIT_FIELD(win->wsba, WSBA, ADDR, 0);
            INIT_FIELD(win->wsba, WSBA, SG, WSBA_SG_DISABLE);
            INIT_FIELD(win->wsba, WSBA, ENA, WSBA_ENA_DISABLE);
            break;
        case 3:
            INIT_FIELD(win->wsba, WSBA3, ADDR, 0);
            INIT_FIELD(win->wsba, WSBA3, SG, WSBA_SG_ENABLE);
            INIT_FIELD(win->wsba, WSBA3, ENA, WSBA_ENA_DISABLE);
            INIT_FIELD(win->wsba, WSBA3, DAC, WSBA3_DAC_DISABLE);
            break;
        default:
            g_assert_not_reached();
        }
    }

    /*
     * Initialization for WSM0, WSM1, WSM2, WSM3. (Table 10-37)
     */
    for (i = 0; i < ARRAY_SIZE(pcs->regs.win); i++) {
        win = &pcs->regs.win[i];
        INIT_FIELD(win->wsm, WSM, AM, 0);
    }

    /*
     * Initialization for TBA0, TBA1, TBA2. (Table 10-38)
     * Initialization for TBA3. (Table 10-39)
     */
    for (i = 0; i < ARRAY_SIZE(pcs->regs.win); i++) {
        win = &pcs->regs.win[i];
        INIT_FIELD(win->tba, WSM, AM, 0);
    }

    /*
     * Initialization for PCTL. (Table 10-40)
     */
    INIT_FIELD(pcs->regs.pctl, PCTL, PID, 0);
    INIT_FIELD(pcs->regs.pctl, PCTL, RPP, PCTL_RPP_NOT_PRESENT);
    INIT_FIELD(pcs->regs.pctl, PCTL, PTEVRFY, PCTL_PTEVRFY_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, FDWDIS, PCTL_FDWDIS_NORMAL);
    INIT_FIELD(pcs->regs.pctl, PCTL, FDSDIS, PCTL_FDSDIS_NORMAL);
    INIT_FIELD(pcs->regs.pctl, PCTL, PCLKX, PCTL_PCLKX_6_TIMES);
    INIT_FIELD(pcs->regs.pctl, PCTL, PTPMAX, 2);
    INIT_FIELD(pcs->regs.pctl, PCTL, CRQMAX, 1);
    INIT_FIELD(pcs->regs.pctl, PCTL, REV, 1);
    INIT_FIELD(pcs->regs.pctl, PCTL, CDQMAX, 1);
    INIT_FIELD(pcs->regs.pctl, PCTL, PADM, 0);
    INIT_FIELD(pcs->regs.pctl, PCTL, ECCEN, PCTL_ECCEN_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, PPRI, PCTL_PPRI_LOW);
    INIT_FIELD(pcs->regs.pctl, PCTL, PRIGRP, PCTL_PRIGRP_PCI_LOW);
    INIT_FIELD(pcs->regs.pctl, PCTL, ARBENA, PCTL_ARBENA_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, MWIN, PCTL_MWIN_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, HOLE, PCTL_HOLE_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, TGTLAT, PCTL_TGTLAT_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, CHAINDIS, PCTL_CHAINDIS_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, THDIS, PCTL_THDIS_NORMAL);
    INIT_FIELD(pcs->regs.pctl, PCTL, FBTB, PCTL_FBTB_DISABLE);
    INIT_FIELD(pcs->regs.pctl, PCTL, FDSC, PCTL_FDSC_ENABLE);

    /*
     * Initialization for PLAT. (Table 10-41)
     */
    INIT_FIELD(pcs->regs.plat, PLAT, LAT, 0);

    /*
     * Initialization for PERROR. (Table 10-42)
     */
    INIT_FIELD(pcs->regs.perror, PERROR, SYN, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, CMD, PERROR_CMD_DMA_READ);
    INIT_FIELD(pcs->regs.perror, PERROR, INV, PERROR_INFO_VALID);
    INIT_FIELD(pcs->regs.perror, PERROR, ADDR, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, CRE, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, UECC, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, NDS, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, RDPE, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, TA, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, APE, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, SGE, PERROR_SGE_VALID);
    INIT_FIELD(pcs->regs.perror, PERROR, DCRTO, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, PERR, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, SERR, 0);
    INIT_FIELD(pcs->regs.perror, PERROR, LOST, PERROR_LOST_NOT_LOST);

    /*
     * Initialization for PERRMASK. (Table 10-43)
     */
    INIT_FIELD(pcs->regs.perrmask, PERRMASK, MASK, 0);

    /*
     * Initialization for PERRSET. (Table 10-44)
     */
    INIT_FIELD(pcs->regs.perrset, PERRSET, INFO, 0);
    INIT_FIELD(pcs->regs.perrset, PERRSET, SET, 0);

    /*
     * Initialization for PMONCTL. (Table 10-47)
     */
    INIT_FIELD(pcs->regs.pmonctl, PMONCTL, STKDIS1, PMONCTL_STKDIS_STICKS_1S);
    INIT_FIELD(pcs->regs.pmonctl, PMONCTL, STKDIS0, PMONCTL_STKDIS_STICKS_1S);
    INIT_FIELD(pcs->regs.pmonctl, PMONCTL, SLCT1, 0);
    INIT_FIELD(pcs->regs.pmonctl, PMONCTL, SLCT0, 1);

    /*
     * Initialization for PMONCNT. (Table 10-48)
     */
    INIT_FIELD(pcs->regs.pmonctl, PMONCNT, CNT1, 0);
    INIT_FIELD(pcs->regs.pmonctl, PMONCNT, CNT0, 0);
}


static uint64_t pchip_read(void *opaque, hwaddr addr, unsigned size)
{
    TsunamiPchipState *pcs = opaque;
    uint64_t ret;

    switch (addr) {
    case A_PCHIP_WSBA0:
        /* WSBA0: Window Space Base Address Register. */
        ret = pcs->regs.win[0].wsba;
        break;
    case A_PCHIP_WSBA1:
        ret = pcs->regs.win[1].wsba;
        break;
    case A_PCHIP_WSBA2:
        ret = pcs->regs.win[2].wsba;
        break;
    case A_PCHIP_WSBA3:
        ret = pcs->regs.win[3].wsba;
        break;

    case A_PCHIP_WSM0:
        /* WSM0: Window Space Mask Register. */
        ret = pcs->regs.win[0].wsm;
        break;
    case A_PCHIP_WSM1:
        ret = pcs->regs.win[1].wsm;
        break;
    case A_PCHIP_WSM2:
        ret = pcs->regs.win[2].wsm;
        break;
    case A_PCHIP_WSM3:
        ret = pcs->regs.win[3].wsm;
        break;

    case A_PCHIP_TBA0:
        /* TBA0: Translated Base Address Register. */
        ret = pcs->regs.win[0].tba;
        break;
    case A_PCHIP_TBA1:
        ret = pcs->regs.win[1].tba;
        break;
    case A_PCHIP_TBA2:
        ret = pcs->regs.win[2].tba;
        break;
    case A_PCHIP_TBA3:
        ret = pcs->regs.win[3].tba;
        break;

    case A_PCHIP_PCTL:
        /* PCTL: Pchip Control Register. */
        ret = pcs->regs.pctl;
        break;

    case A_PCHIP_PLAT:
        /* PLAT: Pchip Master Latency Register. */
        ret = pcs->regs.plat;
        break;

    case A_PCHIP_PERROR:
        /* PERROR: Pchip Error Register. */
        ret = pcs->regs.perror;
        break;

    case A_PCHIP_PERRMASK:
        /* PERRMASK: Pchip Error Mask Register. */
        ret = pcs->regs.perrmask;
        break;

    case A_PCHIP_PMONCTL:
        /* PMONCTL: Pchip Monitor Control Register. */
        ret = pcs->regs.pctl;
        break;

    case A_PCHIP_PMONCNT:
        /* PMONCNT: Pchip Monitor Counter Register. */
        ret = 0;
        break;

    case A_PCHIP_SPRST:
        /* SPRST: Soft PCI Reset Register. */
        ret = 0;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid offset 0x%04" HWADDR_PRIx "\n",
                      __func__, addr);
        ret = 0;
        break;
    }

    trace_tsunami_pchip_read(pcs->bus_nr, "csr", addr, ret, size);
    return ret;
}

static void pchip_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    TsunamiPchipState *pcs = opaque;

    switch (addr) {
    case A_PCHIP_WSBA0:
        /* WSBA0: Window Space Base Address Register. */
        MERGE_FIELD(pcs->regs.win[0].wsba, val, 0x00000000fff00003);
        break;
    case A_PCHIP_WSBA1:
        MERGE_FIELD(pcs->regs.win[1].wsba, val, 0x00000000fff00003);
        break;
    case A_PCHIP_WSBA2:
        MERGE_FIELD(pcs->regs.win[2].wsba, val, 0x00000000fff00003);
        break;
    case A_PCHIP_WSBA3:
        MERGE_FIELD(pcs->regs.win[3].wsba, val, 0x00000080fff00001);
        pcs->regs.win[3].wsba |= R_WSBA3_SG_MASK;
        break;

    case A_PCHIP_WSM0:
        /* WSM0: Window Space Mask Register. */
        MERGE_FIELD(pcs->regs.win[0].wsm, val, 0x00000000fff00000);
        break;
    case A_PCHIP_WSM1:
        MERGE_FIELD(pcs->regs.win[1].wsm, val, 0x00000000fff00000);
        break;
    case A_PCHIP_WSM2:
        MERGE_FIELD(pcs->regs.win[2].wsm, val, 0x00000000fff00000);
        break;
    case A_PCHIP_WSM3:
        MERGE_FIELD(pcs->regs.win[3].wsm, val, 0x00000000fff00000);
        break;

    case A_PCHIP_TBA0:
        /* TBA0: Translated Base Address Register. */
        MERGE_FIELD(pcs->regs.win[0].tba, val, 0x00000007fffffc00);
        break;
    case A_PCHIP_TBA1:
        MERGE_FIELD(pcs->regs.win[1].tba, val, 0x00000007fffffc00);
        break;
    case A_PCHIP_TBA2:
        MERGE_FIELD(pcs->regs.win[2].tba, val, 0x00000007fffffc00);
        break;
    case A_PCHIP_TBA3:
        MERGE_FIELD(pcs->regs.win[3].tba, val, 0x00000007fffffc00);
        break;

    case A_PCHIP_PCTL:
        /* PCTL: Pchip Control Register. */
        MERGE_FIELD(pcs->regs.pctl, val, 0x00001cff00fcffff);
        break;

    case A_PCHIP_PLAT:
        /* PLAT: Pchip Master Latency Register. */
        MERGE_FIELD(pcs->regs.plat, val, 0x000000000000ff00);
        break;

    case A_PCHIP_PERROR:
        /* PERROR: Pchip Error Register. */
        MERGE_FIELD(pcs->regs.plat, val, 0x0000000000000dff);
        break;

    case A_PCHIP_PERRMASK:
        /* PERRMASK: Pchip Error Mask Register. */
        MERGE_FIELD(pcs->regs.perrmask, val, 0x0000000000000fff);
        break;

    case A_PCHIP_PERRSET:
        /* PERRSET: Pchip Error Set Register. */
        MERGE_FIELD(pcs->regs.perrset, val, 0xffffffffffff0fff);
        break;

    case A_PCHIP_TLBIV:
        /* TLBIV: Translation Buffer Invalidate Virtual Register. */
        break;

    case A_PCHIP_TLBIA:
        /* TLBIA: Translation Buffer Invalidate All Register (WO). */
        break;

    case A_PCHIP_PMONCTL:
        /* PMONCTL: Pchip Monitor Control Register. */
        MERGE_FIELD(pcs->regs.pmonctl, val, 0x000000000003ffff);
        break;

    case A_PCHIP_SPRST:
        /* SPRST: Soft PCI Reset Register. */
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid offset 0x%04" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    trace_tsunami_pchip_write(pcs->bus_nr, "csr", addr, val, size);
}

static const MemoryRegionOps pchip_ops = {
    .read = pchip_read,
    .write = pchip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

/* PCI/EISA Interrupt Acknowledge Cycle.  */
static uint64_t iack_read(void *opaque, hwaddr addr, unsigned size)
{
    TsunamiPchipState *pcs = opaque;
    uint64_t ret = pic_read_irq(isa_pic);

    trace_tsunami_pchip_read(pcs->bus_nr, "iack", addr, ret, size);
    return ret;
}

static void iack_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TsunamiPchipState *pcs = opaque;

    trace_tsunami_pchip_write(pcs->bus_nr, "iack", addr, val, size);
}

const MemoryRegionOps iack_ops = {
    .read = iack_read,
    .write = iack_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* PCI config space reads/writes, to byte-word addressable memory.  */
static uint64_t pci_conf_read(void *opaque, hwaddr addr,
                              unsigned size)
{
    TsunamiPchipState *pcs = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(pcs);
    uint64_t ret = pci_data_read(phb->bus, addr, size);

    trace_tsunami_pchip_read(pcs->bus_nr, "pci-config", addr, ret, size);
    return ret;
}

static void pci_conf_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    TsunamiPchipState *pcs = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(pcs);

    trace_tsunami_pchip_write(pcs->bus_nr, "pci-config", addr, val, size);
    pci_data_write(phb->bus, addr, val, size);
}

const MemoryRegionOps pci_conf_ops = {
    .read = pci_conf_read,
    .write = pci_conf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static inline char *tsunami_bus_name(TsunamiPchipState *pcs)
{
    return g_strdup_printf("pci.%d", pcs->bus_nr);
}

static inline char *tsunami_region_name(TsunamiPchipState *pcs,
                                        const char *name)
{
    return g_strdup_printf("pchip%d.%s", pcs->bus_nr, name);
}

static int tsunami_pcihost_map_irq(PCIDevice *d, int intx)
{
    PCIBus *bus = pci_device_root_bus(d);
    int slot = PCI_SLOT(d->devfn) + 1;
    int offset = tsunami_pci_root_bus_index(bus) * 0x10;

    assert(0 <= intx && intx < PCI_NUM_PINS);
    return ((slot * PCI_NUM_PINS) + offset + intx) % bus->nirq;
}

static void tsunami_pcihost_set_irq(void *opaque, int n, int level)
{
    TsunamiPchipState *pcs = opaque;
    TsunamiState *s = pcs->upstream;

    trace_tsunami_pchip_set_irq(pcs->bus_nr, n, level);
    cchip_device_irq_update(s, n, level);
}

static void tsunami_pcihost_reset(DeviceState *dev)
{
    TsunamiPchipState *pcs = TSUNAMI_PCI_HOST_BRIDGE(dev);

    memset(&pcs->regs, 0, sizeof(pcs->regs));
    pchip_reset(pcs);
}

static int tsunami_pcihost_hose_number(PCIHostState *host_bridge)
{
    TsunamiPchipState *pcs = TSUNAMI_PCI_HOST_BRIDGE(host_bridge);

    return pcs->bus_nr;
}

static const char *tsunami_pcihost_root_bus_path(PCIHostState *host_bridge,
                                                 PCIBus *rootbus)
{
    TsunamiPchipState *pcs = TSUNAMI_PCI_HOST_BRIDGE(host_bridge);

    snprintf(pcs->bus_path, sizeof(pcs->bus_path), "0000:%02x", pcs->bus_nr);
    return pcs->bus_path;
}

static void tsunami_pcihost_realize(DeviceState *dev, Error **errp)
{
    TsunamiPchipState *s = TSUNAMI_PCI_HOST_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    const struct pchip_base *memmap;
    g_autofree char *bus_name = NULL;
    g_autofree char *iommu_name = NULL;
    g_autofree char *csr_name = NULL;
    g_autofree char *iack_name = NULL;
    g_autofree char *config_name = NULL;
    g_autofree char *mem_name = NULL;
    g_autofree char *io_name = NULL;

    if (unlikely(!s->upstream)) {
        error_setg(errp, "%s: 'upstream' property is not set", __func__);
        return;
    }

    bus_name = tsunami_bus_name(s);
    iommu_name = tsunami_region_name(s, "iommu");
    csr_name = tsunami_region_name(s, "csr");
    iack_name = tsunami_region_name(s, "iack");
    config_name = tsunami_region_name(s, "pci-config");
    mem_name = tsunami_region_name(s, "pci-mem");
    io_name = tsunami_region_name(s, "pci-io");

    assert(s->bus_nr < ARRAY_SIZE(pchip_memmap));
    memmap = &pchip_memmap[s->bus_nr];

    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_TSUNAMI_IOMMU_MEMORY_REGION, OBJECT(s),
                             iommu_name, 16 * TiB);
    address_space_init(&s->iommu_as, MEMORY_REGION(&s->iommu), bus_name);

    memory_region_init_io(&s->reg_csr, OBJECT(s), &pchip_ops, s,
                          csr_name, memmap->csr.size);
    memory_region_init_io(&s->reg_iack, OBJECT(s), &iack_ops, s,
                          iack_name, memmap->pci_iack.size);
    memory_region_init_io(&s->reg_conf, OBJECT(s), &pci_conf_ops, s,
                          config_name, memmap->pci_config.size);
    memory_region_init(&s->reg_mem, OBJECT(s), mem_name, memmap->pci_mem.size);
    memory_region_init(&s->reg_io, OBJECT(s), io_name, memmap->pci_io.size);

    phb->bus = pci_register_root_bus(dev, bus_name,
                                     tsunami_pcihost_set_irq,
                                     tsunami_pcihost_map_irq, dev,
                                     &s->reg_mem, &s->reg_io,
                                     PCI_DEVFN(s->upstream->first_slot, 0),
                                     TSUNAMI_PCI_NIRQS, TYPE_PCI_BUS);
    pci_setup_iommu(phb->bus, &tsunami_pci_iommu_ops, s);
    pci_bus_set_slot_reserved_mask(phb->bus, 0xfff00000);

    memory_region_init_io(&phb->conf_mem, OBJECT(phb), &pci_host_conf_le_ops,
                          phb, "pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, OBJECT(phb), &pci_host_data_le_ops,
                          phb, "pci-conf-data", 4);
    memory_region_add_subregion(&s->reg_io, 0xcf8, &phb->conf_mem);
    memory_region_add_subregion(&s->reg_io, 0xcfc, &phb->data_mem);
    memory_region_set_flush_coalesced(&phb->data_mem);
    memory_region_add_coalescing(&phb->conf_mem, 0, 4);
}

static const VMStateDescription vmstate_tsunami_pcihost_window = {
    .name = TYPE_TSUNAMI_PCI_HOST_BRIDGE_PREFIX "window",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(wsba, TsunamiDMAWindow),
        VMSTATE_UINT64(wsm, TsunamiDMAWindow),
        VMSTATE_UINT64(tba, TsunamiDMAWindow),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_tsunami_pcihost = {
    .name = TYPE_TSUNAMI_PCI_HOST_BRIDGE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(regs.win, TsunamiPchipState, 4, 1,
                             vmstate_tsunami_pcihost_window, TsunamiDMAWindow),
        VMSTATE_UINT64(regs.pctl, TsunamiPchipState),
        VMSTATE_UINT64(regs.plat, TsunamiPchipState),
        VMSTATE_UINT64(regs.perror, TsunamiPchipState),
        VMSTATE_UINT64(regs.perrmask, TsunamiPchipState),
        VMSTATE_UINT64(regs.perrset, TsunamiPchipState),
        VMSTATE_UINT64(regs.pmonctl, TsunamiPchipState),
        VMSTATE_UINT64(regs.pmoncnt, TsunamiPchipState),
        VMSTATE_END_OF_LIST()
    }
};

static void tsunami_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    hc->hose_number = tsunami_pcihost_hose_number;
    hc->root_bus_path = tsunami_pcihost_root_bus_path;
    dc->realize = tsunami_pcihost_realize;
    /* Reason: Must be wired up in code (sysbus MRs and IRQ) */
    dc->user_creatable = false;
    dc->vmsd = &vmstate_tsunami_pcihost;
    dc->fw_name = "pci";
    device_class_set_legacy_reset(dc, tsunami_pcihost_reset);
    device_class_set_props(dc, tsunami_pcihost_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo tsunami_pcihost_info = {
    .name          = TYPE_TSUNAMI_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(TsunamiPchipState),
    .class_init    = tsunami_pcihost_class_init,
};

static void tsunami_register_types(void)
{
    type_register_static(&tsunami_chipset_info);
    type_register_static(&tsunami_pcihost_info);
    type_register_static(&tsunami_iommu_memory_region_info);
}

type_init(tsunami_register_types)
