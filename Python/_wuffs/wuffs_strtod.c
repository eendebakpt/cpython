// Bridge between Wuffs's wuffs_base__parse_number_f64 and the _Py_dg_strtod
// calling convention used by Python/pystrtod.c.
//
// _Py_dg_strtod's contract (from the David Gay dtoa that used to live in
// Python/dtoa.c):
//
//   double _Py_dg_strtod(const char *nptr, char **endptr);
//
//   * Skips leading whitespace (like the C standard strtod).
//   * Consumes an optional sign, a decimal mantissa (with optional '.'),
//     and an optional 'e'/'E' exponent.
//   * Does NOT accept infinities or NaNs — the caller in pystrtod.c falls
//     back to _Py_parse_inf_or_nan when we don't consume anything.
//   * On success *endptr points past the last consumed character.
//   * On parse failure *endptr == nptr, returns 0.
//   * On overflow returns +/-HUGE_VAL and sets errno = ERANGE.
//   * On underflow returns the nearest representable value (possibly 0) and
//     sets errno = ERANGE.
//
// Wuffs is stricter: wuffs_base__parse_number_f64 requires the whole slice
// to be consumed, returns an in-band status, and does not touch errno. So
// the shim has to:
//
//   (1) scan the ASCII tail itself to find where the numeric syntax ends,
//   (2) hand wuffs just that slice,
//   (3) translate wuffs's result (including inf-on-overflow) back into the
//       strtod errno discipline.

// Compile only the base/floatconv path of Wuffs into this TU. With these
// defines set (before the include), the preprocessor prunes every other
// module (image codecs, JSON, compression, ...), cutting a ~3.3 MB source
// down to ~80-100 KB of object code. `WUFFS_IMPLEMENTATION` activates
// function definitions alongside declarations.
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__BASE__CORE
#define WUFFS_CONFIG__MODULE__BASE__FLOATCONV
#define WUFFS_CONFIG__MODULE__BASE__INTCONV
#include "wuffs-v0.4.c"

#include <Python.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

// Scan forward from `p` and return the first character that isn't part of a
// valid strtod-style numeric literal (after the sign we've already stepped
// past). Returns `p` itself when no digits were found — the caller uses that
// to signal "parse failure, don't consume".
static const char *
scan_number_end(const char *p)
{
    const char *start = p;
    int have_int = 0, have_frac = 0;
    while (isdigit((unsigned char)*p)) { ++p; have_int = 1; }
    if (*p == '.') {
        ++p;
        while (isdigit((unsigned char)*p)) { ++p; have_frac = 1; }
    }
    if (!have_int && !have_frac) return start;  // no digits at all
    if (*p == 'e' || *p == 'E') {
        const char *exp_at = p;
        ++p;
        if (*p == '+' || *p == '-') ++p;
        int have_exp_digits = 0;
        while (isdigit((unsigned char)*p)) { ++p; have_exp_digits = 1; }
        if (!have_exp_digits) p = exp_at;   // malformed exponent; back out
    }
    return p;
}

double
_Py_wuffs_strtod(const char *nptr, char **endptr)
{
    const char *p = nptr;

    // Leading whitespace (strtod semantics).
    while (isspace((unsigned char)*p)) ++p;

    const char *sign_start = p;
    if (*p == '+' || *p == '-') ++p;

    const char *digits_start = p;
    const char *digits_end = scan_number_end(p);

    if (digits_end == digits_start) {
        // No numeric content. Caller (pystrtod.c) will then try
        // _Py_parse_inf_or_nan.
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    // Hand wuffs the [sign_start, digits_end) slice. We include the sign so
    // wuffs handles +/- consistently with strtod. Wuffs rejects leading
    // zeros by default (e.g. "00.7"), so opt in to ALLOW_MULTIPLE_LEADING_ZEROES.
    // REJECT_INF_AND_NAN mirrors _Py_dg_strtod — pystrtod.c's
    // _Py_parse_inf_or_nan handles those separately.
    wuffs_base__slice_u8 slice = wuffs_base__make_slice_u8(
        (uint8_t *)sign_start, (size_t)(digits_end - sign_start));
    uint32_t options =
        WUFFS_BASE__PARSE_NUMBER_XXX__ALLOW_MULTIPLE_LEADING_ZEROES
        | WUFFS_BASE__PARSE_NUMBER_FXX__REJECT_INF_AND_NAN;

    wuffs_base__result_f64 r = wuffs_base__parse_number_f64(slice, options);
    if (r.status.repr != NULL) {
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    if (endptr) *endptr = (char *)digits_end;

    // Overflow: wuffs returns +/-inf silently; strtod convention is
    // HUGE_VAL + errno=ERANGE.
    if (isinf(r.value)) {
        errno = ERANGE;
    }
    // Underflow: parsed value is zero but the numeric substring had at least
    // one non-zero digit.
    else if (r.value == 0.0) {
        for (const char *q = digits_start; q < digits_end; ++q) {
            if (*q >= '1' && *q <= '9') {
                errno = ERANGE;
                break;
            }
        }
    }

    return r.value;
}
