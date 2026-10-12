/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha TLB helpers
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "internals.h"
#include "syndrome.h"
#include "exec/cputlb.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/target_page.h"
#include "exec/log.h"

QEMU_BUILD_BUG_ON(PAGE_READ != BIT(MMU_DATA_LOAD));
QEMU_BUILD_BUG_ON(PAGE_WRITE != BIT(MMU_DATA_STORE));
QEMU_BUILD_BUG_ON(PAGE_EXEC != BIT(MMU_INST_FETCH));

typedef struct GetPhysAddrFlags {
    bool vpte_fetch;
    bool write_check;
} GetPhysAddrFlags;

static inline const char *access_str(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_DATA_LOAD:
        return "read";
    case MMU_DATA_STORE:
        return "write";
    case MMU_INST_FETCH:
        return "execute";
    default:
        g_assert_not_reached();
    }
}

static inline const char *fault_str(AlphaFaultType fault_type)
{
    switch (fault_type) {
    case AlphaFault_None:
        return "none";
    case AlphaFault_Address:
        return "address";
    case AlphaFault_Permission:
        return "permission";
    case AlphaFault_Translation:
        return "translation";
    case AlphaFault_Double:
        return "double";
    case AlphaFault_Alignment:
        return "alignment";
    case AlphaFault_Access:
        return "access";
    default:
        g_assert_not_reached();
    }
}

static inline const char *tlb_str(AlphaTLBType tlb_type)
{
    switch (tlb_type) {
    case AlphaTLBType_Data:
        return "data";
    case AlphaTLBType_Instruction:
        return "instruction";
    default:
        g_assert_not_reached();
    }
}

static inline bool regime_is_pal(AlphaMMUIdx mmu_idx)
{
    return mmu_idx == AlphaMMUIdx_PAL;
}

static inline bool regime_is_privileged(AlphaMMUIdx mmu_idx)
{
    return alpha_is_privileged(mmu_idx);
}

/* Return the number of address bits for a given addressing type. */
static inline int regime_address_bits(bool va48)
{
    return va48 ? 48 : 43;
}

static inline bool regime_address_check(vaddr address, bool va48)
{
    uint64_t ext;

    ext = sextract64(address, 0, regime_address_bits(va48));
    return (ext == address);
}

static inline AlphaTLBType map_tlb_type(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_DATA_LOAD:
    case MMU_DATA_STORE:
        return AlphaTLBType_Data;
    case MMU_INST_FETCH:
        return AlphaTLBType_Instruction;
    default:
        g_assert_not_reached();
    }
}

static inline bool access_is_effective_read(MMUAccessType access_type)
{
    return access_type == MMU_DATA_LOAD ||
           access_type == MMU_INST_FETCH;
}

static inline bool access_is_effective_write(MMUAccessType access_type)
{
    return access_type == MMU_DATA_STORE;
}

static inline bool fault_on_access(MMUAccessType access_type,
                                   GetPhysAddrFlags gpaf,
                                   AlphaMMUFaultInfo *fi)
{
    if (fi->fault_write) {
        return access_is_effective_write(access_type) ||
               gpaf.write_check;
    }
    if (fi->fault_read) {
        return access_is_effective_read(access_type);
    }
    return false;
}

/*
 * Granularity hint helper functions.
 */
static inline int pte_gh_raw(uint64_t pte)
{
    return FIELD_EX64(pte, PTE, GH);
}

static inline int pte_gh_shift(int gh)
{
    return TARGET_PAGE_BITS + (3 * (gh & 0b11));
}

static inline uint64_t pte_gh_keep(int gh)
{
   return MAKE_64BIT_MASK(0, pte_gh_shift(gh));
}

static inline uint64_t pte_gh_match(int gh, int implver)
{
    return alpha_vamask(implver) & ~pte_gh_keep(gh);
}

static inline uint64_t pte_gh_phys(int gh, int implver)
{
    return alpha_pamask(implver) & ~pte_gh_keep(gh);
}

/**
 * Return the corresponding TLB context for a given type.
 */
static inline CPUAlphaTLBContext *get_tlb_context(CPUAlphaState *env,
                                                  AlphaTLBType type)
{
    switch (type) {
    case AlphaTLBType_Data:
    case AlphaTLBType_Instruction:
        return &env->tlb[type];
    default:
        g_assert_not_reached();
    }
}

/**
 * Return the next TLB entry to use.
 */
static inline uint32_t get_next_tlbe(CPUAlphaTLBContext *ctx)
{
    uint32_t val = ctx->last_way;

    ctx->last_way = (ctx->last_way + 1) % MAX_TLB_ENTRIES;
    return val;
}

/**
 * Subtract permission flags based on fault info.
 */
