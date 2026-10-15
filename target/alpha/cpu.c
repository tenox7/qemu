/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Alpha CPU
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "accel/tcg/cpu-loop.h"
#include "fpu/softfloat-helpers.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/intc.h"
#include "tcg/tcg.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"
#include "exec/translation-block.h"
#include "monitor/hmp.h"
#include "internals.h"
#include "ipregs.h"
#include "trace.h"

const char *const alpha_gregnames[32] = {
    "v0",  "t0",  "t1",  "t2",  "t3",  "t4",  "t5",  "t6",
    "t7",  "s0",  "s1",  "s2",  "s3",  "s4",  "s5",  "fp",
    "a0",  "a1",  "a2",  "a3",  "a4",  "a5",  "t8",  "t9",
    "t10", "t11", "ra",  "t12", "at",  "gp",  "sp",  "zero",
};

const char *const alpha_fregnames[32] = {
    "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
    "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
    "f16", "f17", "f18", "f19", "f20", "f21", "f21", "f22",
    "f23", "f24", "f25", "f26", "f27", "f29", "f30", "f31",
};

void cpu_interrupt_exittb(CPUState *cs)
{
    BQL_LOCK_GUARD();
    cpu_interrupt(cs, CPU_INTERRUPT_EXITTB);
}

uint32_t alpha_ldl_phys(CPUState *cs, hwaddr addr)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = alpha_cpu_address_space(cs, attrs);

    return address_space_ldl_le(as, addr, attrs, NULL);
}

uint64_t alpha_ldq_phys(CPUState *cs, hwaddr addr)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = alpha_cpu_address_space(cs, attrs);

    return address_space_ldq_le(as, addr, attrs, NULL);
}

void alpha_stl_phys(CPUState *cs, hwaddr addr, uint32_t val)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = alpha_cpu_address_space(cs, attrs);

    address_space_stl_le(as, addr, val, attrs, NULL);
}

void alpha_stq_phys(CPUState *cs, hwaddr addr, uint64_t val)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = alpha_cpu_address_space(cs, attrs);

    address_space_stq_le(as, addr, val, attrs, NULL);
}

static void ev4_excp_init(AlphaCPU *cpu)
{
    CPUAlphaState *env = &cpu->env;

    env->excp_vectors[EXCP_RESET]         = 0x0000;
    env->excp_vectors[EXCP_MCHK]          = 0x0020;
    env->excp_vectors[EXCP_ARITH]         = 0x0060;
    env->excp_vectors[EXCP_IRQ]           = 0x00E0;
    env->excp_vectors[EXCP_DFAULT]        = 0x01E0;
    env->excp_vectors[EXCP_ITB_MISS]      = 0x03E0;
    env->excp_vectors[EXCP_IACV]          = 0x07E0;
    env->excp_vectors[EXCP_DTBM_SINGLE]   = 0x08E0;
    env->excp_vectors[EXCP_DTBM_DOUBLE_3] = 0x09E0;
    env->excp_vectors[EXCP_UNALIGNED]     = 0x11E0;
    env->excp_vectors[EXCP_OPCDEC]        = 0x13E0;
    env->excp_vectors[EXCP_FEN]           = 0x17E0;
}

static void ev5_excp_init(AlphaCPU *cpu)
{
    CPUAlphaState *env = &cpu->env;

    env->excp_vectors[EXCP_RESET]         = 0x0000;
    env->excp_vectors[EXCP_IACV]          = 0x0080;
    env->excp_vectors[EXCP_IRQ]           = 0x0100;
    env->excp_vectors[EXCP_ITB_MISS]      = 0x0180;
    env->excp_vectors[EXCP_DTBM_SINGLE]   = 0x0200;
    env->excp_vectors[EXCP_DTBM_DOUBLE_3] = 0x0280;
    env->excp_vectors[EXCP_UNALIGNED]     = 0x0300;
    env->excp_vectors[EXCP_DFAULT]        = 0x0380;
    env->excp_vectors[EXCP_MCHK]          = 0x0400;
    env->excp_vectors[EXCP_OPCDEC]        = 0x0480;
    env->excp_vectors[EXCP_ARITH]         = 0x0500;
    env->excp_vectors[EXCP_FEN]           = 0x0580;
}

