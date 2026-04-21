#ifndef Py_INTERNAL_DTOA_H
#define Py_INTERNAL_DTOA_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_pymath.h"        // _PY_SHORT_FLOAT_REPR


// Float <-> string conversion entry points for libpython. Both replace
// David Gay's dtoa machinery that previously lived in Python/dtoa.c:
//
//   * _Py_fmt_dtoa is backed by a vendored trim of fmtlib ({fmt}) in
//     Python/_fmt/. Mirrors _Py_dg_dtoa's calling convention for modes
//     0/2/3. The returned char* is PyMem_Malloc'd — pair each call with
//     _Py_fmt_dtoa_free.
//
//   * _Py_fast_float_strtod is backed by a vendored drop of fast_float
//     in Python/_fast_float/. Mirrors _Py_dg_strtod's calling convention.
extern char* _Py_fmt_dtoa(double d, int mode, int ndigits,
                          int *decpt, int *sign, char **rve);
extern void _Py_fmt_dtoa_free(char *s);
extern double _Py_fast_float_strtod(const char *nptr, char **endptr);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_DTOA_H */
