#ifndef Py_INTERNAL_PYMATH_H
#define Py_INTERNAL_PYMATH_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif


/* _Py_ADJUST_ERANGE1(x)
 * _Py_ADJUST_ERANGE2(x, y)
 * Set errno to 0 before calling a libm function, and invoke one of these
 * macros after, passing the function result(s) (_Py_ADJUST_ERANGE2 is useful
 * for functions returning complex results).  This makes two kinds of
 * adjustments to errno:  (A) If it looks like the platform libm set
 * errno=ERANGE due to underflow, clear errno. (B) If it looks like the
 * platform libm overflowed but didn't set errno, force errno to ERANGE.  In
 * effect, we're trying to force a useful implementation of C89 errno
 * behavior.
 * Caution:
 *    This isn't reliable.  C99 no longer requires libm to set errno under
 *        any exceptional condition, but does require +- HUGE_VAL return
 *        values on overflow.  A 754 box *probably* maps HUGE_VAL to a
 *        double infinity, and we're cool if that's so, unless the input
 *        was an infinity and an infinity is the expected result.  A C89
 *        system sets errno to ERANGE, so we check for that too.  We're
 *        out of luck if a C99 754 box doesn't map HUGE_VAL to +Inf, or
 *        if the returned result is a NaN, or if a C89 box returns HUGE_VAL
 *        in non-overflow cases.
 */
static inline void _Py_ADJUST_ERANGE1(double x)
{
    if (errno == 0) {
        if (x == INFINITY || x == -INFINITY) {
            errno = ERANGE;
        }
    }
    else if (errno == ERANGE && x == 0.0) {
        errno = 0;
    }
}

static inline void _Py_ADJUST_ERANGE2(double x, double y)
{
    if (x == INFINITY || x == -INFINITY ||
        y == INFINITY || y == -INFINITY)
    {
        if (errno == 0) {
            errno = ERANGE;
        }
    }
    else if (errno == ERANGE) {
        errno = 0;
    }
}


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYMATH_H */