static void ev6_excp_init(AlphaCPU *cpu)
{
    CPUAlphaState *env = &cpu->env;

    env->excp_vectors[EXCP_DTBM_DOUBLE_3] = 0x0100;
    env->excp_vectors[EXCP_DTBM_DOUBLE_4] = 0x0180;
    env->excp_vectors[EXCP_FEN]           = 0x0200;
    env->excp_vectors[EXCP_UNALIGNED]     = 0x0280;
    env->excp_vectors[EXCP_DTBM_SINGLE]   = 0x0300;
    env->excp_vectors[EXCP_DFAULT]        = 0x0380;
    env->excp_vectors[EXCP_OPCDEC]        = 0x0400;
    env->excp_vectors[EXCP_IACV]          = 0x0480;
    env->excp_vectors[EXCP_MCHK]          = 0x0500;
    env->excp_vectors[EXCP_ITB_MISS]      = 0x0580;
    env->excp_vectors[EXCP_ARITH]         = 0x0600;
    env->excp_vectors[EXCP_IRQ]           = 0x0680;
    env->excp_vectors[EXCP_MT_FPCR]       = 0x0700;
    env->excp_vectors[EXCP_RESET]         = 0x0780;
}

void alpha_cpu_post_init(Object *obj)
{
}

static void alpha_ev4_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev4;
    cpu->chip_id = 0b000111;            /* 2106x, production version */
    cpu->proc_id = PROCID_EV4;
    cpu->isa_implver = IMPLVER_2106x;
    cpu->isa_amask = 0;
    ev4_excp_init(cpu);
}

static void alpha_ev45_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev4;
    cpu->chip_id = 0b000111;            /* 2106x, production version */
    cpu->proc_id = PROCID_EV45;
    cpu->isa_implver = IMPLVER_2106x;
    cpu->isa_amask = 0;
    ev4_excp_init(cpu);
}

static void alpha_ev5_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev5;
    cpu->chip_id = 0b000101;            /* 21164/EV5 pass 2.2 */
    cpu->proc_id = PROCID_EV5;
    cpu->isa_implver = IMPLVER_21164;
    cpu->isa_amask = 0;
    ev5_excp_init(cpu);
}

static void alpha_ev56_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev5;
    cpu->chip_id = 0b000110;            /* 21164A/EV56 pass 1 */
    cpu->proc_id = PROCID_EV56;
    cpu->isa_implver = IMPLVER_21164;
    cpu->isa_amask = AMASK_BWX;
    ev5_excp_init(cpu);
}

static void alpha_pca56_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev5;
    cpu->chip_id = 0b000111;            /* 21164PC/PCA56 pass 1 */
    cpu->proc_id = PROCID_PCA56;
    cpu->isa_implver = IMPLVER_21164;
    cpu->isa_amask = AMASK_BWX | AMASK_MVI;
    ev5_excp_init(cpu);
}

static void alpha_ev6_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev6;
    cpu->chip_id = 0b000110;            /* 21264/EV6 pass 2.5 */
    cpu->proc_id = PROCID_EV6;
    cpu->isa_implver = IMPLVER_21264;
    cpu->isa_amask = AMASK_BWX | AMASK_FIX | AMASK_MVI | AMASK_TRAP;
    ev6_excp_init(cpu);
}

static void alpha_ev67_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev6;
    cpu->chip_id = 0b000111;        /* 21264/EV67 pass 2.5 */
    cpu->proc_id = PROCID_EV67;
    cpu->isa_implver = IMPLVER_21264;
    cpu->isa_amask = AMASK_BWX | AMASK_FIX | AMASK_MVI | AMASK_CIX |
                     AMASK_TRAP;
    ev6_excp_init(cpu);
}

static void alpha_ev68al_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev6;
    cpu->chip_id = 0b010010;        /* EV68AL pass 2.1 */
    cpu->proc_id = PROCID_EV68AL;
    cpu->isa_implver = IMPLVER_21264;
    cpu->isa_amask = AMASK_BWX | AMASK_FIX | AMASK_MVI | AMASK_CIX |
                     AMASK_TRAP | AMASK_PREFETCH;
    ev6_excp_init(cpu);
}

