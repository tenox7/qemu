/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2003-2017, Robert M Supnik

   Except as contained in this notice, the name of Robert M Supnik shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   Respectfully dedicated to the great people of the Alpha chip, systems, and
   software development projects; and to the memory of Peter Conklin, of the
   Alpha Program Office.  */

#include "qemu/osdep.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat-macros.h"
#include "vax_helper.h"
#include "cpu.h"
#include "internals.h"

/*
 * This entire file needs to be rewritten. The current implementation is only
 * a stopgap to get VMS working.
 */

#define Q_SIGN          BIT_ULL(63)
#define Q_GETSIGN(x)    (((x) >> 63) & 1)
#define NEG_Q(x)        ((~(x) + 1))

typedef struct UnpackedFloat {
    bool sign;
    int32_t exp;
    uint64_t frac;
} UnpackedFloat;

static uint64_t uemul64(uint64_t a, uint64_t b, uint64_t *hi)
{
    uint64_t lo;

    mulu64(&lo, hi, a, b);
    return lo;
}

static uint64_t ufdiv64(uint64_t dvd, uint64_t dvr, uint32_t prec)
{
    uint64_t quo;
    uint32_t i;

    quo = 0;                                        /* clear quotient */
    for (i = 0; (i < prec) && dvd; i++) {           /* divide loop */
        quo = quo << 1;                             /* shift quotient */
        if (dvd >= dvr) {                           /* div step ok? */
            dvd = dvd - dvr;                        /* subtract */
            quo = quo + 1;                          /* quo bit = 1 */
        }
        dvd = dvd << 1;                             /* shift divd */
    }
    quo = quo << (UF_V_NM - i + 1);                 /* shift quotient */
    return quo;                                     /* return quotient */
}

static uint64_t fsqrt64(uint64_t asig, int32_t exp)
{
    uint64_t zsig, remh, reml, t;
    uint32_t sticky = 0;

    zsig = estimateSqrt32(exp, (uint32_t)(asig >> 32));

    /* Calculate the final answer in two steps.  First, do one iteration of
       Newton's approximation.  The divide-by-2 is accomplished by clever
       positioning of the operands.  Then, check the bits just below the
       (double precision) rounding bit to see if they are close to zero
       (that is, the rounding bits are close to midpoint).  If so, make
       sure that the result^2 is <below> the input operand.  */

    asig = asig >> ((exp & 1) ? 3 : 2);             /* leave 2b guard */
    zsig = ufdiv64(asig, zsig << 32, 64) + (zsig << 30);
    if ((zsig & 0x1FF) <= 5) {                      /* close to even? */
        reml = uemul64(zsig, zsig, &remh);          /* result^2 */
        remh = asig - remh - (reml ? 1 : 0);        /* arg - result^2 */
        reml = NEG_Q(reml);
        while (Q_GETSIGN(remh) != 0) {              /* if arg < result^2 */
            zsig = zsig - 1;                        /* decr result */
            t = (zsig << 1) | 1;                    /* incr result^2 */
            reml = reml + t;                        /* and retest */
            remh = remh + (zsig >> 63) + ((reml < t) ? 1 : 0);
        }
        if ((remh | reml) != 0) {
            sticky = 1;                             /* not exact? */
        }
    }
    zsig = (zsig << 1) | sticky;                    /* left justify result */
    return zsig;
}

static void vax_unpack(uint64_t val, UnpackedFloat *raw, float_status *fpst)
{
    raw->sign = extract64(val, FPR_V_SIGN, 1);
    raw->exp = extract64(val, FPR_V_EXP, 11);
    raw->frac = extract64(val, 0, FPR_V_EXP);

    if (raw->exp == 0) {
        if (unlikely(raw->sign)) {
            float_raise(float_flag_invalid, fpst);
        }
        raw->frac = 0;
        raw->sign = 0;
    } else {
        raw->frac = (raw->frac | FPR_HB) << FPR_GUARD;
    }
}

