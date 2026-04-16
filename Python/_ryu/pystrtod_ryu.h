/* pystrtod_ryu.h - Ryu-based adapter replacing _Py_dg_dtoa for modes 0, 2, 3
 *
 * Copyright 2024 Python Software Foundation
 *
 * _PyRyu_dtoa() is a drop-in replacement for _Py_dg_dtoa() for the three
 * modes used by format_float_short() in pystrtod.c:
 *
 *   mode 0  – shortest round-trip string  (repr / str)
 *   mode 2  – N significant digits        (%e, %g)
 *   mode 3  – N digits past decimal point (%f), ndigits >= 0 only
 *
 * The negative-ndigits case of mode 3 (used by float.__round__ with a
 * negative argument) is NOT handled here; callers must use _Py_dg_dtoa for
 * that path.
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
#include "pymem.h"          /* PyMem_Malloc / PyMem_Free */
#include "_ryu/ryu.h"       /* d2s_buffered_n, d2exp_buffered_n,
                               d2fixed_buffered_n */

/* Maximum buffer sizes for Ryu's output:
 *   d2s    : up to 25 chars  (sign + 17 digits + 'E' + sign + 3-digit exp)
 *   d2exp  : up to 2000 chars (for very high precision; Ryu uses 2000 in d2exp)
 *   d2fixed: up to 2000 chars
 * We use a generous stack buffer for d2s and heap for the others.
 */
#define _PYRYU_D2S_BUFSIZE   32
#define _PYRYU_D2EXP_BUFSIZE 2000
#define _PYRYU_D2FIXED_BUFSIZE 2000

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
         * for a total of P+1 significant digits.
         * So we pass precision = ndigits - 1. */
        int precision = (ndigits > 0) ? ndigits - 1 : 0;
        char *buf = (char *)PyMem_Malloc(_PYRYU_D2EXP_BUFSIZE);
        if (buf == NULL)
            return NULL;
        int len = d2exp_buffered_n(d, (uint32_t)precision, buf);
        int ok = parse_ryu_d2exp_output(buf, len, &out_digits, decpt, sign,
                                        digits_end);
        PyMem_Free(buf);
        if (!ok)
            return NULL;
        break;
    }
    case 3: {
        /* ndigits digits after the decimal point (fixed-point format).
         * ndigits must be >= 0 here (negative case uses _Py_dg_dtoa). */
        assert(ndigits >= 0);
        char *buf = (char *)PyMem_Malloc(_PYRYU_D2FIXED_BUFSIZE);
        if (buf == NULL)
            return NULL;
        int len = d2fixed_buffered_n(d, (uint32_t)ndigits, buf);
        int ok = parse_ryu_d2fixed_output(buf, len, &out_digits, decpt, sign,
                                          digits_end);
        PyMem_Free(buf);
        if (!ok)
            return NULL;
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