static void alpha_ev68cb_initfn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    cpu->bfd_mach = bfd_mach_alpha_ev6;
    cpu->chip_id = 0b011100;        /* EV68CB/EV68DC pass 2.4 */
    cpu->proc_id = PROCID_EV68CB;
    cpu->isa_implver = IMPLVER_21264;
    cpu->isa_amask = AMASK_BWX | AMASK_FIX | AMASK_MVI | AMASK_CIX |
                     AMASK_TRAP | AMASK_PREFETCH;
    ev6_excp_init(cpu);
}

static const AlphaCPUInfo alpha_cpus[] = {
    { .name = "ev4",        .initfn = alpha_ev4_initfn },
    { .name = "ev45",       .initfn = alpha_ev45_initfn },
    { .name = "ev5",        .initfn = alpha_ev5_initfn },
    { .name = "ev56",       .initfn = alpha_ev56_initfn },
    { .name = "pca56",      .initfn = alpha_pca56_initfn },
    { .name = "ev6",        .initfn = alpha_ev6_initfn },
    { .name = "ev67",       .initfn = alpha_ev67_initfn },
    { .name = "ev68al",     .initfn = alpha_ev68al_initfn },
    { .name = "ev68cb",     .initfn = alpha_ev68cb_initfn },
};

const AlphaCPUAlias alpha_cpu_aliases[] = {
    /* Model number aliases */
    { .alias = "21064",     .model = "ev4" },
    { .alias = "21064a",    .model = "ev45" },
    { .alias = "21164",     .model = "ev5" },
    { .alias = "21164a",    .model = "ev56" },
    { .alias = "21164pc",   .model = "pca56" },
    { .alias = "21264",     .model = "ev6" },
    { .alias = "21264a",    .model = "ev67" },
    { .alias = "21264b",    .model = "ev68al" },
    { .alias = "21264c",    .model = "ev68cb" },

    /* Other aliases */
    { .alias = "ev4s",      .model = "ev4" },
    { .alias = "pca57",     .model = "pca56" },
    { .alias = "ev68",      .model = "ev68al" },
    { .alias = "ev68a",     .model = "ev68al" },
    { .alias = "ev68dc",    .model = "ev68cb" },

    /* Terminator, must be null. */
    { .alias = NULL,        .model = NULL },
};

static const char *alpha_cpu_lookup_alias(const char *alias)
{
    int i;

    for (i = 0; alpha_cpu_aliases[i].alias != NULL; i++) {
        if (strcmp(alpha_cpu_aliases[i].alias, alias) == 0) {
            return alpha_cpu_aliases[i].model;
        }
    }
    return NULL;
}

ObjectClass *alpha_cpu_class_by_name(const char *name)
{
    g_autofree char *cpu_model = g_ascii_strdown(name, -1);
    g_autofree char *typename = NULL;
    ObjectClass *oc;
    const char *p;

    p = alpha_cpu_lookup_alias(cpu_model);
    if (p) {
        g_free(cpu_model);
        cpu_model = g_strdup(p);
    }

    typename = g_strdup_printf("%s" ALPHA_CPU_TYPE_SUFFIX, cpu_model);
    oc = object_class_by_name(typename);
    return oc;
}

static void alpha_cpu_list_entries(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    const char *typename = object_class_get_name(oc);
    g_autofree char *name = cpu_model_from_type(typename);

    qemu_printf("  %-16s\n", name);
}

static void alpha_cpu_list_aliases(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    const char *typename = object_class_get_name(oc);
    g_autofree char *name = cpu_model_from_type(typename);
    int i;

    for (i = 0; alpha_cpu_aliases[i].alias != NULL; i++) {
        const AlphaCPUAlias *alias = &alpha_cpu_aliases[i];
        ObjectClass *alias_oc = alpha_cpu_class_by_name(alias->model);

        if (alias_oc != oc) {
            continue;
        }

        qemu_printf("  %-16s (alias for %s)\n", alias->alias, name);
    }
}

void alpha_cpu_list(void)
{
    g_autoptr(GSList) list;

    qemu_printf("Available CPUs:\n");
    list = object_class_get_list_sorted(TYPE_ALPHA_CPU, false);
    g_slist_foreach(list, alpha_cpu_list_entries, NULL);
    g_slist_foreach(list, alpha_cpu_list_aliases, NULL);
}

