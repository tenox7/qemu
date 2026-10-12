/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Helpers for floating point instructions.
 *
 * Fixes applied:
 * 1. Added check_float_exceptions to all arithmetic operation macros
 * 2. Fixed inverted status flag condition in check_float_exceptions
 * 3. Removed incorrect infinity->invalid raise in ieee_input (IEEE mode)
 * 4. Fixed flush_to_zero to only require UNDZ, not UNFD+UNDZ
 * 5. Added check_float_exceptions to ieee_cvtst and ieee_cvtts
 */

#include "qemu/osdep.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/helper-proto.h"
#include "fpu_helper.h"
#include "cpu.h"
#include "internals.h"
#include "instmap.h"
#include "syndrome.h"

static inline int alpha_fpcr_dyn_rounding_mode(uint32_t fpcr)
{
    static const FloatRoundMode rounding_mode_map[] = {
        [FPCR_DYN_RM_CHOPPED] = float_round_to_zero,
        [FPCR_DYN_RM_MINUS]   = float_round_down,
        [FPCR_DYN_RM_NORMAL]  = float_round_nearest_even,
        [FPCR_DYN_RM_PLUS]    = float_round_up,
    };
    return rounding_mode_map[(fpcr >> FPCR_DYN_SHIFT) & 3];
}

void alpha_cpu_set_fpcr(CPUAlphaState *env, uint64_t val)
{
    uint32_t fpcr = (val >> 32) & FPCR_MASK;

    /* Mask off the precise disable bits when not supported. */
    if (!cpu_amask_feature(trap, env_archcpu(env))) {
        fpcr &= ~FPCR_DISABLE_MASK;
    }

    if (fpcr & FPCR_STATUS_MASK) {
        fpcr |= FPCR_SUM;
    }

    /* Break apart the FPCR value. */
    env->fpcr = fpcr;
    env->fpcr_dyn_round = alpha_fpcr_dyn_rounding_mode(fpcr);

    /*
     * UNDZ alone controls whether underflowed results are flushed
     * to zero. UNFD separately controls whether underflow traps
     * are disabled. The original code required both bits, which
     * prevented denormal flushing when UNDZ was set but UNFD
     * was clear - causing incorrect results in programs like
     * Pinball that rely on proper denormal handling.
     */
    env->fpcr_flush_to_zero = !!(fpcr & FPCR_UNDZ);

    set_flush_inputs_to_zero(fpcr & FPCR_DNZ, &env->fp_status);
}

uint64_t alpha_cpu_get_fpcr(CPUAlphaState *env)
{
    return (uint64_t)env->fpcr << 32;
}

static inline int alpha_exceptbits_from_host(int host_bits)
{
    int target_bits = 0;

    if (host_bits & float_flag_invalid) {
        target_bits |= R_ARITH_ISS_INV_MASK;
    }
    if (host_bits & float_flag_overflow) {
        target_bits |= R_ARITH_ISS_FOV_MASK;
    }
    if (host_bits & float_flag_underflow) {
        target_bits |= R_ARITH_ISS_UNF_MASK;
    }
    if (host_bits & float_flag_divbyzero) {
        target_bits |= R_ARITH_ISS_DZE_MASK;
    }
    if (host_bits & float_flag_inexact) {
        target_bits |= R_ARITH_ISS_INE_MASK;
    }
    if (host_bits & float_flag_invalid_cvti) {
        target_bits |= R_ARITH_ISS_IOV_MASK;
    }
    return target_bits;
}

static inline uint32_t alpha_exceptbits_summary_mask(CPUAlphaState *env,
                                                     bool swc)
{
    uint32_t target_bits = 0;

    if (swc) {
        if (env->fpcr & FPCR_INVD) {
            target_bits |= R_ARITH_ISS_INV_MASK;
        }
        if (env->fpcr & FPCR_DZED) {
            target_bits |= R_ARITH_ISS_DZE_MASK;
        }
        if (env->fpcr & FPCR_OVFD) {
            target_bits |= R_ARITH_ISS_FOV_MASK;
        }
        if (env->fpcr & FPCR_UNFD) {
            target_bits |= R_ARITH_ISS_UNF_MASK;
        }
        if (env->fpcr & FPCR_INED) {
            target_bits |= R_ARITH_ISS_INE_MASK;
        }
    }
    return target_bits;
}

static inline uint32_t alpha_fpcr_status(CPUAlphaState *env)
{
    return (env->fpcr & FPCR_STATUS_MASK) >> FPCR_STATUS_SHIFT;
}

