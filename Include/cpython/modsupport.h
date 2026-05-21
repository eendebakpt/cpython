#ifndef Py_CPYTHON_MODSUPPORT_H
#  error "this header file must not be included directly"
#endif

PyAPI_FUNC(int) PyArg_ParseArray(
    PyObject *const *args,
    Py_ssize_t nargs,
    const char *format,
    ...);
PyAPI_FUNC(int) PyArg_ParseArrayAndKeywords(
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames,
    const char *format,
    const char * const *kwlist,
    ...);

// A data structure that can be used to run initialization code once in a
// thread-safe manner. The C++11 equivalent is std::call_once.
typedef struct {
    uint8_t v;
} _PyOnceFlag;

/* Optional companion to _PyArg_Parser, used only by parsers that consume
   a format string (the _PyArg_ParseStackAndKeywords / _PyArg_ParseTuple-
   AndKeywordsFast family).  Argument Clinic wrappers that go through
   _PyArg_UnpackKeywords do not need one and leave _PyArg_Parser.ext NULL. */
typedef struct _PyArg_ParserExt {
    const char *format;
    /* The following are derived from `format` on first call and cached. */
    const char *custom_msg;
    int min;                /* minimal number of arguments */
    int max;                /* maximal number of positional arguments */
} _PyArg_ParserExt;

typedef struct _PyArg_Parser {
    const char * const *keywords;
    const char *fname;
    _PyOnceFlag once;       /* atomic one-time initialization flag */
    int is_kwtuple_owned;   /* does this parser own the kwtuple object? */
    int pos;                /* number of positional-only arguments */
    PyObject *kwtuple;      /* tuple of keyword parameter names */
    struct _PyArg_Parser *next;
    struct _PyArg_ParserExt *ext;  /* NULL for kwarg-only (clinic) parsers */
} _PyArg_Parser;

PyAPI_FUNC(int) _PyArg_ParseTupleAndKeywordsFast(PyObject *, PyObject *,
                                                 struct _PyArg_Parser *, ...);

#ifdef Py_BUILD_CORE
// For internal use in stdlib. Needs C99 compound literals.
// Defined here to avoid every stdlib module including pycore_modsupport.h
#define _Py_ABI_SLOT {Py_mod_abi, (void*) &(PyABIInfo) _PyABIInfo_DEFAULT}
#endif
