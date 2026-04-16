/* pystrtod_ryu.h - Ryu-based adapter replacing _Py_dg_dtoa for modes 0, 2, 3
 *
 * Copyright 2024 Python Software Foundation
 *
 * _PyRyu_dtoa() is a drop-in replacement for _Py_dg_dtoa() for the three
 * modes used by format_float_short() in pystrtod.c:
 *
 *   mode 0  – shortest round-trip string  (repr / str)
 *   mode 2  – N significant digits        (%e, %g)
 *   mode 3  – N digits past decimal point (%f); supports ndigits >= 0 via
 *             d2fixed_buffered_n, and ndigits < 0 (float.__round__ with a
 *             negative argument) via a small adapter that rounds to the
 *             nearest multiple of 10^(-ndigits) using the exact digit
 *             expansion of floor(|d|) plus a "fractional part > 0" bit.
 *
 * Output contract (matches Gay's dtoa):
 *   - Returns a PyMem_Malloc'd buffer containing raw decimal digits only
 *     (no sign, no decimal point, no exponent).
 *   - *sign  : 1 if the value is negative, 0 otherwise.
 *   - *decpt : decimal-point position in Gay's convention:
 *                decpt == k  means the decimal point sits after the k-th digit,
 *                counting from the left of the digit string.
 *                decpt == 1  → "d.ddd..."   (1 digit before the point)
 *                decpt == 0  → "0.ddd..."   (0 digits before the point)
 *                decpt == -2 → "0.00ddd..." (2 leading zeros after point)
 *                decpt can be very large (e.g. 309 for 1e308) or very
 *                negative (e.g. -323 for 5e-324).
 *   - *digits_end : pointer one past the last digit in the buffer.
 *   - Special values (Inf / NaN) are returned as literal strings
 *     "Infinity" or "NaN" with *sign set appropriately; format_float_short
 *     detects these by checking digits[0].
 *   - Returns NULL on PyMem_Malloc failure (caller must set PyErr_NoMemory).
 *
 * The returned buffer must be freed with PyMem_Free().
 */

#ifndef PYSTRTOD_RYU_H
#define PYSTRTOD_RYU_H

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>           /* fabs, floor, signbit, isnan, isinf */
#include "pymem.h"          /* PyMem_Malloc / PyMem_Free */
#include "_ryu/ryu.h"       /* d2s_buffered_n, d2exp_buffered_n,
                               d2fixed_buffered_n */

/* Maximum buffer sizes for Ryu's output.
 *
 * d2s always fits in ~25 chars (sign + 17 digits + 'E' + sign + 3-digit exp).
 *
 * d2exp and d2fixed write a size proportional to the requested precision.
 * Python's format code accepts arbitrarily large precisions (e.g. "%.123456f"),
 * so we must size the heap buffer dynamically — a fixed 2000-byte buffer
 * would be smashed.  Worst-case output sizes (from reading the Ryu source):
 *
 *   d2fixed: [sign] + up to 309 integer digits + '.' + precision + NUL
 *          ≤ 312 + precision
 *   d2exp  : [sign] + 1 digit + '.' + precision + 'e' + sign + 4 exp + NUL
 *          ≤ 10 + precision
 *
 * The helpers below round up generously (+64 slack) and clamp to a modest
 * minimum so negative/zero precision still allocates a sane-sized buffer.
 * _PYRYU_D2FIXED_BUFSIZE is retained as a small stack buffer for the
 * integer-digit extraction in ryu_mode3_neg (precision=0 path).
 */
#define _PYRYU_D2S_BUFSIZE   32
#define _PYRYU_D2FIXED_BUFSIZE 512
static inline size_t _pyryu_d2fixed_bufsize(int precision) {
    size_t p = (precision > 0) ? (size_t)precision : 0;
    size_t n = p + 384;
    return n < 512 ? 512 : n;
}
static inline size_t _pyryu_d2exp_bufsize(int precision) {
    size_t p = (precision > 0) ? (size_t)precision : 0;
    size_t n = p + 96;
    return n < 128 ? 128 : n;
}