static inline int alpha_exceptbits_summary(CPUAlphaState *env, uint32_t qual)
{
    int flags = get_float_exception_flags(&env->fp_status);
    int ret = 0;

    if (unlikely(flags)) {
        ret = alpha_exceptbits_from_host(flags);

        /*
         * Deal with flag masks that certain instructions require.
         */
        if ((qual & QUAL_U) == 0) {
            /* Note that QUAL_U == QUAL_V, so ignore either. */
            ret &= ~(R_ARITH_ISS_UNF_MASK |
                     R_ARITH_ISS_IOV_MASK);
        }
        if ((qual & QUAL_I) == 0) {
            ret &= ~(R_ARITH_ISS_INE_MASK);
        }
        if ((qual & QUAL_S) != 0) {
            /*
             * 4.7.7.11 IEEE Denormal Control Bits
             *
             * With DNZ set, an IEEE operation with a denormal operand never
             * generates an overflow, underflow, or inexact result arithmetic
             * trap.
             */
            if (get_flush_inputs_to_zero(&env->fp_status)) {
                ret &= ~(R_ARITH_ISS_UNF_MASK |
                         R_ARITH_ISS_INE_MASK |
                         R_ARITH_ISS_FOV_MASK);
            }
        }
    }
    return ret;
}

/*
 * Check for exceptions after an IEEE operation.
 */
static void check_float_exceptions(CPUAlphaState *env, uint32_t qual,
                                  uintptr_t ra)
{
    int flags, shifted;
    uint32_t status, mask;
    uint64_t iss = 0;
    bool swc = qual & QUAL_S;

    status = alpha_fpcr_status(env);
    flags = alpha_exceptbits_summary(env, qual);
    mask = alpha_exceptbits_summary_mask(env, swc);
    shifted = flags >> 1;                           /* shift over SWC bit */

    /*
     * 6.7.1 Status Flags
     *
     * Floating-point exceptions, for which the associated FPCR status bit
     * is clear or for which the associated trap is enabled, result in a
     * hardware trap to the ARITH PALcode routine. The EXC_SUM register
     * contains information to allow this routine to update the FPCR
     * appropriately, and to decide whether to report the exception to the
     * operating system.
     *
     * Fix: The original condition (status & shifted) == shifted fired
     * when status bits were SET, meaning it trapped on already-seen
     * exceptions. Per the spec, we should trap when the status bit is
     * CLEAR (exception is newly occurring). The corrected condition uses
     * ~status to identify bits that are clear in the current FPCR status,
     * meaning this exception has not been previously recorded and therefore
     * requires a trap to let the PALcode update FPCR and notify the OS.
     */
    if ((~status & shifted) != 0) {
        iss |= (uint64_t)(~status & shifted) << R_ARITH_ISS_SET_INV_SHIFT;
    }

    /* Handle masked exception flags. */
    iss |= flags & ~mask;

    /* Take the exception if necessary. */
    if (unlikely(iss)) {
        /* Place the SWC bit into the summary. */
        if (qual & QUAL_S) {
            iss |= R_ARITH_ISS_SWC_MASK;
        }

        alpha_env_reset_float_exceptions(env);
        do_raise_exception_ra(env, EXCP_ARITH, syn_arith(iss), ra);
    }
}

static inline float64 ieee_input(float_status *fpst, float64 val, uint32_t qual,
                                 bool is_cmp)
{
    if (float64_is_denormal(val)) {
        /* Denormals without /S raise an exception.  */
        if ((qual & QUAL_S) == 0 ||
            !get_flush_inputs_to_zero(fpst)) {
            float_raise(float_flag_invalid, fpst);
        }
        val = float64_squash_input_denormal(val, fpst);
    }
    if (float64_is_any_nan(val)) {
        if (float64_is_signaling_nan(val, fpst)) {
            float_raise(float_flag_invalid, fpst);
            val = float64_silence_nan(val, fpst);
        }
    }
    /*
     * Fix: Removed incorrect infinity -> float_flag_invalid raise.
     *
     * The original code raised float_flag_invalid for any infinity
     * input when is_cmp was false. This is incorrect for IEEE mode:
     * IEEE 754 defines infinity as a valid representable value and
     * arithmetic on infinities (e.g., inf + 1.0, inf * 2.0) is
     * well-defined and should not raise an invalid operation exception.
     *
     * Only specific operations on infinities are invalid per IEEE 754,
     * such as inf - inf or 0 * inf, and softfloat already handles
     * those cases correctly internally. Raising invalid here caused
     * legitimate infinity arithmetic in programs like Internet Explorer's
     * layout engine and Pinball's physics to incorrectly fault.
     *
     * For VAX format operations (which do not support infinity),
     * the invalid raise belongs in a separate vax_input() path,
     * not here in the IEEE path.
     */
    return val;
}

