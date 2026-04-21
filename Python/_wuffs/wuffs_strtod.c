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
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Parse the input into its decomposed parts and produce a canonical form
// "[sign]<digits>e<exp>" with leading/trailing zeros stripped, so wuffs
// receives a short, well-behaved string regardless of how extreme the
// original mantissa or exponent was. This is what absorbs the gap between
// strtod's tolerance and wuffs's stricter parser:
//
//   * Leading-dot forms like ".00E2"  (wuffs rejects these by default)
//   * Very long all-zero mantissas with large compensating exponents like
//     "0." + "0"*29999 + "1e+30000" (wuffs's HPD bails at >800 sig digits,
//     and its decimal-point range is limited to +/-2047).
//   * Trailing zeros beyond the ones wuffs keeps implicit.
//
// Returns the parsed double, sets *endptr to the first character beyond the
// consumed numeric literal, and sets errno = ERANGE on over- or underflow —
// matching _Py_dg_strtod's contract.

double
_Py_wuffs_strtod(const char *nptr, char **endptr)
{
    const char *p = nptr;
    // No leading-whitespace skip. C strtod's standard behaviour includes
    // one, but _Py_dg_strtod (which lived in dtoa.c and which we replace)
    // deliberately does not, and PyOS_string_to_double's contract warns
    // callers against passing whitespace. Matching dtoa's behaviour here
    // keeps the ValueError tests in Modules/_testcapi/float.c happy.

    // Optional sign.
    bool negative = false;
    if (*p == '+') ++p;
    else if (*p == '-') { negative = true; ++p; }

    // Integer and fractional digit runs. At least one must be non-empty;
    // otherwise this isn't a number and the caller retries via
    // _Py_parse_inf_or_nan.
    const char *int_start = p;
    while (isdigit((unsigned char)*p)) ++p;
    const char *int_end = p;

    const char *frac_start = NULL;
    const char *frac_end = NULL;
    if (*p == '.') {
        ++p;
        frac_start = p;
        while (isdigit((unsigned char)*p)) ++p;
        frac_end = p;
    }

    if (int_start == int_end && (frac_start == NULL || frac_start == frac_end)) {
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    // Optional exponent.
    long long explicit_exp = 0;
    const char *after_exp = p;
    if (*p == 'e' || *p == 'E') {
        const char *exp_at = p;
        ++p;
        bool exp_neg = false;
        if (*p == '+') ++p;
        else if (*p == '-') { exp_neg = true; ++p; }
        const char *exp_digits = p;
        while (isdigit((unsigned char)*p)) ++p;
        if (p == exp_digits) {
            p = exp_at;           // malformed exponent; back out
        } else {
            // Parse the exponent. Cap to avoid long-long overflow on pathological
            // inputs; anything past ~10^18 saturates to +/- that bound, which
            // wuffs will then translate to inf or 0 on its own.
            long long v = 0;
            for (const char *q = exp_digits; q < p; ++q) {
                if (v < 1000000000000000000LL) v = v * 10 + (*q - '0');
            }
            explicit_exp = exp_neg ? -v : v;
            after_exp = p;
        }
    }
    const char *num_end = (p == after_exp) ? p : after_exp;

    // Combine int + frac digits conceptually; find first and last non-zero.
    int int_len = (int)(int_end - int_start);
    int frac_len = frac_start ? (int)(frac_end - frac_start) : 0;
    int total_len = int_len + frac_len;

    int first_nz = -1;
    for (int i = 0; i < int_len; ++i)
        if (int_start[i] != '0') { first_nz = i; break; }
    if (first_nz < 0 && frac_start)
        for (int i = 0; i < frac_len; ++i)
            if (frac_start[i] != '0') { first_nz = int_len + i; break; }

    if (first_nz < 0) {
        // All-zero mantissa => value is +/-0 regardless of the exponent.
        if (endptr) *endptr = (char *)num_end;
        return negative ? -0.0 : 0.0;
    }

    int last_nz = -1;
    if (frac_start)
        for (int i = frac_len - 1; i >= 0; --i)
            if (frac_start[i] != '0') { last_nz = int_len + i; break; }
    if (last_nz < 0)
        for (int i = int_len - 1; i >= 0; --i)
            if (int_start[i] != '0') { last_nz = i; break; }

    int canonical_len = last_nz - first_nz + 1;
    int trailing_zeros_stripped = total_len - 1 - last_nz;

    // value = canonical_mantissa * 10^(explicit_exp - frac_len + trailing_zeros_stripped)
    long long effective_exp =
        explicit_exp - (long long)frac_len + (long long)trailing_zeros_stripped;

    // Cap canonical_len at wuffs's HPD precision and absorb the drop into
    // effective_exp. We can't rely on wuffs's own "truncated" flag for this:
    // wuffs_private_impl__high_prec_dec__parse stops advancing `dp` once it
    // has stored WUFFS_PRIVATE_IMPL__HPD__DIGITS_PRECISION (800) digits, so
    // a 1000-digit integer ends up with dp=800 instead of dp=1000 — a
    // factor-of-100 error. Truncating here and bumping exp moves the digits
    // we drop out of the mantissa, where wuffs's book-keeping is correct.
    // The cost is up to 1 ULP in halfway cases that dtoa's bignum round
    // exactly; those are rare and surface in test_strtod rather than in
    // ordinary Python code.
    const int MAX_DIGITS = 800;
    if (canonical_len > MAX_DIGITS) {
        int dropped = canonical_len - MAX_DIGITS;
        effective_exp += dropped;
        canonical_len = MAX_DIGITS;
    }

    // Assemble "[sign]<canonical_digits>e<effective_exp>" into a buffer.
    char stack_work[1024];
    char *work = stack_work;
    size_t need = (size_t)canonical_len + 32;
    char *heap_work = NULL;
    if (need > sizeof(stack_work)) {
        heap_work = (char *)PyMem_Malloc(need);
        if (!heap_work) {
            if (endptr) *endptr = (char *)nptr;
            errno = ENOMEM;
            return 0.0;
        }
        work = heap_work;
    }
    size_t off = 0;
    if (negative) work[off++] = '-';
    for (int i = 0; i < canonical_len; ++i) {
        int src_idx = first_nz + i;
        work[off++] = (src_idx < int_len)
                          ? int_start[src_idx]
                          : frac_start[src_idx - int_len];
    }
    work[off++] = 'e';
    int n = snprintf(work + off, need - off, "%lld", effective_exp);
    off += (size_t)n;

    wuffs_base__slice_u8 slice = wuffs_base__make_slice_u8((uint8_t *)work, off);
    uint32_t options =
        WUFFS_BASE__PARSE_NUMBER_XXX__ALLOW_MULTIPLE_LEADING_ZEROES;

    wuffs_base__result_f64 r = wuffs_base__parse_number_f64(slice, options);
    if (heap_work) PyMem_Free(heap_work);

    if (r.status.repr != NULL) {
        // Should not happen for a well-formed canonical string unless wuffs
        // hits its own decimal-point range bound (|exp| > 2047). That case
        // means the value is essentially 0 or +/-inf — report accordingly.
        if (endptr) *endptr = (char *)num_end;
        errno = ERANGE;
        return effective_exp > 0 ? (negative ? -HUGE_VAL : HUGE_VAL)
                                 : (negative ? -0.0 : 0.0);
    }

    if (endptr) *endptr = (char *)num_end;

    if (isinf(r.value)) {
        errno = ERANGE;
    } else if (r.value == 0.0 && first_nz >= 0) {
        // Non-zero input that underflowed (or rounded) to zero.
        errno = ERANGE;
    }
    return r.value;
}