static void vax_unpack_d(uint64_t val, UnpackedFloat *raw, float_status *fpst)
{
    raw->sign = extract64(val, FDR_V_SIGN, 1);
    raw->exp = extract64(val, FDR_V_EXP, 8);
    raw->frac = extract64(val, 0, FDR_V_EXP);

    if (raw->exp == 0) {
        if (unlikely(raw->sign)) {
            float_raise(float_flag_invalid, fpst);
        }
        raw->frac = 0;
        raw->sign = 0;
    } else {
        raw->exp = raw->exp + G_BIAS - D_BIAS;
        raw->frac = (raw->frac | FDR_HB) << FDR_GUARD;
    }
}

static void vax_normalize(UnpackedFloat *raw)
{
    static uint64_t norm_mask[5] = { 0xc000000000000000,
                                     0xf000000000000000,
                                     0xff00000000000000,
                                     0xffff000000000000,
                                     0xffffffff00000000 };
    static int32_t norm_tab[6] = { 1, 2, 4, 8, 16, 32 };
    int i;

    if (raw->frac == 0) {                           /* if fraction = 0 */
        raw->sign = 0;
        raw->exp = 0;                               /* result is 0 */
    } else {
        while ((raw->frac & UF_NM) == 0) {          /* normalized? */
            for (i = 0; i < 5; i++) {               /* find first 1 */
                if (raw->frac & norm_mask[i]) {
                    break;
                }
            }
            raw->frac = raw->frac << norm_tab[i];   /* shift frac */
            raw->exp = raw->exp - norm_tab[i];      /* decr exp */
        }
    }
}

static uint64_t vax_rpack(UnpackedFloat *r, float_status *fpst, bool dp)
{
    static const uint64_t round_bit[2] = { UF_FRND, UF_GRND };
    static const int32_t exp_max[2] = { G_BIAS - F_BIAS + F_M_EXP, G_M_EXP };
    static const int32_t exp_min[2] = { G_BIAS - F_BIAS, 0 };

    if (unlikely(r->frac == 0)) {
        return 0;                                   /* result 0? */
    }
    if (get_float_rounding_mode(fpst) != float_round_to_zero) {
        r->frac = (r->frac + round_bit[dp]);        /* add round bit */
        if ((r->frac & UF_NM) == 0) {               /* carry out? */
            r->frac = (r->frac >> 1) | UF_NM;       /* renormalize */
            r->exp = r->exp + 1;
        }
    }
    if (unlikely(r->exp > exp_max[dp])) {           /* overflow? */
        float_raise(float_flag_overflow, fpst);
        r->exp = exp_max[dp];                       /* return max */
    }
    if (unlikely(r->exp <= exp_min[dp])) {          /* underflow? */
        float_raise(float_flag_underflow, fpst);
        return 0;
    }

    return (((uint64_t)r->sign) << FPR_V_SIGN) |
           (((uint64_t)r->exp) << FPR_V_EXP) |
           (((uint64_t)r->frac >> FPR_GUARD) & FPR_FRAC);
}

static uint64_t vax_rpack_d(UnpackedFloat *r, float_status *fpst)
{
    if (r->frac == 0) {
        return 0;
    }
    r->exp = r->exp + D_BIAS - G_BIAS;              /* rebias */
    if (unlikely(r->exp > FDR_M_EXP)) {             /* overflow? */
        float_raise(float_flag_overflow, fpst);
        r->exp = FDR_M_EXP;                         /* return max */
    }
    if (unlikely(r->exp <= 0)) {                    /* underflow? */
        float_raise(float_flag_underflow, fpst);
        return 0;                                   /* underflow to 0 */
    }

    return (((uint64_t)r->sign) << FDR_V_SIGN) |
           (((uint64_t)r->exp) << FDR_V_EXP) |
           (((uint64_t)r->frac >> FDR_GUARD) & FDR_FRAC);
}