const char *alpha_cpu_exception_name(int excp)
{
    static const char * const excp_names[] = {
        [EXCP_DTBM_DOUBLE_3]    = "dtbm_double_3",
        [EXCP_DTBM_DOUBLE_4]    = "dtbm_double_4",
        [EXCP_FEN]              = "fen",
        [EXCP_UNALIGNED]        = "unaligned",
        [EXCP_DTBM_SINGLE]      = "dtbm_single",
        [EXCP_DFAULT]           = "dfault",
        [EXCP_OPCDEC]           = "opcdec",
        [EXCP_IACV]             = "iacv",
        [EXCP_MCHK]             = "mchk",
        [EXCP_ITB_MISS]         = "itb_miss",
        [EXCP_ARITH]            = "arith",
        [EXCP_IRQ]              = "irq",
        [EXCP_MT_FPCR]          = "mt_fpcr",
        [EXCP_RESET]            = "reset",
        [EXCP_CALL_PAL]         = "call_pal",
        [EXCP_ASTIRQ]           = "astirq",
        [EXCP_SIRQ]             = "sirq",
        [EXCP_HW_REI]           = "hw_rei",
    };
    const char *exc = NULL;
    if (excp >= 0 && excp < ARRAY_SIZE(excp_names)) {
        exc = excp_names[excp];
    }
    if (!exc) {
        exc = "unknown";
    }
    return exc;
}

void alpha_cpu_set_pc(CPUState *cs, vaddr value)
{
    alpha_env_set_pc(cpu_env(cs), value);
}

vaddr alpha_cpu_get_pc(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);
    return env->pc;
}

void alpha_synchronize_from_tb(CPUState *cs, const TranslationBlock *tb)
{
    CPUAlphaState *env = cpu_env(cs);

    /* The program counter is always up to date with CF_PCREL. */
    if (!(tb_cflags(tb) & CF_PCREL)) {
        env->pc = tb->pc;
    }
}

void alpha_restore_state_to_opc(CPUState *cs, const TranslationBlock *tb,
                                const uint64_t *data)
{
    CPUAlphaState *env = cpu_env(cs);

    if (tb_cflags(tb) & CF_PCREL) {
        env->pc = (env->pc & TARGET_PAGE_MASK) | data[0];
    } else {
        env->pc = data[0];
    }
}

#ifndef CONFIG_USER_ONLY
static bool alpha_cpu_has_work(CPUState *cs)
{
    CPUAlphaState *env = cpu_env(cs);
    AlphaCPU *cpu = env_archcpu(env);
    int mask = CPU_INTERRUPT_HARD | CPU_INTERRUPT_EXITTB |
               CPU_INTERRUPT_SIRQ | CPU_INTERRUPT_ASTIRQ;

    return (cpu->power_state != CPU_POWER_OFF) &&
           (cs->interrupt_request & mask);
}
#endif /* !CONFIG_USER_ONLY */

static int alpha_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return alpha_env_mmu_index(cpu_env(cs), ifetch);
}

void alpha_set_default_fp_behaviors(float_status *fpst)
{
    set_float_detect_tininess(float_tininess_after_rounding, fpst);
    set_float_ftz_detection(float_ftz_after_rounding, fpst);
    set_float_2nan_prop_rule(float_2nan_prop_ba, fpst);
    set_float_3nan_prop_rule(float_3nan_prop_cba, fpst);
    set_float_infzeronan_rule(float_infzeronan_dnan_never, fpst);

    /* Default NaN: sign bit set, most significant frac bit set */
    set_float_default_nan_pattern(0b11000000, fpst);
}

#define CPU_FREQ_HZ_DEFAULT     500000000ULL

static void alpha_cpu_init_clock(AlphaCPU *cpu)
{
    if (!clock_get(cpu->refclk)) {
#ifndef CONFIG_USER_ONLY
        g_autofree char *cpu_freq_str = freq_to_str(CPU_FREQ_HZ_DEFAULT);

        warn_report("CPU input clock is not connected to any output clock, "
                    "using default frequency of %s.", cpu_freq_str);
#endif
        /* Initialize the frequency in case the clock remains unconnected. */
        clock_set_hz(cpu->refclk, CPU_FREQ_HZ_DEFAULT);
    }

    /*
     * Trial and error have suggested that the PCC actually increments every
     * 32 processor cycles. The 21064 counts every cycle, as the Jensen SRM's
     * delay loop (150000 per ms at 150 MHz) assumes.
     */
    clock_set_mul_div(cpu->sysclk_div,
                      cpu->isa_implver == IMPLVER_2106x ? 1 : 32, 1);
    clock_set_source(cpu->sysclk_div, cpu->refclk);
    clock_set_source(cpu->sysclk, cpu->sysclk_div);
}

