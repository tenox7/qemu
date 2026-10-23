/*
 * PowerPC 601 POWER bridge instructions
 *
 * The 601 executes most of the original POWER instruction set, many of them
 * using the MQ register as an extension of the general purpose registers.
 * Semantics follow the AIX Version 3 Assembler Language Reference.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"

#define PWR_RT(op)  (((op) >> 21) & 0x1f)
#define PWR_RA(op)  (((op) >> 16) & 0x1f)
#define PWR_RB(op)  (((op) >> 11) & 0x1f)
#define PWR_SH(op)  (((op) >> 11) & 0x1f)
#define PWR_MB(op)  (((op) >> 6) & 0x1f)
#define PWR_ME(op)  (((op) >> 1) & 0x1f)
#define PWR_XO9(op) (((op) >> 1) & 0x1ff)
#define PWR_XO(op)  (((op) >> 1) & 0x3ff)
#define PWR_OE(op)  (((op) >> 10) & 1)
#define PWR_RC(op)  ((op) & 1)

static uint32_t rotl32_(uint32_t v, int n)
{
    return n ? (v << n) | (v >> (32 - n)) : v;
}

/* IBM bit numbering: ones from bit mb to bit me, wrapping around */
static uint32_t mask32(int mb, int me)
{
    uint32_t hi = 0xffffffffu >> mb, lo = 0xffffffffu << (31 - me);

    return mb <= me ? hi & lo : hi | lo;
}

static void set_cr0(CPUPPCState *env, uint32_t v)
{
    int32_t s = v;

    env->crf[0] = (s < 0 ? 8 : s > 0 ? 4 : 2) | env->so;
}

static void set_ov(CPUPPCState *env, bool ov)
{
    env->ov = ov;
    env->so |= ov;
}

static void pwr_div(CPUPPCState *env, uint32_t op, int64_t dividend)
{
    int32_t divisor = env->gpr[PWR_RB(op)];
    bool ov = divisor == 0 || (dividend == INT64_MIN && divisor == -1);
    int64_t q = 0, r = 0;

    if (!ov) {
        q = dividend / divisor;
        r = dividend % divisor;
        ov = q > INT32_MAX || q < INT32_MIN;
    }
    if (ov) {
        q = INT32_MIN;
        r = 0;
    }
    env->gpr[PWR_RT(op)] = (uint32_t)q;
    env->spr[SPR_MQ] = (uint32_t)r;
    if (PWR_OE(op)) {
        set_ov(env, ov);
    }
    if (PWR_RC(op)) {
        set_cr0(env, r);
    }
}

static void pwr_lscbx(CPUPPCState *env, uint32_t op, uintptr_t ra)
{
    uint8_t buf[128];
    target_ulong ea = (PWR_RA(op) ? env->gpr[PWR_RA(op)] : 0) +
                      env->gpr[PWR_RB(op)];
    uint32_t xer = cpu_read_xer(env);
    int n = xer & 0x7f, cmp = (xer >> 8) & 0xff, i, reg;
    bool match = false;

    for (i = 0; i < n && !match; i++) {
        buf[i] = cpu_ldub_data_ra(env, ea + i, ra);
        match = buf[i] == cmp;
    }
    n = i;
    for (i = 0, reg = PWR_RT(op); i < n; i += 4, reg = (reg + 1) & 31) {
        uint32_t v = env->gpr[reg];
        int b;

        for (b = 0; b < 4 && i + b < n; b++) {
            v = (v & ~(0xff000000u >> (b * 8))) |
                ((uint32_t)buf[i + b] << (24 - b * 8));
        }
        if ((PWR_RA(op) && reg == PWR_RA(op)) || reg == PWR_RB(op)) {
            continue;
        }
        env->gpr[reg] = v;
    }
    cpu_write_xer(env, (xer & ~0x7fu) | n);
    if (PWR_RC(op)) {
        env->crf[0] = (match ? 2 : 0) | env->so;
    }
}