uint64_t HELPER(ieee_lds)(uint32_t f32)
{
    return ieee_single_to_register(f32);
}

uint32_t HELPER(ieee_sts)(uint64_t f64)
{
    return ieee_register_to_single(f64);
}

/*
 * Fix: Added check_float_exceptions to ieee_cvtst.
 * Converting single->double can raise inexact or invalid
 * (e.g., signaling NaN input) which must be reported.
 */
uint64_t HELPER(ieee_cvtst)(CPUAlphaState *env, uint64_t val, uint32_t qual)
{
    float_status *fpst = &env->fp_status;
    float64 input, ret;

    alpha_env_reset_float_exceptions(env);
    input = ieee_input(fpst, val, qual, false);
    ret = float32_to_float64(ieee_register_to_single(input), fpst);
    check_float_exceptions(env, qual, GETPC());
    return ret;
}

/*
 * Fix: Added check_float_exceptions to ieee_cvtts.
 * Converting double->single can raise overflow, underflow,
 * or inexact which must be reported to the PALcode handler.
 */
uint64_t HELPER(ieee_cvtts)(CPUAlphaState *env, uint64_t val, uint32_t qual)
{
    float_status *fpst = &env->fp_status;
    float64 input;
    float32 ret;

    alpha_env_reset_float_exceptions(env);
    input = ieee_input(fpst, val, qual, false);
    ret = float64_to_float32(input, fpst);
    check_float_exceptions(env, qual, GETPC());
    return ieee_single_to_register(ret);
}

uint64_t HELPER(ieee_cvtqs)(CPUAlphaState *env, uint64_t val, uint32_t qual)
{
    float32 ret;

    alpha_env_reset_float_exceptions(env);
    ret = int64_to_float32(val, &env->fp_status);
    return ieee_single_to_register(ret);
}

uint64_t HELPER(ieee_cvtqt)(CPUAlphaState *env, uint64_t val, uint32_t qual)
{
    float64 ret;

    alpha_env_reset_float_exceptions(env);
    ret = int64_to_float64(val, &env->fp_status);
    check_float_exceptions(env, qual, GETPC());
    return ret;
}

uint64_t HELPER(ieee_cvtql)(CPUAlphaState *env, uint64_t val, uint32_t qual)
{
    float_status *fpst = &env->fp_status;

    alpha_env_reset_float_exceptions(env);
    if (unlikely(val != (int32_t)val)) {
        float_raise(float_flag_invalid_cvti | float_flag_inexact, fpst);
    }

    return ((extract64(val, 30, 2)) << 62) |
            (extract64(val, 0, 30) << 29);
}

uint64_t HELPER(ieee_cvttq)(CPUAlphaState *env, uint64_t val, uint32_t qual)
{
    float_status *fpst = &env->fp_status;
    FloatRoundMode mode = get_float_rounding_mode(fpst);
    float64 input;
    int64_t ret;

    alpha_env_reset_float_exceptions(env);
    input = ieee_input(fpst, val, qual, false);
    ret = float64_to_int64_modulo(input, mode, fpst);
    return ret;
}

/*
 * Fix: Added check_float_exceptions to all four comparison helpers.
 * Comparisons involving signaling NaNs must raise invalid operation.
 * Without the check, the exception was silently swallowed.
 */
#define GEN_HELPER_FCMP(NAME, CMPFN)                        \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint64_t b, uint32_t qual)            \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
    float64 lhs, rhs, ret;                                  \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    lhs = ieee_input(fpst, a, qual, true);                  \
    rhs = ieee_input(fpst, b, qual, true);                  \
    if (CMPFN(lhs, rhs, fpst)) {                            \
        ret = float64_two;                                  \
    } else {                                                \
        ret = 0;                                            \
    }                                                       \
    check_float_exceptions(env, qual, GETPC());             \
    return ret;                                             \
}

GEN_HELPER_FCMP(ieee_cmptun, float64_unordered_quiet)
GEN_HELPER_FCMP(ieee_cmpteq, float64_eq_quiet)
GEN_HELPER_FCMP(ieee_cmptle, float64_le_quiet)
GEN_HELPER_FCMP(ieee_cmptlt, float64_lt_quiet)