static bool alpha_get_irq_stats(InterruptStatsProvider *obj,
                                uint64_t **irq_counts, unsigned int *nb_irqs)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);
    CPUAlphaState *env = &cpu->env;

    *irq_counts = env->excp_stats;
    *nb_irqs = ARRAY_SIZE(env->excp_stats);
    return true;
}

static const char *alpha_irq_name(int pin)
{
    static const char * const irq_names[] = {
        [ALPHA_CPU_INPUT_IRQ0]      = "irq 0",
        [ALPHA_CPU_INPUT_IRQ1]      = "irq 1",
        [ALPHA_CPU_INPUT_IRQ2]      = "irq 2",
        [ALPHA_CPU_INPUT_IRQ3]      = "irq 3",
        [ALPHA_CPU_INPUT_IRQ4]      = "irq 4",
        [ALPHA_CPU_INPUT_IRQ5]      = "irq 5",
        [ALPHA_CPU_INPUT_PWRFAIL]   = "power failure",
        [ALPHA_CPU_INPUT_MCHK]      = "machine check",
        [ALPHA_CPU_INPUT_HLT]       = "halt request",
        [ALPHA_CPU_INPUT_CRR]       = "corrected read error",
        [ALPHA_CPU_INPUT_SLR]       = "serial line",
        [ALPHA_CPU_INPUT_PC0]       = "performance counter 0",
        [ALPHA_CPU_INPUT_PC1]       = "performance counter 1",
        [ALPHA_CPU_INPUT_PC2]       = "performance counter 2",
    };
    const char *irq = NULL;
    if (pin >= 0 && pin < ARRAY_SIZE(irq_names)) {
        irq = irq_names[pin];
    }
    if (!irq) {
        irq = "unknown";
    }
    return irq;
}

static void alpha_cpu_set_irq(void *opaque, int pin, int level)
{
    AlphaCPU *cpu = opaque;
    CPUAlphaState *env = &cpu->env;
    CPUState *cs = env_cpu(env);
    int cur_level;

    if (unlikely(pin > ALPHA_CPU_INPUT_COUNT || pin < 0)) {
        return;
    }

    BQL_LOCK_GUARD();
    trace_alpha_cpu_irq_set_state(cs->cpu_index, alpha_irq_name(pin), level);

    /* Don't generate spurious events. */
    cur_level = extract64(env->irq_line_state, pin, 1);
    if ((cur_level == 1 && level != 0) ||
        (cur_level == 0 && level == 0)) {
        return;
    }

    /* All external IRQs are level sensitive. */
    env->irq_line_state = deposit64(env->irq_line_state, pin, 1, level);
    if (env->irq_line_state) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }

    trace_alpha_cpu_irq_set_exit(cs->cpu_index, pin, level,
                                 env->irq_line_state, cs->interrupt_request);
}

void alpha_emulate_srom_reset(CPUState *cs)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    AlphaCPU *cpu = ALPHA_CPU(cs);
    CPUAlphaState *env = cpu_env(cs);

    /*
     * Set state per SROM reset parameters.
     *
     * These are as follows, per the Alpha Motherboard SDK:
     *
     * r1   - dc_ctl
     * r15  - srom_rev
     * r16  - proc_id
     * r17  - mem_size
     * r18  - cycle_cnt
     * r19  - signature
     * r20  - proc_mask
     * r21  - sysctx
     */
    if (env->implver == IMPLVER_21264) {
        alpha_cpu_set_reg_value(env, IR_T0, env->ipr.dc_ctl);
    }
    alpha_cpu_set_reg_value(env, IR_S6, 0x5800000000000901);
    alpha_cpu_set_reg_value(env, IR_A0, cpu->proc_id);
    alpha_cpu_set_reg_value(env, IR_A1, ms->ram_size);
    alpha_cpu_set_reg_value(env, IR_A2, clock_get_hz(cpu->refclk));
    alpha_cpu_set_reg_value(env, IR_A3, 0xdecb0007);
    alpha_cpu_set_reg_value(env, IR_A4, MAKE_64BIT_MASK(0, ms->smp.cpus));
    alpha_cpu_set_reg_value(env, IR_A5, 0);
}