void helper_power(CPUPPCState *env, uint32_t op)
{
    uint32_t rs, rb, mq, r, m, res = 0;
    int n, rt = PWR_RT(op), ra = PWR_RA(op);
    bool rc = PWR_RC(op), wr_mq = true;

    rs = env->gpr[rt];
    rb = env->gpr[PWR_RB(op)];
    mq = env->spr[SPR_MQ];
    n = rb & 0x1f;

    if ((op >> 26) == 9) {                      /* dozi */
        int32_t a = env->gpr[ra], si = (int16_t)op;

        env->gpr[rt] = a > si ? 0 : (uint32_t)(si - a);
        return;
    }
    if ((op >> 26) == 22) {                     /* rlmi */
        m = mask32(PWR_MB(op), PWR_ME(op));
        res = (rotl32_(rs, n) & m) | (env->gpr[ra] & ~m);
        env->gpr[ra] = res;
        if (rc) {
            set_cr0(env, res);
        }
        return;
    }

    switch (PWR_XO9(op)) {
    case 360: {                                 /* abs */
        int32_t a = env->gpr[ra];

        res = a < 0 ? -(uint32_t)a : (uint32_t)a;
        env->gpr[rt] = res;
        if (PWR_OE(op)) {
            set_ov(env, (uint32_t)a == 0x80000000u);
        }
        if (rc) {
            set_cr0(env, res);
        }
        return;
    }
    case 488: {                                 /* nabs */
        int32_t a = env->gpr[ra];

        res = a > 0 ? -(uint32_t)a : (uint32_t)a;
        env->gpr[rt] = res;
        if (PWR_OE(op)) {
            env->ov = 0;
        }
        if (rc) {
            set_cr0(env, res);
        }
        return;
    }
    case 264: {                                 /* doz */
        int32_t a = env->gpr[ra], b = rb;
        int64_t d = (int64_t)b - a;

        res = a > b ? 0 : (uint32_t)d;
        env->gpr[rt] = res;
        if (PWR_OE(op)) {
            set_ov(env, a <= b && (d > INT32_MAX || d < INT32_MIN));
        }
        if (rc) {
            set_cr0(env, res);
        }
        return;
    }
    case 107: {                                 /* mul */
        int64_t p = (int64_t)(int32_t)env->gpr[ra] * (int32_t)rb;

        env->gpr[rt] = (uint32_t)(p >> 32);
        env->spr[SPR_MQ] = (uint32_t)p;
        if (PWR_OE(op)) {
            set_ov(env, p != (int32_t)p);
        }
        if (rc) {
            set_cr0(env, (uint32_t)p);
        }
        return;
    }
    case 331:                                   /* div */
        pwr_div(env, op, (int64_t)(((uint64_t)env->gpr[ra] << 32) | mq));
        return;
    case 363:                                   /* divs */
        pwr_div(env, op, (int32_t)env->gpr[ra]);
        return;
    }

    /* X-form logical and shift instructions: RA is the target, RT is RS */
    switch (PWR_XO(op)) {
    case 29:                                    /* maskg */
        res = mask32(rs & 0x1f, n);
        wr_mq = false;
        break;
    case 541:                                   /* maskir */
        res = (rs & rb) | (env->gpr[ra] & ~rb);
        wr_mq = false;
        break;
    case 537:                                   /* rrib */
        m = 0x80000000u >> n;
        res = (env->gpr[ra] & ~m) | ((rs & 0x80000000u) >> n);
        wr_mq = false;
        break;
    case 153:                                   /* sle */
        r = rotl32_(rs, n);
        res = r & mask32(0, 31 - n);
        mq = r;
        break;
    case 217:                                   /* sleq */
        r = rotl32_(rs, n);
        m = mask32(0, 31 - n);
        res = (r & m) | (mq & ~m);
        mq = r;
        break;
    case 184:                                   /* sliq */
        r = rotl32_(rs, PWR_SH(op));
        res = r & mask32(0, 31 - PWR_SH(op));
        mq = r;
        break;
    case 248:                                   /* slliq */
        r = rotl32_(rs, PWR_SH(op));
        m = mask32(0, 31 - PWR_SH(op));
        res = (r & m) | (mq & ~m);
        mq = r;
        break;
    case 216:                                   /* sllq */
        m = mask32(0, 31 - n);
        res = (rb & 0x20) ? mq & m : (rotl32_(rs, n) & m) | (mq & ~m);
        wr_mq = false;
        break;
    case 152:                                   /* slq */
        r = rotl32_(rs, n);
        res = (rb & 0x20) ? 0 : r & mask32(0, 31 - n);
        mq = r;
        break;
    case 696:                                   /* sriq */
        r = rotl32_(rs, (32 - PWR_SH(op)) & 31);
        res = r & mask32(PWR_SH(op), 31);
        mq = r;
        break;
    case 760:                                   /* srliq */
        r = rotl32_(rs, (32 - PWR_SH(op)) & 31);
        m = mask32(PWR_SH(op), 31);
        res = (r & m) | (mq & ~m);
        mq = r;
        break;
    case 728:                                   /* srlq */
        m = mask32(n, 31);
        res = (rb & 0x20) ? mq & m :
              (rotl32_(rs, (32 - n) & 31) & m) | (mq & ~m);
        wr_mq = false;
        break;
    case 664:                                   /* srq */
        r = rotl32_(rs, (32 - n) & 31);
        res = (rb & 0x20) ? 0 : r & mask32(n, 31);
        mq = r;
        break;
    case 665:                                   /* sre */
        r = rotl32_(rs, (32 - n) & 31);
        res = r & mask32(n, 31);
        mq = r;
        break;
    case 729:                                   /* sreq */
        r = rotl32_(rs, (32 - n) & 31);
        m = mask32(n, 31);
        res = (r & m) | (mq & ~m);
        mq = r;
        break;
    case 921:                                   /* srea */
    case 920:                                   /* sraq */
    case 952: {                                 /* sraiq */
        int sh = PWR_XO(op) == 952 ? PWR_SH(op) : n;
        uint32_t sign = (rs & 0x80000000u) ? 0xffffffffu : 0;

        r = rotl32_(rs, (32 - sh) & 31);
        m = (PWR_XO(op) == 920 && (rb & 0x20)) ? 0 : mask32(sh, 31);
        res = (r & m) | (sign & ~m);
        env->ca = sign && (r & ~m);
        env->ca32 = env->ca;
        mq = r;
        break;
    }
    case 277:                                   /* lscbx */
        pwr_lscbx(env, op, GETPC());
        return;
    default:
        g_assert_not_reached();
    }

    env->gpr[ra] = res;
    if (wr_mq) {
        env->spr[SPR_MQ] = mq;
    }
    if (rc) {
        set_cr0(env, res);
    }
}

target_ulong helper_clcs(CPUPPCState *env, uint32_t arg)
{
    switch (arg) {
    case 0x0c:
        return env->icache_line_size;
    case 0x0d:
        return env->dcache_line_size;
    case 0x0e:
        return MIN(env->icache_line_size, env->dcache_line_size);
    case 0x0f:
        return MAX(env->icache_line_size, env->dcache_line_size);
    default:
        return 0;
    }
}