/* -------------------------------------------------------------------------
 * FP fast path for Ryu mode 2 (%e / %g)
 *
 * Ryu's d2exp_buffered_n runs the full table-driven algorithm on every
 * call (~230 ns).  Gay's dtoa mode 2, which Ryu replaces, has an FP
 * approximation path that short-circuits easy inputs in ~150 ns.  This
 * benchmark-visible ~20% gap is algorithmic — the adapter can't close it
 * from the outside.  So we reintroduce Gay's *approach* in-adapter: use
 * double-precision arithmetic when we can prove the result is correctly
 * rounded, and fall back to d2exp otherwise.
 *
 * Correctness sketch (see _pyryu_fast_mode2 below for the routine):
 *   Let d > 0 finite, P ∈ [0, 14].  Choose k = floor(log₁₀ d) and let
 *   v* = d · 10^(P-k) be the true scaled value (a real number with
 *   10^P ≤ v* < 10^(P+1), i.e. P+1 decimal digits).  We compute
 *      v  = round_to_double( d · scale )     where scale = 10^|P-k| (exact).
 *   By IEEE 754, |v − v*| ≤ 0.5·ULP(v).
 *
 *   The output m = round_half_even(v*) differs from round_half_even(v)
 *   only when v and v* straddle a half-integer — i.e. when
 *      |0.5 − |v − rint(v)||  <  |v − v*| ≤ 0.5·ULP(v).
 *
 *   Conservative guard: require 0.5 − err ≥ 2·ULP(v) = |v|·2⁻⁵².  If the
 *   guard holds, rint(v) equals round_half_even(v*).  If it doesn't, bail
 *   out to d2exp.
 *
 * Precision limit: P ≤ 14 so m < 10¹⁵ < 2⁵³ is exactly representable as
 * a double, and the cast to uint64_t is lossless.
 *
 * Scale range: we only tabulate 10⁰..10²² (exactly representable).  When
 * |P − k| > 22 we bail out.  This covers |d| in roughly [10⁻²², 10²²] for
 * typical precision — the common case.  Extreme magnitudes (1e100, 1e-100,
 * subnormals, etc.) fall back to d2exp with no correctness concern.
 * ------------------------------------------------------------------------- */
static const double _pyryu_pow10_exact[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22,
};

static const uint64_t _pyryu_pow10_u64[16] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
};

/* Returns 1 on success with P+1 digits written to digits_out (no NUL, no
 * sign) and *exp_out set so that d ≈ 0.<digits> × 10^(*exp_out + 1),
 * matching Gay's decpt convention.  Returns 0 when the FP computation
 * cannot be proven correctly rounded.
 *
 * Precondition: d > 0 finite, 0 ≤ precision ≤ 14.
 */
static int
_pyryu_fast_mode2(double d, int precision,
                  char *digits_out, int *exp_out)
{
    assert(d > 0.0);
    assert(precision >= 0 && precision <= 14);

    /* Decimal exponent estimate (may be off by one; we check below). */
    int k = (int)floor(log10(d));

    /* Scale factor: 10^|P-k| from the exact-powers table. */
    int se = precision - k;
    double v;
    if (se >= 0) {
        if (se > 22) return 0;
        v = d * _pyryu_pow10_exact[se];
    }
    else {
        if (-se > 22) return 0;
        v = d / _pyryu_pow10_exact[-se];
    }

    /* Verify v lies in [10^P, 10^(P+1)).  FP log10 can round k one too
     * high when d is very close to a power of 10 from below, producing
     * v ≈ 10^P − ε.  Rint would then round v up to 10^P, which looks
     * like a legal P+1-digit result but in fact represents 10^k (one
     * magnitude too high for d) — the output would be silently off by
     * a factor of 10.  pow10_exact is exact up to 10^22 and precision+1
     * ≤ 15, so the comparison is always against exact bounds. */
    if (v < _pyryu_pow10_exact[precision] ||
        v >= _pyryu_pow10_exact[precision + 1]) {
        return 0;
    }

    /* Round to nearest-even integer and compute |v - m|. */
    double m_d = rint(v);
    double err = v - m_d;
    if (err < 0) err = -err;

    /* Slop guard: fail if v is within 2·ULP of a half-integer.  See the
     * correctness sketch in the comment above. */
    double err_bound = v * 0x1p-52;
    if (0.5 - err < err_bound) {
        return 0;
    }

    /* Cast to uint64_t.  m_d is in [10^P, 10^(P+1)] ⊆ [1, 10^15] so the
     * cast is always lossless. */
    uint64_t mi = (uint64_t)m_d;

    /* Digit count check.  If the initial k was off by one, mi can be 10× too
     * big (mi == 10^(P+1)) or too small.  The overflow case reshapes
     * cleanly; for the too-small case we fall back rather than retry. */
    if (mi == _pyryu_pow10_u64[precision] * 10ULL) {
        /* v rounded up across a power-of-10 boundary.  Output is
         * "1" followed by P zeros, exponent bumped. */
        digits_out[0] = '1';
        for (int i = 1; i <= precision; i++) {
            digits_out[i] = '0';
        }
        *exp_out = k + 1;
        return 1;
    }
    if (mi < _pyryu_pow10_u64[precision] ||
        mi >= _pyryu_pow10_u64[precision] * 10ULL) {
        return 0;
    }

    /* Emit digits low-to-high. */
    for (int i = precision; i >= 0; i--) {
        digits_out[i] = (char)('0' + (mi % 10));
        mi /= 10;
    }
    *exp_out = k;
    return 1;
}

