#ifndef Py_INTERNAL_MODSUPPORT_H
#define Py_INTERNAL_MODSUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif


extern int _PyArg_NoKwnames(const char *funcname, PyObject *kwnames);
#define _PyArg_NoKwnames(funcname, kwnames) \
    ((kwnames) == NULL || _PyArg_NoKwnames((funcname), (kwnames)))

// Export for '_bz2' shared extension
PyAPI_FUNC(int) _PyArg_NoPositional(const char *funcname, PyObject *args);
#define _PyArg_NoPositional(funcname, args) \
    ((args) == NULL || _PyArg_NoPositional((funcname), (args)))

// Export for '_asyncio' shared extension
PyAPI_FUNC(int) _PyArg_NoKeywords(const char *funcname, PyObject *kwargs);
#define _PyArg_NoKeywords(funcname, kwargs) \
    ((kwargs) == NULL || _PyArg_NoKeywords((funcname), (kwargs)))

// Export for 'zlib' shared extension
PyAPI_FUNC(int) _PyArg_CheckPositional(const char *, Py_ssize_t,
                                       Py_ssize_t, Py_ssize_t);
#define _PyArg_CheckPositional(funcname, nargs, min, max) \
    (((min) <= (nargs) && (nargs) <= (max)) \
     || _PyArg_CheckPositional((funcname), (nargs), (min), (max)))

extern PyObject ** _Py_VaBuildStack(
    PyObject **small_stack,
    Py_ssize_t small_stack_len,
    const char *format,
    va_list va,
    Py_ssize_t *p_nargs);

extern PyObject* _PyModule_CreateInitialized(PyModuleDef*, int apiver);

// Export for '_curses' shared extension
PyAPI_FUNC(int) _PyArg_ParseStack(
    PyObject *const *args,
    Py_ssize_t nargs,
    const char *format,
    ...);

extern int _PyArg_UnpackStack(
    PyObject *const *args,
    Py_ssize_t nargs,
    const char *name,
    Py_ssize_t min,
    Py_ssize_t max,
    ...);

// Export for '_heapq' shared extension
PyAPI_FUNC(void) _PyArg_BadArgument(
    const char *fname,
    const char *displayname,
    const char *expected,
    PyObject *arg);

// --- _PyArg_Parser API ---------------------------------------------------

// Export for '_dbm' shared extension
PyAPI_FUNC(int) _PyArg_ParseStackAndKeywords(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames,
    struct _PyArg_Parser *,
    ...);

// Export for 'math' shared extension
PyAPI_FUNC(PyObject * const *) _PyArg_UnpackKeywords(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwargs,
    PyObject *kwnames,
    struct _PyArg_Parser *parser,
    int minpos,
    int maxpos,
    int minkw,
    int varpos,
    PyObject **buf);
// Inline fast path for _PyArg_UnpackKeywords().  minpos, maxpos, minkw
// and varpos are integer literals in Argument Clinic generated code, so
// the predicate folds at compile time.  Read the macro below as:
//
//     if (minkw != 0 || kwargs != NULL || kwnames != NULL)
//         goto slow;
//     if (varpos) {
//         if (nargs < minpos) goto slow;
//     }
//     else if (minpos == maxpos) {           // all-required: a single
//         if (nargs != maxpos) goto slow;    // 'nargs == maxpos' test
//     }
//     else if (nargs < minpos || nargs > maxpos) {
//         goto slow;
//     }
//     if (minpos < 1 && args == NULL) goto slow;
//     return args;                /* fast path */
//   slow:
//     return _PyArg_UnpackKeywords(args, nargs, ...);   /* the function */
//
// The 'minpos == maxpos' branch lets an all-required function reduce to
// a single 'nargs == maxpos' test instead of the pair
// 'minpos <= nargs && nargs <= maxpos' (which the compiler does not
// always fold by itself -- notably under -flto).  The 'args != NULL'
// guard is dropped when a positional argument is mandatory: on the fast
// path nargs is then >= 1, and a non-empty vectorcall argument array is
// never NULL.
#define _PyArg_UnpackKeywords(args, nargs, kwargs, kwnames, parser, minpos, maxpos, minkw, varpos, buf) \
    (((minkw) == 0 && (kwargs) == NULL && (kwnames) == NULL \
      && ((varpos) ? (minpos) <= (nargs) \
          : (minpos) == (maxpos) ? (nargs) == (maxpos) \
          : ((minpos) <= (nargs) && (nargs) <= (maxpos))) \
      && ((minpos) >= 1 || (args) != NULL)) ? \
      (args) : \
     _PyArg_UnpackKeywords((args), (nargs), (kwargs), (kwnames), (parser), \
                           (minpos), (maxpos), (minkw), (varpos), (buf)))

#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_MODSUPPORT_H