static void alpha_cpu_disas_set_info(const CPUState *cs, disassemble_info *info)
{
    AlphaCPU *cpu = ALPHA_CPU(cs);
    const gchar *flavor;

    info->endian = BFD_ENDIAN_LITTLE;
    info->mach = cpu->bfd_mach;
    info->print_insn = print_insn_alpha;

    /*
     * Allow the user to explicitly set the disassembly type based on an
     * environment variable setting. The rationale for using an environment
     * variable is that this is not really relevant to the CPU's internal
     * state and is explicitly intended for debugging purposes only.
     */
    flavor = g_getenv("ALPHA_DISAS_TYPE");
    if (flavor) {
        if (!g_ascii_strcasecmp(flavor, "unix")) {
            info->flavour = bfd_target_ecoff_flavour;
        } else if (!g_ascii_strcasecmp(flavor, "vms")) {
            info->flavour = bfd_target_evax_flavour;
        } else if (!g_ascii_strcasecmp(flavor, "nt")) {
            info->flavour = bfd_target_coff_flavour;
        }
    }
}

static void alpha_cpu_do_reset(CPUState *cs)
{
    AlphaCPU *cpu = ALPHA_CPU(cs);
    CPUAlphaState *env = cpu_env(cs);

    /* Set default FP behaviors. */
    alpha_set_default_fp_behaviors(&env->fp_status);
    alpha_cpu_set_fpcr(env, 0);

    /* Set default IPR reset values. */
    alpha_cpu_reset_ipregs(cpu);

    /* Reset all breakpoints. */
    cpu_breakpoint_remove_all(cs, BP_CPU);
    cpu_watchpoint_remove_all(cs, BP_CPU);

    /* Set power state. */
    cpu->power_state = cs->start_powered_off ? CPU_POWER_OFF : CPU_POWER_ON;

    /* Reset the timer. */
    env->cc_ns_then = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    env->cc_ticks_then = 0;

    /* Be sure no exceptions or interrupts are pending. */
    env->irq_line_state = 0;
    cs->exception_index = EXCP_NONE;

    /* Perform the reset exception. */
    alpha_cpu_do_system_reset(cs);
}

static void alpha_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    AlphaCPU *cpu = ALPHA_CPU(cs);
    AlphaCPUClass *acc = ALPHA_CPU_GET_CLASS(cpu);
    CPUAlphaState *env = cpu_env(cs);

    if (acc->parent_phases.hold) {
        (*acc->parent_phases.hold)(obj, type);
    }

    memset(env, 0, offsetof(CPUAlphaState, end_reset_fields));
    alpha_cpu_do_reset(cs);
    alpha_rebuild_hflags(env);
}

static void alpha_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    AlphaCPU *cpu = ALPHA_CPU(cs);
    AlphaCPUClass *acc = ALPHA_CPU_GET_CLASS(dev);
    CPUAlphaState *env = cpu_env(cs);
    Error *local_err = NULL;

    /* Use pc-relative instructions in system-mode */
    tcg_cflags_set(cs, CF_PCREL);

    if (!cpu_common_realize(cs, &local_err)) {
        error_propagate(errp, local_err);
        return;
    }

    alpha_cpu_register_ipregs(cpu);
    alpha_cpu_init_ipreg_list(cpu);
    alpha_cpu_init_clock(cpu);

    /* Set feature values. */
    env->bfd_mach = cpu->bfd_mach;
    env->implver = cpu->isa_implver;
    env->amask = cpu->isa_amask;

    /* Now, perform the reset. */
    qemu_init_vcpu(cs);
    cpu_reset(cs);

    (*acc->parent_realize)(dev, errp);
}

static void alpha_cpu_unrealizefn(DeviceState *dev)
{
    CPUState *cs = CPU(dev);
    AlphaCPUClass *acc = ALPHA_CPU_GET_CLASS(dev);

    cpu_remove_sync(cs);
    (*acc->parent_unrealize)(dev);
}

#define DUMP_CODE_BYTES_TOTAL    64
#define DUMP_CODE_BYTES_BACKWARD 32