/* -------------------------------------------------------------------------
 * parse_ryu_d2s_output
 *
 * Parse the output of d2s_buffered_n into digits/decpt/sign.
 * d2s produces scientific notation like:
 *   "1E0"        (integer 1)
 *   "1.5E0"      (1.5)
 *   "1.23E-4"    (0.000123)
 *   "1.23E100"   (1.23e100)
 *   "-1.5E0"     (never – sign is stripped before calling d2s)
 *   "NaN"
 *   "Infinity"
 *   "-Infinity"
 *
 * Returns 1 on success, 0 on memory failure.
 * On success, *out_digits is a PyMem_Malloc'd digit string.
 * ------------------------------------------------------------------------- */
static int
parse_ryu_d2s_output(const char *ryu_buf, int ryu_len,
                     char **out_digits, int *decpt, int *sign,
                     char **digits_end)
{
    const char *p = ryu_buf;
    const char *end = ryu_buf + ryu_len;

    /* Sign */
    *sign = 0;
    if (p < end && *p == '-') {
        *sign = 1;
        ++p;
    }

    /* Special values: NaN, Infinity */
    if (p < end && (*p == 'N' || *p == 'n' || *p == 'I' || *p == 'i')) {
        /* Return a copy of the special string WITHOUT the leading sign.
         * format_float_short checks digits[0] for 'I' or 'N' and uses
         * the separately-returned *sign for the sign character. */
        size_t special_len = (size_t)(end - p);
        char *buf = (char *)PyMem_Malloc(special_len + 1);
        if (buf == NULL)
            return 0;
        memcpy(buf, p, special_len);
        buf[special_len] = '\0';
        /* *sign was already set from the leading '-' if present */
        *out_digits = buf;
        *digits_end = buf + special_len;
        *decpt = 9999; /* unused for special values */
        return 1;
    }

    /* Collect mantissa digits (skip the decimal point) */
    /* d2s format: [sign] digit ['.' digits] 'E' ['-'] digits */
    char mant_digits[20]; /* at most 17 significant digits */
    int mant_len = 0;
    int dot_pos = -1; /* position of '.' among mantissa chars */

    while (p < end && *p != 'E' && *p != 'e') {
        if (*p == '.') {
            dot_pos = mant_len; /* dot is after mant_len digits */
        } else {
            assert(mant_len < (int)(sizeof(mant_digits)));
            mant_digits[mant_len++] = *p;
        }
        ++p;
    }
    if (dot_pos < 0)
        dot_pos = mant_len; /* no dot: all digits are before the exponent */

    /* Parse exponent */
    int exp = 0;
    if (p < end && (*p == 'E' || *p == 'e')) {
        ++p;
        int exp_sign = 1;
        if (p < end && *p == '-') { exp_sign = -1; ++p; }
        else if (p < end && *p == '+') { ++p; }
        while (p < end) {
            exp = exp * 10 + (*p - '0');
            ++p;
        }
        exp *= exp_sign;
    }

    /* Gay's decpt convention:
     *   The value is mant_digits * 10^(exp - (mant_len - dot_pos))
     *   In Gay's terms: value = 0.<digits> * 10^decpt
     *   So decpt = dot_pos + exp
     * Example: "1.5E0"  → dot_pos=1, exp=0  → decpt=1  → "1.5"  ✓
     *           "1.23E4" → dot_pos=1, exp=4  → decpt=5  → "12300."
     *           "1E-4"   → dot_pos=1, exp=-4 → decpt=-3 → "0.000<1>"
     */
    *decpt = dot_pos + exp;

    /* Allocate output buffer */
    char *buf = (char *)PyMem_Malloc((size_t)mant_len + 1);
    if (buf == NULL)
        return 0;
    memcpy(buf, mant_digits, (size_t)mant_len);
    buf[mant_len] = '\0';
    *out_digits = buf;
    *digits_end = buf + mant_len;
    return 1;
}

