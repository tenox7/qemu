/* SPDX-License-Identifier: MIT */
/*
 * QEMU monitor
 */

#include "qemu/osdep.h"
#include "monitor/monitor.h"
#include "monitor/hmp.h"
#include "qobject/qdict.h"
#include "cpu.h"
#include "internals.h"

#ifdef CONFIG_HMP

void hmp_info_iprs(MonitorHMP *hmp, const QDict *qdict)
{
    bool all_cpus = qdict_get_try_bool(qdict, "cpustate_all", false);
    int vcpu = qdict_get_try_int(qdict, "vcpu", -1);
    CPUState *cs;

    if (all_cpus) {
        CPU_FOREACH(cs) {
            monitor_hmp_printf(hmp, "\nCPU#%d\n", cs->cpu_index);
            alpha_cpu_dump_iprs(cs);
        }
    } else {
        cs = vcpu >= 0 ? qemu_get_cpu(vcpu) : monitor_hmp_get_cpu(hmp);

        if (!cs) {
            if (vcpu >= 0) {
                monitor_hmp_printf(hmp, "CPU#%d not available\n", vcpu);
            } else {
                monitor_hmp_printf(hmp, "No CPU available\n");
            }
            return;
        }

        monitor_hmp_printf(hmp, "\nCPU#%d\n", cs->cpu_index);
        alpha_cpu_dump_iprs(cs);
    }
}

void hmp_info_tlb(MonitorHMP *hmp, const QDict *qdict)
{
    CPUState *cs = monitor_hmp_get_cpu(hmp);

    if (!cs) {
        monitor_hmp_printf(hmp, "No CPU available\n");
        return;
    }
    alpha_cpu_dump_mmu(cs);
}

static int64_t monitor_get_reg(MonitorHMP *hmp, const struct MonitorDef *md,
                               int val)
{
    CPUArchState *env = monitor_hmp_get_cpu_env(hmp);
    return alpha_cpu_get_reg_value(env, val);
}

const MonitorDef alpha_monitor_defs[] = {
    { "r0|v0", 0, monitor_get_reg },
    { "r1|t0", 1, monitor_get_reg },
    { "r2|t1", 2, monitor_get_reg },
    { "r3|t2", 3, monitor_get_reg },
    { "r4|t3", 4, monitor_get_reg },
    { "r5|t4", 5, monitor_get_reg },
    { "r6|t5", 6, monitor_get_reg },
    { "r7|t6", 7, monitor_get_reg },
    { "r8|t7", 8, monitor_get_reg },
    { "r9|s0", 9, monitor_get_reg },
    { "r10|s1", 10, monitor_get_reg },
    { "r11|s2", 11, monitor_get_reg },
    { "r12|s3", 12, monitor_get_reg },
    { "r13|s4", 13, monitor_get_reg },
    { "r14|s5", 14, monitor_get_reg },
    { "r15|fp", 15, monitor_get_reg },
    { "r16|a0", 16, monitor_get_reg },
    { "r17|a1", 17, monitor_get_reg },
    { "r18|a2", 18, monitor_get_reg },
    { "r19|a3", 19, monitor_get_reg },
    { "r20|a4", 20, monitor_get_reg },
    { "r21|a5", 21, monitor_get_reg },
    { "r22|t8", 22, monitor_get_reg },
    { "r23|t9", 23, monitor_get_reg },
    { "r24|t10", 24, monitor_get_reg },
    { "r25|t11", 25, monitor_get_reg },
    { "r26|ra", 26, monitor_get_reg },
    { "r27|t12", 27, monitor_get_reg },
    { "r28|at", 28, monitor_get_reg },
    { "r29|gp", 29, monitor_get_reg },
    { "r30|sp", 30, monitor_get_reg },
    { "r31|zero", 31, monitor_get_reg },
    { "pc", offsetof(CPUAlphaState, pc) },
    { "f0", offsetof(CPUAlphaState, fpregs[0]) },
    { "f1", offsetof(CPUAlphaState, fpregs[1]) },
    { "f2", offsetof(CPUAlphaState, fpregs[2]) },
    { "f3", offsetof(CPUAlphaState, fpregs[3]) },
    { "f4", offsetof(CPUAlphaState, fpregs[4]) },
    { "f5", offsetof(CPUAlphaState, fpregs[5]) },
    { "f6", offsetof(CPUAlphaState, fpregs[6]) },
    { "f7", offsetof(CPUAlphaState, fpregs[7]) },
    { "f8", offsetof(CPUAlphaState, fpregs[8]) },
    { "f9", offsetof(CPUAlphaState, fpregs[9]) },
    { "f10", offsetof(CPUAlphaState, fpregs[10]) },
    { "f11", offsetof(CPUAlphaState, fpregs[11]) },
    { "f12", offsetof(CPUAlphaState, fpregs[12]) },
    { "f13", offsetof(CPUAlphaState, fpregs[13]) },
    { "f14", offsetof(CPUAlphaState, fpregs[14]) },
    { "f15", offsetof(CPUAlphaState, fpregs[15]) },
    { "f16", offsetof(CPUAlphaState, fpregs[16]) },
    { "f17", offsetof(CPUAlphaState, fpregs[17]) },
    { "f18", offsetof(CPUAlphaState, fpregs[18]) },
    { "f19", offsetof(CPUAlphaState, fpregs[19]) },
    { "f20", offsetof(CPUAlphaState, fpregs[20]) },
    { "f21", offsetof(CPUAlphaState, fpregs[21]) },
    { "f21", offsetof(CPUAlphaState, fpregs[22]) },
    { "f22", offsetof(CPUAlphaState, fpregs[23]) },
    { "f23", offsetof(CPUAlphaState, fpregs[24]) },
    { "f24", offsetof(CPUAlphaState, fpregs[25]) },
    { "f25", offsetof(CPUAlphaState, fpregs[26]) },
    { "f26", offsetof(CPUAlphaState, fpregs[27]) },
    { "f27", offsetof(CPUAlphaState, fpregs[28]) },
    { "f29", offsetof(CPUAlphaState, fpregs[29]) },
    { "f30", offsetof(CPUAlphaState, fpregs[30]) },
    { "f31", offsetof(CPUAlphaState, fpregs[31]) },
    { NULL },
};

#endif /* CONFIG_HMP */