void alpha_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
#define RGPL  3
#define RFPL  3
    CPUAlphaState *env = cpu_env(cs);
    uint32_t fpcr = alpha_cpu_get_fpcr(env) >> 32;
    int i;

    for (i = 0; i < ARRAY_SIZE(env->gpregs); i++) {
        qemu_fprintf(f, "%4s=" TARGET_FMT_lx "%c",
                     alpha_gregnames[i], alpha_cpu_get_reg_value(env, i),
                     (i % RGPL) == 2 ? '\n' : ' ');
    }
    qemu_fprintf(f, "\n");
    qemu_fprintf(f, "    fpcr=" TARGET_FMT_lx
                 "    %-3s %-3s %-3s %-3s %-3s\n",
                 ((uint64_t)fpcr << 32),
                 (fpcr & FPCR_INV) ? "INV" : "",
                 (fpcr & FPCR_DZE) ? "DZE" : "",
                 (fpcr & FPCR_OVF) ? "OVF" : "",
                 (fpcr & FPCR_UNF) ? "UNF" : "",
                 (fpcr & FPCR_INE) ? "INE" : "");

    qemu_fprintf(f, "      pc=" TARGET_FMT_lx " %s\n",
                 env->pc | alpha_is_pal(env), alpha_is_pal(env) ? "(P)" : "");
    if (flags & CPU_DUMP_FPU) {
        for (i = 0; i < ARRAY_SIZE(env->fpregs); i++) {
            qemu_fprintf(f, "%4s=" TARGET_FMT_lx "%c",
                         alpha_fregnames[i], env->fpregs[i],
                         (i % RFPL) == 2 ? '\n' : ' ');
        }
        qemu_fprintf(f, "\n");
    }
    if (flags & CPU_DUMP_CODE) {
        target_ulong base = env->pc;
        target_ulong offs = MIN(env->pc, DUMP_CODE_BYTES_BACKWARD);
        uint32_t code;
        char codestr[16];
        int res;

        qemu_fprintf(f, "Code=");
        for (i = 0; i < DUMP_CODE_BYTES_TOTAL; i += 4) {
            res = cpu_memory_rw_debug(cs, base - offs + i, &code,
                                      sizeof(code), 0);
            if (likely(res == 0)) {
                snprintf(codestr, sizeof(codestr), "%08x", code);
            } else {
                snprintf(codestr, sizeof(codestr), "????????");
            }
            qemu_fprintf(f, "%s%s%s%s", i > 0 ? " " : "",
                         i == offs ? "<" : "", codestr, i == offs ? ">" : "");
        }
        qemu_fprintf(f, "\n");
    }
}

#ifdef CONFIG_HMP
extern const MonitorDef alpha_monitor_defs[];
#endif
#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps alpha_sysemu_ops = {
    .has_work = alpha_cpu_has_work,
    .get_phys_addr_debug = alpha_cpu_get_phys_page_debug,
    .write_elf32_note = alpha_cpu_write_elf32_note,
    .write_elf64_note = alpha_cpu_write_elf64_note,
    .legacy_vmsd = &vmstate_alpha_cpu,
#ifdef CONFIG_HMP
    .monitor_defs = alpha_monitor_defs,
#endif
};

#include "accel/tcg/cpu-ops.h"

static TCGTBCPUState alpha_get_tb_cpu_state(CPUState *cs)
{
    TCGTBCPUState state = {};

    cpu_get_tb_cpu_state(cpu_env(cs), &state.pc, &state.cs_base, &state.flags);
    return state;
}

static const TCGCPUOps alpha_tcg_ops = {
    .mttcg_supported = true,
    .initialize = alpha_translate_init,
    .translate_code = alpha_translate_code,
    .get_tb_cpu_state = alpha_get_tb_cpu_state,
    .synchronize_from_tb = alpha_synchronize_from_tb,
    .restore_state_to_opc = alpha_restore_state_to_opc,
    .mmu_index = alpha_cpu_mmu_index,

    .tlb_fill = alpha_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_notreached,
    .cpu_exec_reset = cpu_reset,
    .cpu_exec_interrupt = alpha_cpu_exec_interrupt,
    .cpu_exec_halt = alpha_cpu_exec_halt,
    .do_interrupt = alpha_cpu_do_interrupt,
    .do_transaction_failed = alpha_cpu_do_transaction_failed,
    .do_unaligned_access = alpha_cpu_do_unaligned_access,
};