static inline int prot_subtract_fox(AlphaMMUFaultInfo *fi, int prot)
{
    int res = prot;

    if (fi->fault_write) {
        res &= ~(PAGE_WRITE);
    }
    if (fi->fault_read) {
        res &= ~(PAGE_READ | PAGE_EXEC);
    }
    return res;
}

/**
 * Translate page access permissions to page R/W protection flags.
 */
static int pte_flags_to_prot(CPUAlphaState *env, MMUAccessType access_type,
                             AlphaMMUIdx mmu_idx, AlphaMMUFaultInfo *fi,
                             int pte_flags)
{
    int prot = 0;

    switch (mmu_idx) {
    case 0b00: /* Kernel */
        if (FIELD_EX64(pte_flags, PTE, KRE)) {
            prot |= PAGE_READ | PAGE_EXEC;
        }
        if (FIELD_EX64(pte_flags, PTE, KWE)) {
            prot |= PAGE_WRITE;
        }
        break;
    case 0b01: /* Executive */
        if (FIELD_EX64(pte_flags, PTE, ERE)) {
            prot |= PAGE_READ | PAGE_EXEC;
        }
        if (FIELD_EX64(pte_flags, PTE, EWE)) {
            prot |= PAGE_WRITE;
        }
        break;
    case 0b10: /* Supervisor */
        if (FIELD_EX64(pte_flags, PTE, SRE)) {
            prot |= PAGE_READ | PAGE_EXEC;
        }
        if (FIELD_EX64(pte_flags, PTE, SWE)) {
            prot |= PAGE_WRITE;
        }
        break;
    case 0b11: /* User */
        if (FIELD_EX64(pte_flags, PTE, URE)) {
            prot |= PAGE_READ | PAGE_EXEC;
        }
        if (FIELD_EX64(pte_flags, PTE, UWE)) {
            prot |= PAGE_WRITE;
        }
        break;
    default:
        g_assert_not_reached();
    }

    if (FIELD_EX64(pte_flags, PTE, FOW)) {
        fi->fault_write = true;
    }
    if (FIELD_EX64(pte_flags, PTE, FOR)) {
        fi->fault_read = true;
    }
    return prot;
}

/**
 * Find a TLB entry for a given virtual address and ASN pair.
 */
static CPUAlphaTLBEntry *find_tlb_entry_asn(CPUAlphaState *env,
                                            AlphaTLBType type, vaddr addr,
                                            uint8_t asn)
{
    CPUAlphaTLBContext *ctx;
    CPUAlphaTLBEntry *entry;
    int i;

    ctx = get_tlb_context(env, type);
    for (i = 0; i < ARRAY_SIZE(ctx->entries); i++) {
        entry = &ctx->entries[i];
        if ((entry->flags & R_PTE_VALID_MASK) &&
            ((entry->flags & R_PTE_ASM_MASK) || (entry->asn == asn)) &&
            (((entry->vaddr ^ addr) & entry->match_mask) == 0)) {
            return entry;
        }
    }
    return NULL;
}

/**
 * Find a TLB entry for a given virtual address.
 */
static CPUAlphaTLBEntry *find_tlb_entry(CPUAlphaState *env, AlphaTLBType type,
                                        vaddr addr)
{
    CPUAlphaTLBEntry *entry;
    uint8_t asn, i;

    switch (type) {
    case AlphaTLBType_Data:
        for (i = 0; i < 1; i++) {
            asn = alpha_cur_asn(env, type, i);
            entry = find_tlb_entry_asn(env, type, addr, asn);
            if (entry != NULL) {
                return entry;
            }
        }
        return NULL;

    case AlphaTLBType_Instruction:
        asn = alpha_cur_asn(env, type, 0);
        entry = find_tlb_entry_asn(env, type, addr, asn);
        if (entry != NULL) {
            return entry;
        }
        return NULL;

    default:
        g_assert_not_reached();
    }
}

