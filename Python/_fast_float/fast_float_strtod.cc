// Bridge between fast_float's from_chars and the _Py_dg_strtod calling
// convention used by Python/pystrtod.c.
//
// _Py_dg_strtod's contract (from the David Gay dtoa in the old Python/dtoa.c):
//
//   double _Py_dg_strtod(const char *nptr, char **endptr);
//
//   * Does NOT skip leading whitespace (standard strtod does; dtoa deliberately
//     doesn't, and PyOS_string_to_double's contract relies on that).
//   * Consumes an optional sign, a decimal mantissa (optional '.'), and an
//     optional 'e'/'E' exponent.
//   * Does NOT accept infinity or NaN; pystrtod.c's _Py_parse_inf_or_nan
//     handles those when we decline to consume.
//   * On success *endptr points past the last consumed character.
//   * On parse failure *endptr == nptr, returns 0.
//   * On overflow returns +/-HUGE_VAL and sets errno = ERANGE.
//   * On underflow returns the nearest representable value (possibly 0) and
//     sets errno = ERANGE.
//
// fast_float is a nearly-drop-in fit. Its `from_chars` takes a slice (first,
// last), parses as much as it can, returns `{ptr, ec}` with ptr pointing past
// the consumed region. ec maps cleanly to strtod semantics: `std::errc()` on
// success, `std::errc::invalid_argument` on parse failure, and
// `std::errc::result_out_of_range` on overflow/underflow. Setting `no_infnan`
// in the chars_format rejects "inf"/"nan" literals so pystrtod.c's fallback
// keeps owning those.

#include "fast_float.h"

#include <Python.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <system_error>

extern "C" double
_Py_fast_float_strtod(const char *nptr, char **endptr)
{
    const char *last = nptr + std::strlen(nptr);
    double value = 0.0;

    // fast_float's `general` default rejects leading '+' and "inf"/"nan".
    // _Py_dg_strtod accepted leading '+' (C strtod does too) and left
    // "inf"/"nan" to pystrtod.c's _Py_parse_inf_or_nan fallback — match both.
    auto result = fast_float::from_chars(
        nptr, last, value,
        fast_float::chars_format::general
        | fast_float::chars_format::allow_leading_plus
        | fast_float::chars_format::no_infnan);

    if (result.ec == std::errc::invalid_argument) {
        // No numeric prefix. Caller retries via _Py_parse_inf_or_nan.
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    if (endptr) *endptr = (char *)result.ptr;

    if (result.ec == std::errc::result_out_of_range) {
        // Overflow -> value is +/-inf. Underflow -> value is 0 (or a
        // subnormal near 0). Either way, strtod convention is
        // errno = ERANGE.
        errno = ERANGE;
    }
    return value;
}
