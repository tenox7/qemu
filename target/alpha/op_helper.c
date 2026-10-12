/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Alpha emulation helpers for QEMU.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"

#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "hw/core/irq.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "fpu/softfloat.h"
#include "cpu.h"
#include "ipregs.h"
#include "internals.h"

void do_raise_exception(CPUAlphaState *env, int excp, uint32_t syndrome)
{
    CPUState *cs = env_cpu(env);

    assert(!excp_is_internal(excp));
    cs->exception_index = excp;
    env->exception.syndrome = syndrome;
    cpu_loop_exit(cs);
}

void do_raise_exception_ra(CPUAlphaState *env, int excp, uint32_t syndrome,
                           uintptr_t ra)
{
    CPUState *cs = env_cpu(env);

    cpu_restore_state(cs, ra);
    do_raise_exception(env, excp, syndrome);
}

const void *HELPER(lookup_ip_reg)(CPUAlphaState *env, uint32_t iprn)
{
    AlphaCPU *cpu = env_archcpu(env);
    const AlphaIPRegInfo *ri = get_alpha_ipreg_info(cpu->ipregs, iprn);

    assert(ri != NULL);
    return ri;
}

uint64_t HELPER(get_ip_reg)(CPUAlphaState *env, const void *rip)
{
    const AlphaIPRegInfo *ri = rip;
    uint64_t res;

    if (ri->type & ALPHA_IP_IO) {
        BQL_LOCK_GUARD();
        res = (*ri->readfn)(env, ri);
    } else {
        res = (*ri->readfn)(env, ri);
    }
    return res;
}

void HELPER(set_ip_reg)(CPUAlphaState *env, const void *rip, uint64_t value)
{
    const AlphaIPRegInfo *ri = rip;

    if (ri->type & ALPHA_IP_IO) {
        BQL_LOCK_GUARD();
        (*ri->writefn)(env, ri, value);
    } else {
        (*ri->writefn)(env, ri, value);
    }
}

void HELPER(unaligned_access)(CPUAlphaState *env, uint64_t addr,
                              uint32_t access_type, uint32_t mmu_idx)
{
    alpha_cpu_do_unaligned_access(env_cpu(env), addr, access_type,
                                  mmu_idx, GETPC());
}

G_NORETURN void HELPER(raise_exception)(CPUAlphaState *env,
                                        uint32_t excp, uint32_t syndrome)
{
    do_raise_exception_ra(env, excp, syndrome, GETPC());
}

void HELPER(rebuild_hflags)(CPUAlphaState *env)
{
    alpha_rebuild_hflags(env);
}

uint64_t HELPER(rpcc)(CPUAlphaState *env)
{
    return alpha_read_cyclecounter(env);
}

/*
 * Raise an internal-to-QEMU exception. This is limited to only
 * those EXCP values which are special cases for QEMU to interrupt
 * execution and not to be used for exceptions which are passed to
 * the guest.
 */
G_NORETURN void HELPER(raise_exception_internal)(CPUAlphaState *env,
                                                 uint32_t excp,
                                                 uint32_t syndrome)
{
    CPUState *cs = env_cpu(env);

    assert(excp_is_internal(excp));
    cs->exception_index = excp;
    env->exception.syndrome = syndrome;
    cpu_loop_exit(cs);
}