static bool get_phys_addr(CPUAlphaState *env, vaddr address,
                          MMUAccessType access_type, AlphaMMUIdx mmu_idx,
                          GetPhysAddrFlags gpaf, GetPhysAddrResult *result,
                          AlphaMMUFaultInfo *fi)
{
    CPUAlphaTLBEntry *entry;
    AlphaTLBType type;
    bool va48;
    int spe, prot;

    type = map_tlb_type(access_type);
    fi->fault_read = false;
    fi->fault_write = false;
    fi->ifetch = access_type == MMU_INST_FETCH;

    /*
     * 6.2 PALmode Environment
     *
     * Istream memory mapping is disabled. Because the PALcode is used to
     * implement translation buffer fill routines, Istream mapping clearly
     * cannot be enabled. Dstream mapping is still enabled.
     */
    if ((regime_is_pal(mmu_idx) && fi->ifetch) ||
        (mmu_idx == AlphaMMUIdx_Physical)) {
        result->f.phys_addr = address & alpha_pamask(env->implver);
        result->f.attrs = MEMTXATTRS_UNSPECIFIED;
        result->f.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        result->f.lg_page_size = TARGET_PAGE_BITS;
        fi->type = AlphaFault_None;
        goto exit_success;
    }

    /*
     * Check for an improperly sign-extended address.
     */
    va48 = alpha_va48_enabled(env, fi->ifetch);
    if (unlikely(!regime_address_check(address, va48))) {
        fi->type = AlphaFault_Address;
        goto exit_fault;
    }

    /*
     * Handle the superpage accesses here.
     */
    spe = alpha_superpage_status(env, fi->ifetch);
    switch (env->implver) {
    case IMPLVER_2106x:
    case IMPLVER_21164:
        /*
         * 2.1.1.4 Instruction Translation Buffer
         *
         * One superpage maps virtual address bits <39:13> to physical
         * address bits <39:13>, on a one-to-one basis, when virtual
         * address bits <42:41> equal 2. This maps the entire physical
         * address space four times over to the quadrant of the virtual
         * address space.
         */
        if ((ALPHA_BITMASK_SHIFT(address, 42, 41) == 0x02) &&
            (spe & 0b0010)) {
            int bits = (env->implver == IMPLVER_2106x) ? 33 : 39;
            if (!regime_is_privileged(mmu_idx)) {
                fi->type = AlphaFault_Permission;
                goto exit_fault;
            }
            result->f.phys_addr = ALPHA_BITMASK_SHIFT(address, bits, 0) &
                                  alpha_pamask(env->implver);
            result->f.attrs = MEMTXATTRS_UNSPECIFIED;
            result->f.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            result->f.lg_page_size = TARGET_PAGE_BITS;
            fi->type = AlphaFault_None;
            goto exit_success;
        }

        /*
         * The other superpage maps virtual address bits <29:13> to physical
         * address bits <29:13>, on a one-to-one basis, and forces physica
         * address bits <39:30> to 0 when virtual address bits <42:30> equal
         * 1FFE. This effectively maps a 30-bit region of physical address
         * space to a single region of the virtual address space defined by
         * virtual address bits <42:30> = 1FFE.
         */
        if ((ALPHA_BITMASK_SHIFT(address, 42, 30) == 0x1ffe) &&
            (spe & 0b0001)) {
            if (!regime_is_privileged(mmu_idx)) {
                fi->type = AlphaFault_Permission;
                goto exit_fault;
            }
            result->f.phys_addr = ALPHA_BITMASK_SHIFT(address, 29, 0) &
                                  alpha_pamask(env->implver);
            result->f.attrs = MEMTXATTRS_UNSPECIFIED;
            result->f.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            result->f.lg_page_size = TARGET_PAGE_BITS;
            fi->type = AlphaFault_None;
            goto exit_success;
        }
        break;

    case IMPLVER_21264:
        /*
         * Table 5–18 Mbox Control Register Fields Description:
         *
         * SPE[2], when set, enables super page mapping when VA[47:46] = 2.
         * In this mode, VA[43:13] are mapped directly to PA[43:13] and
         * VA[45:44] are ignored."
         */
        spe = alpha_superpage_status(env, fi->ifetch);
        if ((ALPHA_BITMASK_SHIFT(address, 47, 46) == 0x02) &&
            (spe & 0b0100)) {
            if (!regime_is_privileged(mmu_idx)) {
                fi->type = AlphaFault_Permission;
                goto exit_fault;
            }
            result->f.phys_addr = ALPHA_BITMASK_SHIFT(address, 43, 0) &
                                  alpha_pamask(env->implver);
            result->f.attrs = MEMTXATTRS_UNSPECIFIED;
            result->f.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            result->f.lg_page_size = TARGET_PAGE_BITS;
            fi->type = AlphaFault_None;
            goto exit_success;
        }

        /*
         * SPE[1], when set, enables superpage mapping when VA[47:41] = 7E.
         * In this mode, VA[40:13] are mapped directly to PA[40:13] and
         * PA[43:41] are copies of PA[40] (sign extension).
         */
        if ((ALPHA_BITMASK_SHIFT(address, 47, 41) == 0x7e) &&
            (spe & 0b0010)) {
            if (!regime_is_privileged(mmu_idx)) {
                fi->type = AlphaFault_Permission;
                goto exit_fault;
            }
            result->f.phys_addr = ALPHA_BITMASK_SHIFT_SEXT(address, 40, 0) &
                                  alpha_pamask(env->implver);
            result->f.attrs = MEMTXATTRS_UNSPECIFIED;
            result->f.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            result->f.lg_page_size = TARGET_PAGE_BITS;
            fi->type = AlphaFault_None;
            goto exit_success;
        }

        /*
         * SPE[0], when set, enables superpage mapping when VA[47:30] = 3FFFE.
         * In this mode, VA[29:13] are mapped directly to PA[29:13] and
         * PA[43:30] are cleared.
         */
        if ((ALPHA_BITMASK_SHIFT(address, 47, 30) == 0x3fffe) &&
            (spe & 0b0001)) {
            if (!regime_is_privileged(mmu_idx)) {
                fi->type = AlphaFault_Permission;
                goto exit_fault;
            }
            result->f.phys_addr = ALPHA_BITMASK_SHIFT(address, 29, 0) &
                                  alpha_pamask(env->implver);
            result->f.attrs = MEMTXATTRS_UNSPECIFIED;
            result->f.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            result->f.lg_page_size = TARGET_PAGE_BITS;
            fi->type = AlphaFault_None;
            goto exit_success;
        }
        break;

    default:
        g_assert_not_reached();
    }

    /*
     * Check for a TLB hit.
     */
    if ((entry = find_tlb_entry(env, type, address)) == NULL) {
        if (env->implver == IMPLVER_2106x) {
            fi->type = alpha_is_pal(env) ? AlphaFault_Double
                                         : AlphaFault_Translation;
        } else {
            fi->type = gpaf.vpte_fetch ? AlphaFault_Double
                                       : AlphaFault_Translation;
        }
        goto exit_fault;
    }

    /* Handle access violations. */
    prot = pte_flags_to_prot(env, access_type, mmu_idx, fi, entry->flags);
    if (unlikely((prot & BIT(access_type)) == 0)) {
        fi->type = AlphaFault_Permission;
        goto exit_fault;
    }

    /* Handle write checks. */
    if (gpaf.write_check) {
        assert(access_type == MMU_DATA_LOAD);
        if (unlikely((prot & PAGE_WRITE) == 0)) {
            fi->type = AlphaFault_Permission;
            goto exit_fault;
        }
    }

    /* Handle fault on read/write. */
    if (unlikely(fault_on_access(access_type, gpaf, fi))) {
        fi->type = AlphaFault_Access;
        goto exit_fault;
    }

    /* Compute effective permissions, fault on no-access. */
    prot = prot_subtract_fox(fi, prot);
    if (unlikely(!prot)) {
        fi->type = AlphaFault_Access;
        goto exit_fault;
    }

    result->f.phys_addr = entry->paddr | (address & entry->keep_mask);
    result->f.attrs = MEMTXATTRS_UNSPECIFIED;
    result->f.prot = prot;
    result->f.lg_page_size = TARGET_PAGE_BITS;
    fi->type = AlphaFault_None;

exit_success:
    if (fi->ifetch) {
        result->f.prot &= ~PAGE_WRITE;
    } else {
        result->f.prot &= ~PAGE_EXEC;
    }
    return true;

exit_fault:
    return false;
}

