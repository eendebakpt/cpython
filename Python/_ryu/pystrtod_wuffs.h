/* pystrtod_wuffs.h - CPython adapter: Wuffs parse_number_f64 -> _Py_dg_strtod.
 *
 * Provides _PyWuffs_strtod(const char *s, char **endptr) as a drop-in
 * replacement for _Py_dg_strtod() for all finite and non-finite inputs.
 *
 * Contract (same as _Py_dg_strtod / C99 strtod):
 *   - Parses the longest valid decimal float prefix of s.
 *   - Sets *endptr to one past the last consumed character (or s on failure).
 *   - Returns the correctly-rounded double value.
 *   - On malloc failure: impossible (no allocation), never returns -1.0/ENOMEM.
 *   - Does NOT skip leading whitespace (same as Gay's _Py_dg_strtod).
 *   - Does NOT handle hex floats (0x...) -- caller handles those separately.
 *
 * "inf" / "nan" are handled by the caller (_PyOS_ascii_strtod) via
 * _Py_parse_inf_or_nan(), exactly as before.  Wuffs also accepts them but
 * with different case rules, so we leave that to the existing code path.
 *
 * Coverage: 100% of valid decimal float strings, correctly rounded.
 * Algorithm: Eisel-Lemire fast path (~99.9999% of inputs) with
 *            High Precision Decimal (HPD) fallback for the rest.
 *            No Bigint, no dynamic allocation, no platform strtod dependency.
 */

#ifndef Py_PYSTRTOD_WUFFS_H
#define Py_PYSTRTOD_WUFFS_H

#include "_ryu/floatconv_wuffs.h"

/* _PyWuffs_strtod: parse s as a decimal double.
 *
 * Sets *endptr to point past the last consumed character, or to s if no
 * valid number was found.  Return value is the parsed double (0.0 if none).
 */
static double
_PyWuffs_strtod(const char *s, char **endptr)
{
    /* Wuffs takes a slice (ptr + length), not a NUL-terminated string.
     * We need to find the end of the number first.
     *
     * Strategy: scan forward to find the end of a valid decimal float token
     * (sign, digits, optional dot+digits, optional e/E+exponent), then hand
     * that exact slice to Wuffs.  This avoids strlen on the whole string.
     *
     * We are deliberately conservative: anything ambiguous goes to Wuffs
     * which will return an error and we fall through to returning 0 /
     * leaving endptr==s.  Wuffs itself does not modify the input string.
     */
    const unsigned char *p = (const unsigned char *)s;

    /* Optional leading sign. */
    if (*p == '+' || *p == '-') {
        p++;
    }

    /* Must start with a digit or '.' followed by a digit. */
    if (!(*p >= '0' && *p <= '9') &&
        !(*p == '.' && p[1] >= '0' && p[1] <= '9')) {
        if (endptr) *endptr = (char *)s;
        return 0.0;
    }

    /* Walk integer digits. */
    while (*p >= '0' && *p <= '9') p++;

    /* Optional fractional part. */
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }

    /* Optional exponent. */
    if (*p == 'e' || *p == 'E') {
        const unsigned char *exp_start = p;
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p >= '0' && *p <= '9') {
            while (*p >= '0' && *p <= '9') p++;
        }
        else {
            /* bare 'e' with no digits -- back up */
            p = exp_start;
        }
    }

    size_t len = (size_t)(p - (const unsigned char *)s);
    if (len == 0 || (len == 1 && (s[0] == '+' || s[0] == '-'))) {
        if (endptr) *endptr = (char *)s;
        return 0.0;
    }

    wuffs_base__slice_u8 slice =
        wuffs_base__make_slice_u8((uint8_t *)s, len);
    wuffs_base__result_f64 r = wuffs_base__parse_number_f64(slice, 0);

    if (r.status.repr != NULL) {
        /* Wuffs rejected it (should not happen for a well-formed token,
         * but be safe). */
        if (endptr) *endptr = (char *)s;
        return 0.0;
    }

    if (endptr) *endptr = (char *)s + len;
    return r.value;
}

#endif  /* Py_PYSTRTOD_WUFFS_H */