/*
 * Fix: Added check_float_exceptions to all S-format binary
 * and unary operation macros.
 *
 * Without this, float32 arithmetic exceptions (overflow, underflow,
 * inexact, invalid, divbyzero) were never reported to the PALcode
 * ARITH handler. This meant:
 *   - Underflow producing denormals went undetected
 *   - Programs relying on SIGFPE or structured exception handling
 *     for FP faults (e.g., IE's error recovery paths) never
 *     received the signal and continued with corrupt values
 *   - Pinball physics calculations silently produced NaN/denormal
 *     results that propagated until causing an unrelated fault
 */
#define GEN_HELPER_BINOP_S(NAME, FN)                        \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint64_t b, uint32_t qual)            \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
    float64 input1, input2;                                 \
    float32 lhs, rhs, ret;                                  \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    input1 = ieee_input(fpst, a, qual, false);              \
    input2 = ieee_input(fpst, b, qual, false);              \
    lhs = ieee_register_to_single(input1);                  \
    rhs = ieee_register_to_single(input2);                  \
    ret = FN(lhs, rhs, fpst);                               \
    check_float_exceptions(env, qual, GETPC());             \
    return ieee_single_to_register(ret);                    \
}

#define GEN_HELPER_UNOP_S(NAME, FN)                         \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint32_t qual)                        \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
    float64 input;                                          \
    float32 lhs, ret;                                       \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    input = ieee_input(fpst, a, qual, false);               \
    lhs = ieee_register_to_single(input);                   \
    ret = FN(lhs, fpst);                                    \
    check_float_exceptions(env, qual, GETPC());             \
    return ieee_single_to_register(ret);                    \
}

GEN_HELPER_BINOP_S(ieee_adds, float32_add)
GEN_HELPER_BINOP_S(ieee_subs, float32_sub)
GEN_HELPER_BINOP_S(ieee_muls, float32_mul)
GEN_HELPER_BINOP_S(ieee_divs, float32_div)
GEN_HELPER_UNOP_S(ieee_sqrts, float32_sqrt)

/*
 * Fix: Added check_float_exceptions to all T-format binary
 * and unary operation macros. Same reasoning as S-format above
 * but for float64 (double precision) operations.
 *
 * T-format ops are the primary path for most double-precision
 * math in Windows NT applications on Alpha. The missing exception
 * check here is the most likely direct cause of the Pinball and
 * early IE crashes described in the bug report.
 */
#define GEN_HELPER_BINOP_T(NAME, FN)                        \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint64_t b, uint32_t qual)            \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
    float64 lhs, rhs, ret;                                  \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    lhs = ieee_input(fpst, a, qual, false);                 \
    rhs = ieee_input(fpst, b, qual, false);                 \
    ret = FN(lhs, rhs, fpst);                               \
    check_float_exceptions(env, qual, GETPC());             \
    return ret;                                             \
}

#define GEN_HELPER_UNOP_T(NAME, FN)                         \
uint64_t HELPER(NAME)(CPUAlphaState *env, uint64_t a,       \
                      uint32_t qual)                        \
{                                                           \
    float_status *fpst = &env->fp_status;                   \
    float64 lhs, ret;                                       \
                                                            \
    alpha_env_reset_float_exceptions(env);                  \
    lhs = ieee_input(fpst, a, qual, false);                 \
    ret = FN(lhs, fpst);                                    \
    check_float_exceptions(env, qual, GETPC());             \
    return ret;                                             \
}

GEN_HELPER_BINOP_T(ieee_addt, float64_add)
GEN_HELPER_BINOP_T(ieee_subt, float64_sub)
GEN_HELPER_BINOP_T(ieee_mult, float64_mul)
GEN_HELPER_BINOP_T(ieee_divt, float64_div)
GEN_HELPER_UNOP_T(ieee_sqrtt, float64_sqrt)

void HELPER(check_float_exceptions)(CPUAlphaState *env, uint32_t qual)
{
    check_float_exceptions(env, qual, GETPC());
}

void HELPER(set_rounding_mode)(CPUAlphaState *env, uint32_t val)
{
    set_float_rounding_mode(val, &env->fp_status);
}

void HELPER(set_flush_zero)(CPUAlphaState *env, uint32_t val)
{
    set_flush_to_zero(val, &env->fp_status);
}

void HELPER(set_fpcr)(CPUAlphaState *env, uint64_t val)
{
    alpha_cpu_set_fpcr(env, val);
}

uint64_t HELPER(get_fpcr)(CPUAlphaState *env)
{
    return alpha_cpu_get_fpcr(env);
}