static uint64_t vax_cvtif(uint64_t val, float_status *fpst, bool dp)
{
    UnpackedFloat ufp;

    if (unlikely(val == 0)) {
        return 0;                                   /* 0? return +0 */
    }
    if (val & Q_SIGN) {                             /* < 0? */
        ufp.sign = 1;                               /* set sign */
        val = NEG_Q(val);                           /* |val| */
    } else {
        ufp.sign = 0;
    }

    ufp.exp = 64 + G_BIAS;                          /* set exp */
    ufp.frac = val;                                 /* set frac */
    vax_normalize(&ufp);                            /* normalize */
    return vax_rpack(&ufp, fpst, dp);               /* round and pack */
}

static uint64_t vax_cvtfi(uint64_t op, float_status *fpst)
{
    UnpackedFloat a;
    int32_t ubexp;

    vax_unpack(op, &a, fpst);
    ubexp = a.exp - G_BIAS;                         /* unbiased exp */
    if (unlikely(ubexp < 0)) {
        return 0;                                   /* zero or too small? */
    }

    if (ubexp <= UF_V_NM) {                         /* in range? */
        a.frac = a.frac >> (UF_V_NM - ubexp);       /* leave rnd bit */
        if (get_float_rounding_mode(fpst) != float_round_to_zero) {
            a.frac = a.frac + 1;                    /* not chopped, round */
        }
        a.frac = a.frac >> 1;                       /* now justified */
        if (unlikely(a.frac > (a.sign ? INT64_MIN : INT64_MAX))) {
            float_raise(float_flag_invalid_cvti, fpst);
        }
    } else {
        if (ubexp > (UF_V_NM + 64)) {
            a.frac = 0;                             /* out of range */
        } else {
            a.frac = (a.frac << (ubexp - UF_V_NM - 1));
        }
        float_raise(float_flag_invalid_cvti, fpst);
    }
    return (a.sign ? NEG_Q(a.frac) : a.frac);
}

static int vax_fcmp(uint64_t s1, uint64_t s2, float_status *fpst)
{
    UnpackedFloat a, b;

    vax_unpack(s1, &a, fpst);
    vax_unpack(s2, &b, fpst);
    if (s1 == s2) {
        return 0;                                   /* equal? */
    }
    if (a.sign != b.sign) {
        return (a.sign ? -1 : +1);                  /* opp signs? */
    }
    return (((s1 < s2) ^ a.sign) ? -1 : +1);        /* like signs */
}

static int vax_fcmpeq(uint64_t s1, uint64_t s2, float_status *fpst)
{
    return vax_fcmp(s1, s2, fpst) == 0;
}

static int vax_fcmplt(uint64_t s1, uint64_t s2, float_status *fpst)
{
    return vax_fcmp(s1, s2, fpst) < 0;
}

static int vax_fcmple(uint64_t s1, uint64_t s2, float_status *fpst)
{
    return vax_fcmp(s1, s2, fpst) <= 0;
}

static uint64_t vax_fadd_common(uint64_t s1, uint64_t s2, float_status *fpst,
                                bool dp, bool sub)
{
    UnpackedFloat a, b, t;
    uint32_t sticky;
    int32_t ediff;

    vax_unpack(s1, &a, fpst);
    vax_unpack(s2, &b, fpst);
    if (sub) {
        b.sign = b.sign ^ 1; /* sub? invert b sign */
    }
    if (a.exp == 0) {
        a = b;                                      /* s1 = 0? */
    } else if (b.exp) {
        if ((a.exp < b.exp) ||
            ((a.exp == b.exp) && (a.frac < b.frac))) {
            t = a;
            a = b;
            b = t;
        }

        ediff = a.exp - b.exp; /* exp diff */
        if (a.sign ^ b.sign) {
            if (ediff > 63) {
                b.frac = 1;                         /* >63? retain sticky */
            } else if (ediff) {
                sticky = ((b.frac << (64 - ediff))) ? 1 : 0; /* lost bits */
                b.frac = (b.frac >> ediff) | sticky;
            }
            a.frac = (a.frac - b.frac);             /* subtract fractions */
            vax_normalize(&a);                      /* normalize */
        } else {
            if (ediff > 63) {
                b.frac = 0;                         /* >63? b disappears */
            } else if (ediff) {
                b.frac = b.frac >> ediff;           /* denormalize */
            }
            a.frac = (a.frac + b.frac);             /* add frac */
            if (a.frac < b.frac) {                  /* chk for carry */
                a.frac = UF_NM | (a.frac >> 1);     /* shift in carry */
                a.exp = a.exp + 1;                  /* skip norm */
            }
        }
    }

    return vax_rpack(&a, fpst, dp);                 /* round and pack */
}