/* -------------------------------------------------------------------------
 * parse_ryu_d2exp_output
 *
 * Parse d2exp_buffered_n output (e.g. "1.234560e+02") into digits/decpt/sign.
 * d2exp format: ['-'] digit ['.' digits] 'e' ('+'/'-') DD[D]
 * The precision argument to d2exp is (ndigits_total - 1) digits after '.'.
 * ------------------------------------------------------------------------- */
static int
parse_ryu_d2exp_output(const char *ryu_buf, int ryu_len,
                       char **out_digits, int *decpt, int *sign,
                       char **digits_end)
{
    /* d2exp can output "NaN" / "Infinity" for specials */
    const char *p = ryu_buf;
    const char *end = ryu_buf + ryu_len;

    *sign = 0;
    if (p < end && *p == '-') { *sign = 1; ++p; }

    if (p < end && (*p == 'N' || *p == 'n' || *p == 'I' || *p == 'i')) {
        size_t special_len = (size_t)(end - p);
        char *buf = (char *)PyMem_Malloc(special_len + 1);
        if (buf == NULL) return 0;
        memcpy(buf, p, special_len);
        buf[special_len] = '\0';
        *out_digits = buf;
        *digits_end = buf + special_len;
        *decpt = 9999;
        return 1;
    }

    /* Collect mantissa digits */
    char *mant = (char *)PyMem_Malloc((size_t)ryu_len + 1);
    if (mant == NULL) return 0;
    int mant_len = 0;
    int dot_pos = -1;

    while (p < end && *p != 'e' && *p != 'E') {
        if (*p == '.') {
            dot_pos = mant_len;
        } else {
            mant[mant_len++] = *p;
        }
        ++p;
    }
    if (dot_pos < 0) dot_pos = mant_len;

    int exp = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        int exp_sign = 1;
        if (p < end && *p == '-') { exp_sign = -1; ++p; }
        else if (p < end && *p == '+') { ++p; }
        while (p < end) { exp = exp * 10 + (*p - '0'); ++p; }
        exp *= exp_sign;
    }

    *decpt = dot_pos + exp;

    /* Strip trailing zeros — Gay's dtoa mode 2 never returns trailing zeros,
     * and format_float_short uses digits_len (= digits_end - digits) as the
     * count of significant digits (vdigits_end = digits_len for 'e' format).
     * Ryu's d2exp always pads to the requested precision, so we must trim. */
    while (mant_len > 1 && mant[mant_len - 1] == '0')
        --mant_len;

    mant[mant_len] = '\0';
    *out_digits = mant;
    *digits_end = mant + mant_len;
    return 1;
}

/* -------------------------------------------------------------------------
 * parse_ryu_d2exp_inplace
 *
 * Like parse_ryu_d2exp_output, but takes a PyMem_Malloc'd buffer and
 * rewrites its contents in place — the same buffer is returned as
 * *out_digits (ownership transferred to the caller, who must free it).
 * No extra heap allocation is performed.
 *
 * Safe because the mantissa is compacted toward the front of the buffer
 * (write cursor ≤ read cursor − 0 always) and the exponent suffix lies
 * strictly past the highest write position.
 * ------------------------------------------------------------------------- */
static int
parse_ryu_d2exp_inplace(char *buf, int len,
                        char **out_digits, int *decpt, int *sign,
                        char **digits_end)
{
    char *p = buf;
    char *end = buf + len;

    *sign = 0;
    if (p < end && *p == '-') { *sign = 1; ++p; }

    if (p < end && (*p == 'N' || *p == 'n' || *p == 'I' || *p == 'i')) {
        size_t special_len = (size_t)(end - p);
        if (p != buf) memmove(buf, p, special_len);
        buf[special_len] = '\0';
        *out_digits = buf;
        *digits_end = buf + special_len;
        *decpt = 9999;
        return 1;
    }

    int mant_len = 0;
    int dot_pos = -1;
    while (p < end && *p != 'e' && *p != 'E') {
        if (*p == '.') {
            dot_pos = mant_len;
        } else {
            buf[mant_len++] = *p;
        }
        ++p;
    }
    if (dot_pos < 0) dot_pos = mant_len;

    int exp = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        int exp_sign = 1;
        if (p < end && *p == '-') { exp_sign = -1; ++p; }
        else if (p < end && *p == '+') { ++p; }
        while (p < end) { exp = exp * 10 + (*p - '0'); ++p; }
        exp *= exp_sign;
    }

    *decpt = dot_pos + exp;

    while (mant_len > 1 && buf[mant_len - 1] == '0')
        --mant_len;

    buf[mant_len] = '\0';
    *out_digits = buf;
    *digits_end = buf + mant_len;
    return 1;
}

