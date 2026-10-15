/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha Machine State
 */

#include "qemu/osdep.h"
#include "migration/cpu.h"
#include "migration/qemu-file.h"
#include "cpu.h"
#include "internals.h"

static int get_fpcr(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field)
{
    AlphaCPU *cpu = opaque;
    CPUAlphaState *env = &cpu->env;

    alpha_cpu_set_fpcr(env, qemu_get_be64(f));
    return 0;
}

static int put_fpcr(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field, JSONWriter *vmdesc)
{
    AlphaCPU *cpu = opaque;
    CPUAlphaState *env = &cpu->env;

    qemu_put_be64(f, alpha_cpu_get_fpcr(env));
    return 0;
}

static bool fpu_state_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_fpu = {
    .name = "cpu/fpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = fpu_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(env.fpregs, AlphaCPU, 32),
        /*
         * Save the architecture value of the fpcr, not the internally
         * expanded version.  Since this architecture value does not
         * exist in memory to be stored, this requires a but of hoop
         * jumping.  We want OFFSET=0 so that we effectively pass CPU
         * to the helper functions.
         */
        {
            .name = "fpcr",
            .version_id = 0,
            .size = sizeof(uint64_t),
            .info = &(const VMStateInfo) {
                .name = "fpcr",
                .get = get_fpcr,
                .put = put_fpcr,
            },
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST()
    }
};

static bool tlb_state_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_tlb_entry = {
    .name = "cpu/tlb-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(vaddr, CPUAlphaTLBEntry),
        VMSTATE_UINT64(paddr, CPUAlphaTLBEntry),
        VMSTATE_UINT64(match_mask, CPUAlphaTLBEntry),
        VMSTATE_UINT64(keep_mask, CPUAlphaTLBEntry),
        VMSTATE_UINT16(flags, CPUAlphaTLBEntry),
        VMSTATE_UINT8(asn, CPUAlphaTLBEntry),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_tlb_context = {
    .name = "cpu/tlb-context",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(last_way, CPUAlphaTLBContext),
        VMSTATE_STRUCT_ARRAY(entries, CPUAlphaTLBContext, MAX_TLB_ENTRIES, 1,
                             vmstate_tlb_entry, CPUAlphaTLBEntry),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_tlb = {
    .name = "cpu/tlb",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = tlb_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(env.tlb, AlphaCPU, 2, 1, vmstate_tlb_context,
                             CPUAlphaTLBContext),
        VMSTATE_END_OF_LIST()
    }
};

static int get_power(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field)
{
    AlphaCPU *cpu = opaque;
    bool powered_off = qemu_get_byte(f);
    cpu->power_state = powered_off ? CPU_POWER_OFF : CPU_POWER_ON;
    return 0;
}

static int put_power(QEMUFile *f, void *opaque, size_t size,
                    const VMStateField *field, JSONWriter *vmdesc)
{
    AlphaCPU *cpu = opaque;

    /* Migration should never happen while we transition power states */
    if (cpu->power_state == CPU_POWER_ON ||
        cpu->power_state == CPU_POWER_OFF) {
        bool powered_off = (cpu->power_state == CPU_POWER_OFF) ? true : false;
        qemu_put_byte(f, powered_off);
        return 0;
    } else {
        return 1;
    }
}

static int cpu_pre_save(void *opaque)
{
    AlphaCPU *cpu = opaque;

    if (!write_cpustate_to_list(cpu)) {
        /* This should never fail. */
        g_assert_not_reached();
    }

    cpu->ipreg_vmstate_array_len = cpu->ipreg_array_len;
    memcpy(cpu->ipreg_vmstate_indexes, cpu->ipreg_indexes,
           cpu->ipreg_array_len * sizeof(uint64_t));
    memcpy(cpu->ipreg_vmstate_values, cpu->ipreg_values,
           cpu->ipreg_array_len * sizeof(uint64_t));
    return 0;
}

static void cpu_post_save(void *opaque)
{
    g_assert_not_reached();
}

static int cpu_pre_load(void *opaque)
{
    AlphaCPU *cpu = opaque;
    CPUAlphaState *env = &cpu->env;

    alpha_cpu_set_fpcr(env, 0);
    return 0;
}

static int cpu_post_load(void *opaque, int version_id)
{
    AlphaCPU *cpu = opaque;
    CPUAlphaState *env = &cpu->env;
    int i, v;

    /* Update the values list from the incoming migration data.
     * Anything in the incoming data which we don't know about is
     * a migration failure; anything we know about but the incoming
     * data doesn't specify retains its current (reset) value.
     * The indexes list remains untouched -- we only inspect the
     * incoming migration index list so we can match the values array
     * entries with the right slots in our own values array.
     */
    for (i = 0, v = 0; i < cpu->ipreg_array_len
             && v < cpu->ipreg_vmstate_array_len; i++) {
        if (cpu->ipreg_vmstate_indexes[v] > cpu->ipreg_indexes[i]) {
            /* register in our list but not incoming : skip it */
            continue;
        }
        if (cpu->ipreg_vmstate_indexes[v] < cpu->ipreg_indexes[i]) {
            /* register in their list but not ours: fail migration */
            return -1;
        }
        /* matching register, copy the value over */
        cpu->ipreg_values[i] = cpu->ipreg_vmstate_values[v];
        v++;
    }

    if (!write_list_to_cpustate(cpu)) {
        return -1;
    }

    env->cc_ticks_read = env->cc_ticks_then;
    alpha_rebuild_hflags(env);
    return 0;
}

const VMStateDescription vmstate_alpha_cpu = {
    .name = "cpu",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = cpu_pre_save,
    .post_save = cpu_post_save,
    .pre_load = cpu_pre_load,
    .post_load = cpu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, AlphaCPU, 0, vmstate_cpu_common, CPUState),
        VMSTATE_UINT64_ARRAY(env.gpregs, AlphaCPU, 32),
        VMSTATE_UINT64(env.pc, AlphaCPU),
        VMSTATE_BOOL(env.pal_mode, AlphaCPU),
        VMSTATE_UINT64(env.llsc_addr, AlphaCPU),
        VMSTATE_UINT64(env.llsc_val, AlphaCPU),
        VMSTATE_UINT64(env.rc, AlphaCPU),
        VMSTATE_UINT64(env.cc_ns_then, AlphaCPU),
        VMSTATE_UINT64(env.cc_ticks_then, AlphaCPU),
        VMSTATE_UINT64(env.exception.vaddress, AlphaCPU),
        VMSTATE_UINT32(env.exception.syndrome, AlphaCPU),
        VMSTATE_UINT32(env.irq_line_state, AlphaCPU),
        VMSTATE_UINT64_ARRAY(env.pal_shadow, AlphaCPU, 8),
        VMSTATE_INT32_POSITIVE_LE(ipreg_vmstate_array_len, AlphaCPU),
        VMSTATE_VARRAY_INT32(ipreg_vmstate_indexes, AlphaCPU,
                             ipreg_vmstate_array_len,
                             0, vmstate_info_uint64, uint64_t),
        VMSTATE_VARRAY_INT32(ipreg_vmstate_values, AlphaCPU,
                             ipreg_vmstate_array_len,
                             0, vmstate_info_uint64, uint64_t),
        {
            .name = "power_state",
            .version_id = 0,
            .size = sizeof(bool),
            .info = &(const VMStateInfo) {
                .name = "powered_off",
                .get = get_power,
                .put = put_power,
            },
            .flags = VMS_SINGLE,
            .offset = 0,
        },
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_fpu,
        &vmstate_tlb,
        NULL,
    }
};