/*
 * TLB utility functions.
 */
void alpha_tlb_flush_all(CPUAlphaState *env, AlphaTLBType type)
{
    CPUAlphaTLBContext *ctx;
    CPUAlphaTLBEntry *entry;
    int i;

    ctx = get_tlb_context(env, type);
    ctx->last_way = 0;
    for (i = 0; i < ARRAY_SIZE(ctx->entries); i++) {
        entry = &ctx->entries[i];
        entry->flags = 0;
    }

    /* Flush qemu's TLB and discard all shadowed entries. */
    qemu_log_mask(CPU_LOG_MMU, "%s: %s TLB invalidate all\n",
                  __func__, tlb_str(type));
    tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
}

void alpha_tlb_flush_process(CPUAlphaState *env, AlphaTLBType type)
{
    CPUAlphaTLBContext *ctx;
    CPUAlphaTLBEntry *entry;
    int i;

    ctx = get_tlb_context(env, type);
    for (i = 0; i < ARRAY_SIZE(ctx->entries); i++) {
        entry = &ctx->entries[i];
        if (!FIELD_EX64(entry->flags, PTE, ASM)) {
            entry->flags = 0;
        }
    }

    /* Flush qemu's TLB and discard all shadowed entries. */
    qemu_log_mask(CPU_LOG_MMU, "%s: %s TLB invalidate process\n",
                  __func__, tlb_str(type));
    tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
}