/* -------------------------------------------------------------------------
 * parse_ryu_d2fixed_output
 *
 * Parse d2fixed_buffered_n output (e.g. "123.456000") into digits/decpt/sign.
 * d2fixed format: ['-'] digits ['.' digits]
 * The returned digit string contains all significant digits; decpt tells
 * format_float_short where the decimal point sits.
 * ------------------------------------------------------------------------- */
static int
parse_ryu_d2fixed_output(const char *ryu_buf, int ryu_len,
                          char **out_digits, int *decpt, int *sign,
                          char **digits_end)
{
    const char *p = ryu_buf;
    const char *end = ryu_buf + ryu_len;

    *sign = 0;
    if (p < end && *p == '-') { *sign = 1; ++p; }

    /* Special values */
    if (p < end && (*p == 'N' || *p == 'n' || *p == 'I' || *p == 'i')) {
        size_t special_len = (size_t)(end - p);
        char *buf = (char *)PyMem_Malloc(special_len + 1);
        if (buf == NULL) return 0;
        memcpy(buf, p, special_len);
        buf[special_len] = '\0';
        *out_digits = buf;
        *digits_end = buf + special_len;
        *decpt = 9999;
        return 1;
    }

    /* d2fixed output: all digits of the integer part, then optionally
     * '.' followed by fractional digits.
     * We collect everything except the '.', record where it was.
     */
    char *mant = (char *)PyMem_Malloc((size_t)ryu_len + 1);
    if (mant == NULL) return 0;
    int mant_len = 0;
    int int_digits = -1; /* number of digits before the '.' */

    while (p < end) {
        if (*p == '.') {
            int_digits = mant_len;
        } else {
            mant[mant_len++] = *p;
        }
        ++p;
    }
    if (int_digits < 0)
        int_digits = mant_len; /* no decimal point: pure integer */

    /* Gay's decpt = number of digits before the decimal point.
     * For "123.456" → int_digits=3, decpt=3 ✓
     * For "0.001"   → int_digits=1 (the leading '0'), decpt=1
     *   but Gay would say decpt=-2 for 0.001 (0.001 = .001 * 10^0... wait)
     *
     * Actually Gay's convention: decpt = position of decimal point in the
     * *digit string* (which has leading zeros stripped).  For 0.001, the
     * digit string is "1" and decpt is -2 (meaning 0.001 = 1 * 10^(-3),
     * so the decimal point is 2 positions to the left of the digit).
     *
     * d2fixed("0.001000", 6) gives "0.001000".
     * int_digits=1 (the "0"), and then we have digits "0001000".
     * We need to strip leading zeros from the digit string and adjust decpt.
     */

    /* Strip trailing zeros from fractional part (format_float_short will
     * re-add them as needed, but Gay's dtoa never includes trailing zeros
     * in the returned digit string for modes 0/2). However for mode 3
     * (fixed-point), Gay DOES include trailing zeros up to precision.
     * format_float_short handles trailing zeros itself via vdigits_end.
     * So we can keep them — but we must strip leading zeros from the
     * integer part and adjust decpt accordingly.
     */

    /* Find where non-zero digits start */
    int first_nonzero = 0;
    while (first_nonzero < mant_len && mant[first_nonzero] == '0')
        ++first_nonzero;

    if (first_nonzero == mant_len) {
        /* All zeros: value is 0.000...0 */
        /* Gay returns "0" with decpt=1 for exactly 0 */
        mant[0] = '0';
        mant[1] = '\0';
        *decpt = 1;
        *out_digits = mant;
        *digits_end = mant + 1;
        return 1;
    }

    /* Adjust decpt: how many of the leading zeros are in the integer part? */
    /* int_digits includes leading zeros before the decimal point.
     * If int_digits leading zeros exist in the integer part, each one
     * shifts decpt down by 1. But we only strip zeros from before the
     * first non-zero digit.
     */
    int leading_zeros_stripped = first_nonzero;
    *decpt = int_digits - leading_zeros_stripped;

    /* Shift digits in-place */
    mant_len -= first_nonzero;
    memmove(mant, mant + first_nonzero, (size_t)mant_len);
    mant[mant_len] = '\0';

    *out_digits = mant;
    *digits_end = mant + mant_len;
    return 1;
}