static uint64_t vax_fadd(uint64_t s1, uint64_t s2, float_status *fpst, bool dp)
{
    return vax_fadd_common(s1, s2, fpst, dp, false);
}

static uint64_t vax_fsub(uint64_t s1, uint64_t s2, float_status *fpst, bool dp)
{
    return vax_fadd_common(s1, s2, fpst, dp, true);
}

static uint64_t vax_fmul(uint64_t s1, uint64_t s2, float_status *fpst,
                         bool dp)
{
    UnpackedFloat a, b;

    vax_unpack(s1, &a, fpst);
    vax_unpack(s2, &b, fpst);
    if ((a.exp == 0) || (b.exp == 0)) {
        return 0;                                   /* zero argument? */
    }
    a.sign = a.sign ^ b.sign;                       /* sign of result */
    a.exp = a.exp + b.exp - G_BIAS;                 /* add exponents */
    uemul64(a.frac, b.frac, &a.frac);               /* multiply fractions */
    vax_normalize(&a);                              /* normalize */
    return vax_rpack(&a, fpst, dp);                 /* round and pack */
}

static uint64_t vax_fdiv(uint64_t s1, uint64_t s2, float_status *fpst,
                         bool dp)
{
    UnpackedFloat a, b;

    vax_unpack(s1, &a, fpst);
    vax_unpack(s2, &b, fpst);
    if (unlikely(b.exp == 0)) {                     /* divr = 0? */
        float_raise(float_flag_divbyzero, fpst);
        return 0;
    }
    if (a.exp == 0) {
        return 0;                                   /* divd = 0? */
    }
    a.sign = a.sign ^ b.sign;                       /* result sign */
    a.exp = a.exp - b.exp + G_BIAS + 1;             /* unbiased exp */
    a.frac = a.frac >> 1;                           /* allow 1 bit left */
    b.frac = b.frac >> 1;
    a.frac = ufdiv64(a.frac, b.frac, 55);           /* divide */
    vax_normalize(&a);                              /* normalize */
    return vax_rpack(&a, fpst, dp);                 /* round and pack */
}

static uint64_t vax_sqrt(uint64_t op, float_status *fpst, bool dp)
{
    UnpackedFloat b;

    vax_unpack(op, &b, fpst);
    if (b.exp == 0) {
        return 0;                                   /* zero? */
    }
    if (unlikely(b.sign)) {                         /* negative? */
        float_raise(float_flag_invalid, fpst);
        return 0;
    }
    b.exp = ((b.exp + 1 - G_BIAS) >> 1) + G_BIAS;   /* result exponent */
    b.frac = fsqrt64(b.frac, b.exp);                /* result fraction */
    return vax_rpack(&b, fpst, dp);                 /* round and pack */
}

uint64_t HELPER(vax_ldf)(uint32_t val)
{
    return vax_single_to_register(val);
}

uint32_t HELPER(vax_stf)(uint64_t val)
{
    return vax_register_to_single(val);
}

