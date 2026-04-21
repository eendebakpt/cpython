// Bridge between {fmt}'s float formatting and the _Py_dg_dtoa calling
// convention used throughout Python/pystrtod.c and Objects/floatobject.c.
//
// Exposes a single extern "C" entry point, _Py_fmt_dtoa, with the exact shape
// of _Py_dg_dtoa (the David Gay dtoa that used to live in Python/dtoa.c) so
// existing call sites can swap with a one-line change.
//
//   mode 0 -> shortest round-trip decimal (uses fmt::detail::dragonbox)
//   mode 2 -> max(1, ndigits) significant digits (uses format_float with
//             presentation_type::exp and precision = N - 1)
//   mode 3 -> ndigits digits past the decimal point, ndigits may be negative
//             (uses format_float with presentation_type::fixed)
//
// Trailing zeros are stripped so the digit string + decpt shape matches
// _Py_dg_dtoa byte-for-byte. The "rounded-to-zero-at-target-precision" case
// for mode 3 is normalised from fmt's "one '0' digit" output to cpython's
// empty-digit + decpt = -ndigits convention.
//
// Memory model: the returned char* is allocated with PyMem_Malloc and must be
// freed via _Py_fmt_dtoa_free (mirrors _Py_dg_freedtoa).

// Build fmt in header-only mode so we don't need a separate compiled
// format.cc TU for the Dragonbox tables and the `write_fixed` locale hooks.
// FMT_OPTIMIZE_SIZE >= 2 disables fmt's std::locale-based locale lookup,
// which cpython's dtoa never uses anyway and which would otherwise drag
// libstdc++'s <locale> into libpython.
#define FMT_HEADER_ONLY 1
#define FMT_OPTIMIZE_SIZE 2

#include "format.h"
#include "format-inl.h"  // brings in out-of-line dragonbox tables; FMT_FUNC
                         // resolves to `inline` in header-only mode.

#include <Python.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {

void _Py_fmt_dtoa_free(char *s) {
    PyMem_Free(s);
}

static char *alloc_copy(const char *src, size_t n) {
    char *out = static_cast<char *>(PyMem_Malloc(n + 1));
    if (!out) return nullptr;
    std::memcpy(out, src, n);
    out[n] = '\0';
    return out;
}

char *_Py_fmt_dtoa(double d, int mode, int ndigits,
                   int *decpt, int *sign, char **rve) {
    // Sign. _Py_dg_dtoa distinguishes -0.0 from +0.0 via *sign.
    *sign = std::signbit(d) ? 1 : 0;
    double abs_d = *sign ? -d : d;

    // Infinity / NaN / zero: cpython conventions, no fmt involvement.
    if (std::isinf(abs_d)) {
        *decpt = 9999;
        char *out = alloc_copy("Infinity", 8);
        if (!out) return nullptr;
        if (rve) *rve = out + 8;
        return out;
    }
    if (std::isnan(abs_d)) {
        *decpt = 9999;
        char *out = alloc_copy("NaN", 3);
        if (!out) return nullptr;
        if (rve) *rve = out + 3;
        return out;
    }
    if (abs_d == 0.0) {
        *decpt = 1;
        char *out = alloc_copy("0", 1);
        if (!out) return nullptr;
        if (rve) *rve = out + 1;
        return out;
    }

    // Mode 0 (shortest) uses Dragonbox directly.
    if (mode == 0) {
        auto r = fmt::detail::dragonbox::to_decimal(abs_d);
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%llu",
                              (unsigned long long)r.significand);
        int exp = r.exponent;
        while (n > 1 && tmp[n - 1] == '0') { --n; ++exp; }
        *decpt = exp + n;
        char *out = alloc_copy(tmp, (size_t)n);
        if (!out) return nullptr;
        if (rve) *rve = out + n;
        return out;
    }

    // Modes 2 and 3: fmt::detail::format_float writes digits (no dot, no
    // sign, no exponent) into a buffer and returns the decimal exponent.
    fmt::format_specs specs;
    int precision;
    bool fixed;
    if (mode == 2) {
        specs.set_type(fmt::presentation_type::exp);
        // max(1, ndigits) significant digits, fmt wants precision = N - 1.
        precision = (ndigits < 1 ? 1 : ndigits) - 1;
        fixed = false;
    } else {
        // Treat any unrecognised mode as 3 (matches _Py_dg_dtoa's fallthrough).
        specs.set_type(fmt::presentation_type::fixed);
        precision = ndigits;
        fixed = true;
    }

    fmt::memory_buffer buf;
    int exp = fmt::detail::format_float(abs_d, precision, specs,
                                        /*binary32=*/false, buf);
    const char *bd = buf.data();
    int n = static_cast<int>(buf.size());

    // Normalise fmt's rounded-to-zero output to cpython's "no_digits" shape:
    // fmt may emit a single '0' (e.g. round(5, -1) == 0); cpython returns an
    // empty digit string with decpt = -ndigits.
    if (fixed && n == 1 && bd[0] == '0') {
        char *out = static_cast<char *>(PyMem_Malloc(1));
        if (!out) return nullptr;
        out[0] = '\0';
        *decpt = -ndigits;
        if (rve) *rve = out;
        return out;
    }
    // Similarly handle the fixed case where fmt emitted 0 digits (precision
    // was so negative the whole value rounded away).
    if (fixed && n == 0) {
        char *out = static_cast<char *>(PyMem_Malloc(1));
        if (!out) return nullptr;
        out[0] = '\0';
        *decpt = -ndigits;
        if (rve) *rve = out;
        return out;
    }

    // Strip trailing zeros (cpython mode-2/3 invariant).
    int nz = n;
    while (nz > 1 && bd[nz - 1] == '0') --nz;
    *decpt = exp + n;                    // decpt uses pre-strip length
    char *out = alloc_copy(bd, (size_t)nz);
    if (!out) return nullptr;
    if (rve) *rve = out + nz;
    return out;
}

}  // extern "C"