/* -------------------------------------------------------------------------
 * parse_ryu_d2fixed_inplace
 *
 * Like parse_ryu_d2fixed_output, but takes a PyMem_Malloc'd buffer and
 * rewrites its contents in place — the same buffer is returned as
 * *out_digits (ownership transferred to the caller).
 * ------------------------------------------------------------------------- */
static int
parse_ryu_d2fixed_inplace(char *buf, int len,
                          char **out_digits, int *decpt, int *sign,
                          char **digits_end)
{
    char *p = buf;
    char *end = buf + len;

    *sign = 0;
    if (p < end && *p == '-') { *sign = 1; ++p; }

    if (p < end && (*p == 'N' || *p == 'n' || *p == 'I' || *p == 'i')) {
        size_t special_len = (size_t)(end - p);
        if (p != buf) memmove(buf, p, special_len);
        buf[special_len] = '\0';
        *out_digits = buf;
        *digits_end = buf + special_len;
        *decpt = 9999;
        return 1;
    }

    int mant_len = 0;
    int int_digits = -1;
    while (p < end) {
        if (*p == '.') {
            int_digits = mant_len;
        } else {
            buf[mant_len++] = *p;
        }
        ++p;
    }
    if (int_digits < 0)
        int_digits = mant_len;

    int first_nonzero = 0;
    while (first_nonzero < mant_len && buf[first_nonzero] == '0')
        ++first_nonzero;

    if (first_nonzero == mant_len) {
        buf[0] = '0';
        buf[1] = '\0';
        *decpt = 1;
        *out_digits = buf;
        *digits_end = buf + 1;
        return 1;
    }

    *decpt = int_digits - first_nonzero;

    mant_len -= first_nonzero;
    memmove(buf, buf + first_nonzero, (size_t)mant_len);
    buf[mant_len] = '\0';

    *out_digits = buf;
    *digits_end = buf + mant_len;
    return 1;
}

/* -------------------------------------------------------------------------
 * ryu_mode3_neg
 *
 * Mode 3 with negative ndigits = -k (k >= 1): round |d| to the nearest
 * multiple of 10^k with banker's tie-to-even against the *exact* value
 * of d.  Gay's _Py_dg_dtoa(d, 3, -k, ...) does the same thing.
 *
 * Algorithm:
 *   1. Extract sign and handle NaN/Inf/0.
 *   2. Let ix = floor(|d|).  Since doubles with |d| >= 2^52 are already
 *      integers, floor() is exact for every finite double.
 *   3. Call d2fixed_buffered_n(ix, 0) to obtain the exact decimal digits
 *      of ix.  (No banker-rounding happens because ix is an integer.)
 *   4. frac_nonzero = (|d| != ix).  This is the only information from the
 *      sub-integer part that matters for rounding at an integer-scale
 *      position: for k >= 1, the tie between Q*10^k and (Q+1)*10^k occurs
 *      exactly at R == 10^k/2 with f == 0.
 *   5. Split the integer digit string into Q (high |ix_len|-k digits) and
 *      R (low k digits).  Compare R against 10^k/2 (= "5" + (k-1) "0"s).
 *   6. Round:
 *        R < 10^k/2          : keep Q
 *        R > 10^k/2          : Q += 1
 *        R == 10^k/2, f > 0  : Q += 1
 *        R == 10^k/2, f == 0 : banker's (Q += 1 iff Q's last digit is odd)
 *   7. Output digits = decimal of Q with trailing zeros stripped,
 *      decpt = k + len(Q_before_stripping) (so value = digits * 10^exp
 *      with exp = decpt - len(digits) == k + stripped_zero_count,
 *      preserving the Q * 10^k value).  If Q == 0, emit "0" with decpt=1.
 *
 * Returns 1 on success, 0 on memory failure.
 * ------------------------------------------------------------------------- */