uint64_t HELPER(vax_ldg)(uint64_t val)
{
    return vax_double_to_register(val);
}

uint64_t HELPER(vax_stg)(uint64_t val)
{
    return vax_register_to_double(val);
}

uint64_t HELPER(vax_cvtqf)(CPUAlphaState *env, uint64_t val)
{
    float_status *fpst = &env->fp_status;

    alpha_env_reset_float_exceptions(env);
    return vax_cvtif(val, fpst, false);
}

uint64_t HELPER(vax_cvtgf)(CPUAlphaState *env, uint64_t val)
{
    float_status *fpst = &env->fp_status;
    UnpackedFloat ufp;

    alpha_env_reset_float_exceptions(env);
    vax_unpack(val, &ufp, fpst);
    return vax_rpack(&ufp, fpst, false);
}

uint64_t HELPER(vax_cvtqg)(CPUAlphaState *env, uint64_t val)
{
    float_status *fpst = &env->fp_status;

    alpha_env_reset_float_exceptions(env);
    return vax_cvtif(val, fpst, true);
}

uint64_t HELPER(vax_cvtgq)(CPUAlphaState *env, uint64_t val)
{
    float_status *fpst = &env->fp_status;

    alpha_env_reset_float_exceptions(env);
    return vax_cvtfi(val, fpst);
}

uint64_t HELPER(vax_cvtdg)(CPUAlphaState *env, uint64_t val)
{
    float_status *fpst = &env->fp_status;
    UnpackedFloat ufp;

    alpha_env_reset_float_exceptions(env);
    vax_unpack_d(val, &ufp, fpst);
    return vax_rpack(&ufp, fpst, true);
}

uint64_t HELPER(vax_cvtgd)(CPUAlphaState *env, uint64_t val)
{
    float_status *fpst = &env->fp_status;
    UnpackedFloat ufp;

    alpha_env_reset_float_exceptions(env);
    vax_unpack(val, &ufp, fpst);
    return vax_rpack_d(&ufp, fpst);
}

#define GEN_HELPER_FCMP(NAME, CMPFN)                        \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint64_t b)                           \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
    float64 ret;                                            \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    if (CMPFN(a, b, fpst)) {                                \
        ret = float64_two;                                  \
    } else {                                                \
        ret = 0;                                            \
    }                                                       \
    return ret;                                             \
}

GEN_HELPER_FCMP(vax_cmpgeq, vax_fcmpeq)
GEN_HELPER_FCMP(vax_cmpglt, vax_fcmplt)
GEN_HELPER_FCMP(vax_cmpgle, vax_fcmple)

#define GEN_HELPER_BINOP_F(NAME, FN)                        \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint64_t b)                           \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    return FN(a, b, fpst, false);                           \
}

#define GEN_HELPER_UNOP_F(NAME, FN)                         \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a)       \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    return FN(a, fpst, false);                              \
}

GEN_HELPER_BINOP_F(vax_addf, vax_fadd)
GEN_HELPER_BINOP_F(vax_subf, vax_fsub)
GEN_HELPER_BINOP_F(vax_mulf, vax_fmul)
GEN_HELPER_BINOP_F(vax_divf, vax_fdiv)
GEN_HELPER_UNOP_F(vax_sqrtf, vax_sqrt)

#define GEN_HELPER_BINOP_G(NAME, FN)                        \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint64_t b)                           \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    return FN(a, b, fpst, true);                            \
}

#define GEN_HELPER_UNOP_G(NAME, FN)                         \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a)       \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    return FN(a, fpst, true);                               \
}

GEN_HELPER_BINOP_G(vax_addg, vax_fadd)
GEN_HELPER_BINOP_G(vax_subg, vax_fsub)
GEN_HELPER_BINOP_G(vax_mulg, vax_fmul)
GEN_HELPER_BINOP_G(vax_divg, vax_fdiv)
GEN_HELPER_UNOP_G(vax_sqrtg, vax_sqrt)