void alpha_tlb_flush_single(CPUAlphaState *env, AlphaTLBType type, vaddr addr,
                            int asn)
{
    CPUAlphaTLBEntry *entry = find_tlb_entry_asn(env, type, addr, asn);

    if (entry != NULL) {
        entry->flags = 0;
        qemu_log_mask(CPU_LOG_MMU, "%s: %s TLB invalidate single hit: "
                      "addr=0x" TARGET_FMT_lx " asn=0x%02x\n",
                      __func__, tlb_str(type), (target_ulong)addr, asn);
    }

    /*
     * QEMU may still cache a translation after its hardware TLB entry has
     * been replaced. Invalidate that shadow even if the hardware lookup
     * misses, or a subsequent remap can keep accessing the old page.
     */
    tlb_flush_by_mmuidx(env_cpu(env), all_tlbmask());
}

static inline uint64_t pte_pfn_ev4(CPUAlphaState *env, AlphaTLBType type,
                                   uint64_t pte)
{
    switch (type) {
    case AlphaTLBType_Data:
        return (FIELD_EX64(pte, EV4_DTB_PTE, PFN) << TARGET_PAGE_BITS);
    case AlphaTLBType_Instruction:
        return (FIELD_EX64(pte, EV4_ITB_PTE, PFN) << TARGET_PAGE_BITS);
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t pte_pfn_ev5(CPUAlphaState *env, AlphaTLBType type,
                                   uint64_t pte)
{
    switch (type) {
    case AlphaTLBType_Data:
        return (FIELD_EX64(pte, EV5_DTB_PTE, PFN) << TARGET_PAGE_BITS);
    case AlphaTLBType_Instruction:
        return (FIELD_EX64(pte, EV5_ITB_PTE, PFN) << TARGET_PAGE_BITS);
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t pte_pfn_ev6(CPUAlphaState *env, AlphaTLBType type,
                                   uint64_t pte)
{
    switch (type) {
    case AlphaTLBType_Data:
        return (FIELD_EX64(pte, EV6_DTB_PTE, PFN) << TARGET_PAGE_BITS);
    case AlphaTLBType_Instruction:
        return (FIELD_EX64(pte, EV6_ITB_PTE, PFN) << TARGET_PAGE_BITS);
    default:
        g_assert_not_reached();
    }
}

static inline uint64_t pte_pfn(CPUAlphaState *env, AlphaTLBType type,
                               uint64_t pte)
{
    switch (env->implver) {
    case IMPLVER_2106x:
        return pte_pfn_ev4(env, type, pte);
    case IMPLVER_21164:
        return pte_pfn_ev5(env, type, pte);
    case IMPLVER_21264:
        return pte_pfn_ev6(env, type, pte);
    default:
        g_assert_not_reached();
    }
}

void alpha_tlb_fill(CPUAlphaState *env, AlphaTLBType type, vaddr address,
                    uint64_t pte, int asn)
{
    CPUAlphaTLBContext *ctx;
    CPUAlphaTLBEntry *entry;
    uint64_t paddr, keep_mask, match_mask, phys_mask;
    int granule, end_bit;

    switch (type) {
    case AlphaTLBType_Data:
        end_bit = R_PTE_UWE_SHIFT + 1;
        break;
    case AlphaTLBType_Instruction:
        end_bit = R_PTE_URE_SHIFT + 1;
        break;
    default:
        g_assert_not_reached();
    }

    ctx = get_tlb_context(env, type);
    granule = pte_gh_raw(pte);
    keep_mask = pte_gh_keep(granule);
    match_mask = pte_gh_match(granule, env->implver);
    phys_mask = pte_gh_phys(granule, env->implver);
    paddr = pte_pfn(env, type, pte);

    /*
     * Get a TLB entry, if possible.
     */
    entry = find_tlb_entry_asn(env, type, address, asn);
    if (entry == NULL) {
        entry = &ctx->entries[get_next_tlbe(ctx)];
    }

    entry->vaddr = address & match_mask;
    entry->paddr = paddr & phys_mask;
    entry->match_mask = match_mask;
    entry->keep_mask = keep_mask;
    entry->flags = extract64(pte, 0, end_bit) | R_PTE_VALID_MASK;
    entry->asn = asn;
}

static int compute_exception_code(CPUAlphaState *env,
                                  AlphaMMUFaultInfo *fi)
{
    /*
     * D.7 Restriction 10: Duplicate IPR Mode Bits
     *
     * The virtual address size is selectable by programming IPR bits
     * I_CTL[VA_48] and VA_CTL[VA_48]. These bit values should usually be
     * equal when operating in native (virtual) mode. The I_CTL[VA_48] bit
     * determines the DTB double3/double4 PALcode entry, the JSR mispredict
     * comparison width, the VPC address generation width, the Istream ACV
     * limits, and the IVA_FORM format selection. The VA_CTL[VA_48] bit
     * determines the VA_FORM format selection and the Dstream ACV limits.
     *
     * IPR mode bits I_CTL[VA_FORM_32] and VA_CTL[VA_FORM_32] should be
     * consistent when executing in native mode.
     */
    switch (fi->type) {
    case AlphaFault_Access:
    case AlphaFault_Address:
    case AlphaFault_Permission:
        return fi->ifetch ? EXCP_IACV
                          : EXCP_DFAULT;
    case AlphaFault_Translation:
        return fi->ifetch ? EXCP_ITB_MISS
                          : EXCP_DTBM_SINGLE;
    case AlphaFault_Double:
        return alpha_va48_enabled(env, true) ? EXCP_DTBM_DOUBLE_4
                                             : EXCP_DTBM_DOUBLE_3;
    case AlphaFault_Alignment:
        return EXCP_UNALIGNED;
    default:
        g_assert_not_reached();
    }
}

static uint32_t get_exception_syndrome(CPUAlphaState *env,
                                       GetPhysAddrFlags gpaf,
                                       MMUAccessType access_type,
                                       AlphaMMUFaultInfo *fi)
{
    uint32_t iss = 0;
    bool accvio;

    /*
     * Compute fault-on-read/write and access bits.
     */
    accvio = fi->type == AlphaFault_Address ||
             fi->type == AlphaFault_Alignment ||
             fi->type == AlphaFault_Permission;

    /*
     * "Fault on Read" (FOR) occurs when a read is attempted when PTE<FOR> is
     * set. The same applies to "Fault on Write" (FOW), except in the case of
     * "hw_ld" with "WrtChk" (write check), where both cases end up applying.
     */
    switch (access_type) {
    case MMU_DATA_LOAD:
    case MMU_DATA_STORE:
        if (access_is_effective_write(access_type) || gpaf.write_check) {
            SET_FIELD(iss, MMU_ISS, FOW, fi->fault_write);
        }
        if (access_is_effective_read(access_type)) {
            SET_FIELD(iss, MMU_ISS, FOR, fi->fault_read);
        }
        SET_FIELD(iss, MMU_ISS, ACV, accvio);
        SET_FIELD(iss, MMU_ISS, WR,
                  access_is_effective_write(access_type));
        break;

    case MMU_INST_FETCH:
        SET_FIELD(iss, MMU_ISS, ACV, accvio);
        break;
    }

    switch (access_type) {
    case MMU_DATA_LOAD:
    case MMU_DATA_STORE:
        break;

    case MMU_INST_FETCH:
        SET_FIELD(iss, MMU_ISS, OVFL, fi->type == AlphaFault_Address);

        /*
         * Table 5–8 Exception Summary Register Fields Description
         *
         * This bit should be used by the IACV PALcode routine to determine
         * whether the offending I-stream virtual address is latched in the
         * EXC_ADDR register or the VA register. If BAD_IVA is clear, then
         * EXC_ADDR contains the address, if BAD_IVA is set then VA contains
         * the address.
         */
        SET_FIELD(iss, MMU_ISS, IVA, 0);

        /* XXX: Need to handle: */
        /*
         * D.12 Guideline 16 : JSR-BAD VA
         *
         * A JSR memory format instruction that generates a bad VA (IACV) trap
         * requires PALcode assistance to determine the correct exception
         * address. If the EXC_SUM[BAD_IVA] is set, bits [63,1] of the
         * exception address are valid in the VA IPR and not the EXC_ADDR
         * as usual. The PALmode bit, however, is always located in
         * EXC_ADDR[0] and must be combined, if necessary, by PALcode to
         * determine the full exception address.
         */
        break;
    }

    return iss;
}

void alpha_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr, vaddr addr,
                                     unsigned size, MMUAccessType access_type,
                                     int mmu_idx, MemTxAttrs attrs,
                                     MemTxResult response, uintptr_t retaddr)
{
    /*
     * 4.7.9 Nonexistent Memory Processing
     *
     * Like its predecessors, the 21264/EV6 can generate references to
     * nonexistent (NXM) memory or I/O space. However, unlike the earlier
     * Alpha microprocessor implementations, the 21264/EV6 can generate
     * speculative references to memory space. To accommodate the
     * speculative nature of the 21264/EV6, the system must not generate or
     * lock error registers because of speculative references.
     */

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Encountered NXM at 0x" TARGET_FMT_lx " while accessing "
                  "I/O region at " HWADDR_FMT_plx "\n",
                  (target_ulong)addr, physaddr);
}


/* Raise a data fault alignment exception for the specified virtual address */
void alpha_cpu_do_unaligned_access(CPUState *cs, vaddr address,
                                   MMUAccessType access_type,
                                   int mmu_idx, uintptr_t retaddr)
{
    AlphaMMUFaultInfo fi = { .type = AlphaFault_Alignment };
    GetPhysAddrFlags gpaf = {};
    CPUAlphaState *env = cpu_env(cs);
    uint32_t iss;

    env->exception.vaddress = address;
    iss = get_exception_syndrome(env, gpaf, access_type, &fi);
    do_raise_exception_ra(env, compute_exception_code(env, &fi),
                          syn_mmu_abort(iss), retaddr);
}

hwaddr alpha_cpu_get_phys_page_debug(CPUState *cs, vaddr address)
{
    CPUAlphaState *env = cpu_env(cs);
    AlphaMMUFaultInfo fi = {};
    GetPhysAddrResult res = {};
    GetPhysAddrFlags gpaf = {};
    bool ret;

    /*
     * We have separate TLBs for code and data.
     *
     * If we only try a MMU_DATA_LOAD, we may not be able to read instructions
     * mapped by instruction TLBs, so we also try a MMU_INST_FETCH as well.
     */
    ret = get_phys_addr(env, address, MMU_DATA_LOAD,
                        alpha_mmu_index(env, false), gpaf, &res, &fi);
    if (!ret) {
        ret = get_phys_addr(env, address, MMU_INST_FETCH,
                            alpha_mmu_index(env, true), gpaf, &res, &fi);
        if (!ret) {
            return -1;
        }
    }

    return res.f.phys_addr;
}

bool alpha_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr)
{
    CPUAlphaState *env = cpu_env(cs);
    AlphaMMUFaultInfo fi = {};
    GetPhysAddrResult res = {};
    GetPhysAddrFlags gpaf = {};
    AlphaMMUIdx map_idx = mmu_idx;
    vaddr vaddr;
    uint32_t iss;
    bool ret;

    switch (mmu_idx) {
    case AlphaMMUIdx_Physical:
        assert(access_type == MMU_DATA_LOAD ||
               access_type == MMU_DATA_STORE);
        break;
    case AlphaMMUIdx_Privileged:
        /*
         * The EV6 manual does not say anything about standard hw_{ld,st}
         * instruction accesses, but based on the description below for
         * Virtual/Alt, this mode must use the current mode bits instead
         * of the DTB_ALTMODE bits.
         *
         * The EV5 manual also states that "memory-management checks use MTU
         * IPR DTB_CM for access checks" when AltMode is zero as well.
         *
         * Effectively, this is a normal load/store without alignment checks.
         *
         * The extra MMU index is probably unnecessary.
         */
        assert(access_type == MMU_DATA_LOAD ||
               access_type == MMU_DATA_STORE);
        map_idx = alpha_mmu_index(env, false);
        break;
    case AlphaMMUIdx_PrivilegedVPTE:
        /*
         * Table 6–3 HW_LD Instruction Fields Descriptions
         *
         * Virtual/VPTE — Flags a virtual PTE fetch (LD_VPTE). Used by trap
         * logic to distinguish a single TB miss from a double TB miss.
         * Kernel mode access checks are performed.
         */
        assert(access_type == MMU_DATA_LOAD);
        map_idx = AlphaMMUIdx_Kernel;
        gpaf.vpte_fetch = true;
        break;
    case AlphaMMUIdx_PrivilegedWChk:
        /*
         * Table 6–3 HW_LD Instruction Fields Descriptions
         *
         * Virtual/WrChk - The effective address for the HW_LD instruction is
         * virtual. Access checks for fault-on-read (FOR), fault-on-write (FOW),
         * read and write protection are performed.
         */
        assert(access_type == MMU_DATA_LOAD);
        map_idx = alpha_mmu_index(env, false);
        gpaf.write_check = true;
        break;
    case AlphaMMUIdx_AltMode:
        /*
         * Table 6–3 HW_LD Instruction Fields Descriptions
         *
         * Virtual/Alt - The effective address for the HW_LD instruction is
         * virtual. Access checks use the DTB_ALT_MODE IPR.
         */
        assert(access_type == MMU_DATA_LOAD ||
               access_type == MMU_DATA_STORE);
        map_idx = alpha_mmu_index_altmode(env);
        break;
    case AlphaMMUIdx_AltModeWChk:
        /*
         * Table 6–3 HW_LD Instruction Fields Descriptions
         *
         * Virtual/WrChk/Alt - The effective address for the HW_LD instruction
         * is virtual. Access checks for FOR, FOW, read and write protection.
         * Access checks use the DTB_ALT_MODE IPR.
         */
        assert(access_type == MMU_DATA_LOAD);
        map_idx = alpha_mmu_index_altmode(env);
        gpaf.write_check = true;
        break;
    }

    vaddr = address & TARGET_PAGE_MASK;
    ret = get_phys_addr(env, vaddr, access_type, map_idx, gpaf, &res, &fi);
    if (likely(ret)) {
        qemu_log_mask(CPU_LOG_MMU, "%s: %s at virt=0x" TARGET_FMT_lx
                      " phys=0x" HWADDR_FMT_plx " prot=%c%c%c mmu_idx=%d\n",
                      __func__, access_str(access_type), (target_ulong)vaddr,
                      res.f.phys_addr,
                      res.f.prot & PAGE_READ ? 'r' : '-',
                      res.f.prot & PAGE_WRITE ? 'w' : '-',
                      res.f.prot & PAGE_EXEC ? 'x' : '-',
                      map_idx);
        tlb_set_page_full(cs, mmu_idx, vaddr, &res.f);
        return true;
    }

    if (probe) {
        return false;
    }

    qemu_log_mask(CPU_LOG_MMU, "%s: %s at virt=0x" TARGET_FMT_lx
                  " mmu_idx=%u error=%s\n",
                  __func__, access_str(access_type),
                  (target_ulong)vaddr, map_idx, fault_str(fi.type));

    env->exception.vaddress = address;
    iss = get_exception_syndrome(env, gpaf, access_type, &fi);
    do_raise_exception_ra(env, compute_exception_code(env, &fi),
                          syn_mmu_abort(iss), retaddr);
}

static void alpha_tlb_dump(CPUAlphaState *env, AlphaTLBType tlb_type)
{
    static const struct {
        const char *type;
        uint32_t mask;
    } spe_states[] = {
        { "48-bit", 0b0100 },
        { "43-bit", 0b0010 },
        { "32-bit", 0b0001 },
    };
    static const char *const gh_to_str[4] = { "8K", "64K", "512K", "4M" };
    static const char *const type[] = {
        [AlphaTLBType_Data]        = "Data",
        [AlphaTLBType_Instruction] = "Instruction",
    };
    static const bool ifetch[] = {
        [AlphaTLBType_Data]        = false,
        [AlphaTLBType_Instruction] = true,
    };
    char read_type, write_type;
    CPUAlphaTLBContext *ctx;
    CPUAlphaTLBEntry *entry;
    int i, granule, spe;
    bool va48;

    write_type = 'W';
    read_type = ifetch[tlb_type] ? 'X' : 'R';
    spe = alpha_superpage_status(env, ifetch[tlb_type]);
    va48 = alpha_va48_enabled(env, ifetch[tlb_type]);
    ctx = get_tlb_context(env, tlb_type);

    qemu_printf("\n%s TLB:\n", type[tlb_type]);
    qemu_printf("Current ASID: %d\n", alpha_cur_asn(env, tlb_type, 0));
    qemu_printf("Address bits: %d bits\n", regime_address_bits(va48));
    qemu_printf("Superpage state:\n");
    for (i = 0; i < ARRAY_SIZE(spe_states); i++) {
        qemu_printf("%8s: %s\n",
                    spe_states[i].type,
                    spe & spe_states[i].mask ? "yes" : "no");
    }
    qemu_printf("Index Physical           Virtual            Size ASID  ASM "
                "K  E  S  U  FaultOn\n");
    for (i = 0; i < ARRAY_SIZE(ctx->entries); i++) {
        uint64_t vaddr;

        entry = &ctx->entries[i];
        if (!FIELD_EX64(entry->flags, PTE, VALID)) {
            continue;
        }

        granule = pte_gh_raw(entry->flags) & 0x03;
        vaddr = sextract64(entry->vaddr, 0, regime_address_bits(va48));
        qemu_printf("%5d 0x"HWADDR_FMT_plx " 0x"TARGET_FMT_lx " %4s %-5u %1u   "
                    "%c%c %c%c %c%c %c%c %c%c\n",
                    i, entry->paddr, vaddr, gh_to_str[granule], entry->asn,
                    FIELD_EX32(entry->flags, PTE, ASM),
                    FIELD_EX32(entry->flags, PTE, KRE) ? read_type : '-',
                    FIELD_EX32(entry->flags, PTE, KWE) ? write_type : '-',
                    FIELD_EX32(entry->flags, PTE, ERE) ? read_type : '-',
                    FIELD_EX32(entry->flags, PTE, EWE) ? write_type : '-',
                    FIELD_EX32(entry->flags, PTE, SRE) ? read_type : '-',
                    FIELD_EX32(entry->flags, PTE, SWE) ? write_type : '-',
                    FIELD_EX32(entry->flags, PTE, URE) ? read_type : '-',
                    FIELD_EX32(entry->flags, PTE, UWE) ? write_type : '-',
                    FIELD_EX32(entry->flags, PTE, FOR) ? read_type : '-',
                    FIELD_EX32(entry->flags, PTE, FOW) ? write_type : '-');
    }
}

void alpha_cpu_dump_mmu(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);

    alpha_tlb_dump(env, AlphaTLBType_Instruction);
    alpha_tlb_dump(env, AlphaTLBType_Data);
}