static int
ryu_mode3_neg(double d, int k,
              char **out_digits, int *decpt, int *sign, char **digits_end)
{
    assert(k >= 1);

    *sign = signbit(d) ? 1 : 0;

    /* NaN / Infinity.  Emit the literal string (no sign — caller tracks it
     * via *sign) and decpt=9999, matching Gay's dtoa convention. */
    if (isnan(d) || isinf(d)) {
        const char *lit = isnan(d) ? "NaN" : "Infinity";
        size_t n = strlen(lit);
        char *buf = (char *)PyMem_Malloc(n + 1);
        if (buf == NULL) return 0;
        memcpy(buf, lit, n + 1);
        *out_digits = buf;
        *digits_end = buf + n;
        *decpt = 9999;
        return 1;
    }

    /* Zero (signed or unsigned). */
    if (d == 0.0) {
        char *buf = (char *)PyMem_Malloc(2);
        if (buf == NULL) return 0;
        buf[0] = '0'; buf[1] = '\0';
        *out_digits = buf;
        *digits_end = buf + 1;
        *decpt = 1;
        return 1;
    }

    double ax = fabs(d);
    double ix = floor(ax);
    int frac_nonzero = (ax != ix);

    /* Exact integer digits of ix.  d2fixed with precision=0 on an integer
     * input performs no rounding: Ryu's first loop emits the exact digits
     * from POW10_SPLIT tables, and the fractional-rounding loop finds no
     * nonzero fractional digits. */
    char intbuf[_PYRYU_D2FIXED_BUFSIZE];
    int intlen = d2fixed_buffered_n(ix, 0, intbuf);
    /* ix >= 0, so no leading '-' in intbuf. */

    /* Case: integer part has fewer digits than k.  Value < 10^(k-1) since
     * intlen <= k-1 and d2fixed emits at least one digit.  That is strictly
     * less than 10^k/2, so we round down to 0 regardless of fractional. */
    if (intlen < k) {
        char *buf = (char *)PyMem_Malloc(2);
        if (buf == NULL) return 0;
        buf[0] = '0'; buf[1] = '\0';
        *out_digits = buf;
        *digits_end = buf + 1;
        *decpt = 1;
        return 1;
    }

    int q_len = intlen - k;  /* digits of Q_before_rounding; may be 0 */

    /* Compare R (the low k digits) against 10^k/2 ("5" + (k-1) zeros). */
    int cmp;
    {
        char r_first = intbuf[q_len];
        if (r_first < '5') {
            cmp = -1;
        }
        else if (r_first > '5') {
            cmp = 1;
        }
        else {
            cmp = 0;
            for (int i = q_len + 1; i < intlen; i++) {
                if (intbuf[i] != '0') {
                    cmp = 1;
                    break;
                }
            }
        }
    }

    int round_up;
    if (cmp < 0) {
        round_up = 0;
    }
    else if (cmp > 0) {
        round_up = 1;
    }
    else {
        /* R == 10^k/2 exactly */
        if (frac_nonzero) {
            round_up = 1;
        }
        else {
            char q_last = (q_len > 0) ? intbuf[q_len - 1] : '0';
            round_up = ((q_last - '0') & 1) ? 1 : 0;
        }
    }

    /* Build Q as a digit string, with room for a possible carry-out. */
    char *qbuf = (char *)PyMem_Malloc((size_t)q_len + 2);
    if (qbuf == NULL) return 0;
    if (q_len == 0) {
        qbuf[0] = round_up ? '1' : '0';
        qbuf[1] = '\0';
    }
    else {
        memcpy(qbuf, intbuf, (size_t)q_len);
        qbuf[q_len] = '\0';
        if (round_up) {
            int i = q_len - 1;
            while (i >= 0 && qbuf[i] == '9') {
                qbuf[i] = '0';
                i--;
            }
            if (i >= 0) {
                qbuf[i]++;
            }
            else {
                /* Carry propagated past leading digit — prepend '1'. */
                memmove(qbuf + 1, qbuf, (size_t)q_len);
                qbuf[0] = '1';
                qbuf[q_len + 1] = '\0';
            }
        }
    }
    int qlen = (int)strlen(qbuf);

    /* Special case: rounded value is 0. */
    if (qlen == 1 && qbuf[0] == '0') {
        *out_digits = qbuf;
        *digits_end = qbuf + 1;
        *decpt = 1;
        return 1;
    }

    /* value = Q * 10^k with Q's decimal digits = qbuf; decpt = k + qlen.
     * Strip trailing zeros from qbuf (decpt is unchanged since the
     * represented value is invariant under digits -> digits+"0" with
     * exp += 0 per our decpt formula). */
    *decpt = k + qlen;
    while (qlen > 1 && qbuf[qlen - 1] == '0') {
        qlen--;
    }
    qbuf[qlen] = '\0';

    *out_digits = qbuf;
    *digits_end = qbuf + qlen;
    return 1;
}

/* -------------------------------------------------------------------------
 * _PyRyu_dtoa  – main entry point
 * ------------------------------------------------------------------------- */