static void alpha_cpu_initfn(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    AlphaCPU *cpu = ALPHA_CPU(obj);
    CPUAlphaState *env = &cpu->env;
    int i;

    cpu->ipregs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                        NULL, g_free);

    /* Initialize the various signal pins. */
    cpu->refclk = qdev_init_clock_in(dev, "cpu-refclk", NULL, cpu, 0);
    cpu->sysclk_div = clock_new(obj, "cpu-sysclk-div");
    cpu->sysclk = clock_new(obj, "cpu-sysclk");
    qdev_init_gpio_in(dev, alpha_cpu_set_irq, ALPHA_CPU_INPUT_COUNT);

    /* Set all exception vectors to an invalid address. */
    for (i = 0; i < EXCP_LAST; i++) {
        env->excp_vectors[i] = (target_ulong)(-1ULL);
    }
}

static void alpha_cpu_finalizefn(Object *obj)
{
    AlphaCPU *cpu = ALPHA_CPU(obj);

    g_hash_table_destroy(cpu->ipregs);
}

static void alpha_cpu_instance_init(Object *obj)
{
    AlphaCPUClass *acc = ALPHA_CPU_GET_CLASS(obj);

    (*acc->info->initfn)(obj);
    alpha_cpu_post_init(obj);
}

static void alpha_cpu_register_class_init(ObjectClass *oc, const void *data)
{
    AlphaCPUClass *acc = ALPHA_CPU_CLASS(oc);

    acc->info = data;
}

static void alpha_cpu_class_init(ObjectClass *oc, const void *data)
{
    AlphaCPUClass *acc = ALPHA_CPU_CLASS(oc);
    CPUClass *cc = CPU_CLASS(acc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    InterruptStatsProviderClass *ispc = INTERRUPT_STATS_PROVIDER_CLASS(oc);

    device_class_set_parent_realize(dc, alpha_cpu_realizefn,
                                    &acc->parent_realize);
    device_class_set_parent_unrealize(dc, alpha_cpu_unrealizefn,
                                      &acc->parent_unrealize);
    resettable_class_set_parent_phases(rc, NULL, alpha_cpu_reset_hold, NULL,
                                       &acc->parent_phases);

    cc->class_by_name = alpha_cpu_class_by_name;

    cc->dump_state = alpha_cpu_dump_state;
    cc->set_pc = alpha_cpu_set_pc;
    cc->get_pc = alpha_cpu_get_pc;
    cc->disas_set_info = alpha_cpu_disas_set_info;
    cc->gdb_read_register = alpha_cpu_gdb_read_register;
    cc->gdb_write_register = alpha_cpu_gdb_write_register;
    cc->gdb_num_core_regs = 67;
    cc->gdb_stop_before_watchpoint = true;
    cc->sysemu_ops = &alpha_sysemu_ops;
    cc->tcg_ops = &alpha_tcg_ops;

    ispc->get_statistics = alpha_get_irq_stats;
}

void alpha_cpu_register(const AlphaCPUInfo *info)
{
    TypeInfo type_info = {
        .parent = TYPE_ALPHA_CPU,
        .instance_init = alpha_cpu_instance_init,
        .class_init = info->class_init ?: alpha_cpu_register_class_init,
        .class_data = (void *)info,
    };

    type_info.name = g_strdup_printf("%s-" TYPE_ALPHA_CPU, info->name);
    type_register_static(&type_info);
    g_free((void *)type_info.name);
}

static const TypeInfo alpha_cpu_type_info = {
    .name = TYPE_ALPHA_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(AlphaCPU),
    .instance_align = __alignof(AlphaCPU),
    .instance_init = alpha_cpu_initfn,
    .instance_finalize = alpha_cpu_finalizefn,
    .abstract = true,
    .class_size = sizeof(AlphaCPUClass),
    .class_init = alpha_cpu_class_init,
    .interfaces = (InterfaceInfo[]) {
          { TYPE_INTERRUPT_STATS_PROVIDER },
          { }
    },
};

static void alpha_cpu_register_types(void)
{
    size_t i;

    type_register_static(&alpha_cpu_type_info);
    for (i = 0; i < ARRAY_SIZE(alpha_cpus); ++i) {
        alpha_cpu_register(&alpha_cpus[i]);
    }
}

type_init(alpha_cpu_register_types)
