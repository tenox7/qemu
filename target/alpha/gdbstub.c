/* SPDX-License-Identifier: LGPL-2.1+ */
/*
 * QEMU Alpha CPU -- gdb server stub
 */

#include "qemu/osdep.h"
#include "exec/gdbstub.h"
#include "gdbstub/helpers.h"
#include "gdbstub/commands.h"
#include "cpu.h"
#include "instmap.h"
#include "internals.h"

static inline uint64_t *cpu_gpr_shadow_ptr(CPUAlphaState *env, int reg,
                                           const int *map)
{
    size_t i;

    assert(reg < ARRAY_SIZE(env->gpregs));
    for (i = 0; i < 8; i++) {
        if (map[i] == reg) {
            return cpu_shadow_ptr(env, i);
        }
    }
    return cpu_gpr_ptr(env, reg);
}

static inline uint64_t *reg_ptr(CPUAlphaState *env, int reg)
{
    static const int ev6_shadow_map[8] = { IR_T3, IR_T4, IR_T5, IR_T6,
                                           IR_A4, IR_A5, IR_T8, IR_T9 };
    static const int ev5_shadow_map[8] = { IR_T7, IR_S0, IR_S1, IR_S2,
                                           IR_S3, IR_S4, IR_S5, IR_T11 };

    bool shadow = alpha_palshadow_enabled(env) && alpha_is_pal(env);

    switch (env->implver) {
    case IMPLVER_2106x:
        return cpu_gpr_ptr(env, reg);
    case IMPLVER_21164:
        return shadow ? cpu_gpr_shadow_ptr(env, reg, ev5_shadow_map)
                      : cpu_gpr_ptr(env, reg);
    case IMPLVER_21264:
        return shadow ? cpu_gpr_shadow_ptr(env, reg, ev6_shadow_map)
                      : cpu_gpr_ptr(env, reg);
    default:
        g_assert_not_reached();
    }
}

uint64_t alpha_cpu_get_reg_value(CPUAlphaState *env, int ra)
{
    return *(reg_ptr(env, ra));
}

void alpha_cpu_set_reg_value(CPUAlphaState *env, int ra, uint64_t val)
{
    *(reg_ptr(env, ra)) = val;
}

int alpha_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    AlphaCPU *cpu = ALPHA_CPU(cs);
    CPUAlphaState *env = &cpu->env;
    uint64_t val;

    switch (n) {
    case 0 ... 31:
        val = alpha_cpu_get_reg_value(env, n);
        break;
    case 32 ... 62:
        val = env->fpregs[n - 32];
        break;
    case 63:
        val = alpha_cpu_get_fpcr(env);
        break;
    case 64:
        val = alpha_cpu_get_pc(cs);
        break;
    case 65:
    case 66:
        /* Unassigned but required. */
        val = 0;
        break;
    default:
        return 0;
    }
    return gdb_get_reg64(mem_buf, val);
}

int alpha_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    AlphaCPU *cpu = ALPHA_CPU(cs);
    CPUAlphaState *env = &cpu->env;
    uint64_t tmp;

    tmp = ldq_le_p(mem_buf);
    switch (n) {
    case 0 ... 30:
        alpha_cpu_set_reg_value(env, n, tmp);
        break;
    case 31:
        /* Zero register. */
        break;
    case 32 ... 62:
        env->fpregs[n - 32] = tmp;
        break;
    case 63:
        alpha_cpu_set_fpcr(env, tmp);
        break;
    case 64:
        alpha_cpu_set_pc(cs, tmp | alpha_is_pal(env));
        break;
    case 65:
    case 66:
        /* Unassigned but required. */
        break;
    default:
        return 0;
    }
    return 8;
}