static char *
_PyRyu_dtoa(double d, int mode, int ndigits,
            int *decpt, int *sign, char **digits_end)
{
    char *out_digits = NULL;

    switch (mode) {
    case 0: {
        /* Shortest round-trip representation */
        char buf[_PYRYU_D2S_BUFSIZE];
        int len = d2s_buffered_n(d, buf);
        if (!parse_ryu_d2s_output(buf, len, &out_digits, decpt, sign,
                                  digits_end))
            return NULL;
        break;
    }
    case 2: {
        /* ndigits significant digits (exponential / general format).
         * Gay's mode 2 with ndigits=N gives N significant digits total.
         * d2exp with precision=P gives 1 digit before the point and P after,
         * for a total of P+1 significant digits.  So we pass
         * precision = ndigits - 1.
         *
         * Three paths, in order of preference:
         *   (1) FP fast path — see _pyryu_fast_mode2.  Handles the common
         *       case (non-extreme |d|, precision ≤ 14) in ~130 ns.
         *   (2) d2exp with stack buffer (precision fits in 256B) + copy-parse.
         *   (3) d2exp with heap buffer + in-place parse + ownership steal.
         */
        int precision = (ndigits > 0) ? ndigits - 1 : 0;

        if (precision <= 14 && d != 0.0 && isfinite(d)) {
            char fast_buf[16];  /* P+1 ≤ 15 digits */
            int fast_exp;
            double ad = fabs(d);
            if (_pyryu_fast_mode2(ad, precision, fast_buf, &fast_exp)) {
                int dlen = precision + 1;
                /* Strip trailing zeros — Gay's mode-2 convention. */
                while (dlen > 1 && fast_buf[dlen - 1] == '0') dlen--;
                char *out = (char *)PyMem_Malloc((size_t)dlen + 1);
                if (out == NULL) return NULL;
                memcpy(out, fast_buf, (size_t)dlen);
                out[dlen] = '\0';
                *sign = signbit(d) ? 1 : 0;
                *decpt = fast_exp + 1;
                *digits_end = out + dlen;
                out_digits = out;
                break;
            }
            /* Fall through to d2exp on fast-path bail. */
        }

        size_t need = _pyryu_d2exp_bufsize(precision);
        char stack_buf[256];
        if (need <= sizeof(stack_buf)) {
            int len = d2exp_buffered_n(d, (uint32_t)precision, stack_buf);
            if (!parse_ryu_d2exp_output(stack_buf, len, &out_digits, decpt,
                                        sign, digits_end))
                return NULL;
        }
        else {
            char *buf = (char *)PyMem_Malloc(need);
            if (buf == NULL)
                return NULL;
            int len = d2exp_buffered_n(d, (uint32_t)precision, buf);
            if (!parse_ryu_d2exp_inplace(buf, len, &out_digits, decpt,
                                         sign, digits_end)) {
                PyMem_Free(buf);
                return NULL;
            }
        }
        break;
    }
    case 3: {
        /* ndigits digits after the decimal point (fixed-point format).
         * ndigits < 0 means round to the nearest multiple of 10^(-ndigits),
         * used by float.__round__ with a negative argument.
         *
         * Fast path: for typical precision (fits in 768B — enough for every
         * double's integer part plus ≲ 450 fractional digits), use a stack
         * buffer + copy-parse.  Slow path: heap + in-place parse + steal.
         */
        if (ndigits < 0) {
            if (!ryu_mode3_neg(d, -ndigits, &out_digits, decpt, sign,
                               digits_end))
                return NULL;
            break;
        }
        size_t need = _pyryu_d2fixed_bufsize(ndigits);
        char stack_buf[768];
        if (need <= sizeof(stack_buf)) {
            int len = d2fixed_buffered_n(d, (uint32_t)ndigits, stack_buf);
            if (!parse_ryu_d2fixed_output(stack_buf, len, &out_digits,
                                          decpt, sign, digits_end))
                return NULL;
            break;
        }
        char *buf = (char *)PyMem_Malloc(need);
        if (buf == NULL)
            return NULL;
        int len = d2fixed_buffered_n(d, (uint32_t)ndigits, buf);
        if (!parse_ryu_d2fixed_inplace(buf, len, &out_digits, decpt, sign,
                                       digits_end)) {
            PyMem_Free(buf);
            return NULL;
        }
        break;
    }
    default:
        /* Unsupported mode — should not be reached */
        assert(0 && "_PyRyu_dtoa called with unsupported mode");
        return NULL;
    }

    return out_digits;
}

#endif /* PYSTRTOD_RYU_H */
