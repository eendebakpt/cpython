/*[clinic input]
preserve
[clinic start generated code]*/

#if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)
#  include "pycore_gc.h"          // PyGC_Head
#  include "pycore_runtime.h"     // _Py_ID()
#endif
#include "pycore_modsupport.h"    // _PyArg_UnpackKeywords()

PyDoc_STRVAR(_copy_deepcopy__doc__,
"deepcopy($module, /, x, memo=None)\n"
"--\n"
"\n"
"Deep copy operation on arbitrary Python objects.\n"
"\n"
"See the module\'s __doc__ string for more info.");

#define _COPY_DEEPCOPY_METHODDEF    \
    {"deepcopy", _PyCFunction_CAST(_copy_deepcopy), METH_FASTCALL|METH_KEYWORDS, _copy_deepcopy__doc__},

static PyObject *
_copy_deepcopy_impl(PyObject *module, PyObject *x, PyObject *memo);

static PyObject *
_copy_deepcopy(PyObject *module, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    #if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)

    #define NUM_KEYWORDS 2
    static struct {
        PyGC_Head _this_is_not_used;
        PyObject_VAR_HEAD
        Py_hash_t ob_hash;
        PyObject *ob_item[NUM_KEYWORDS];
    } _kwtuple = {
        .ob_base = PyVarObject_HEAD_INIT(&PyTuple_Type, NUM_KEYWORDS)
        .ob_hash = -1,
        .ob_item = { _Py_LATIN1_CHR('x'), &_Py_ID(memo), },
    };
    #undef NUM_KEYWORDS
    #define KWTUPLE (&_kwtuple.ob_base.ob_base)

    #else  // !Py_BUILD_CORE
    #  define KWTUPLE NULL
    #endif  // !Py_BUILD_CORE

    static const char * const _keywords[] = {"x", "memo", NULL};
    static _PyArg_Parser _parser = {
        .keywords = _keywords,
        .fname = "deepcopy",
        .kwtuple = KWTUPLE,
    };
    #undef KWTUPLE
    PyObject *argsbuf[2];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 1;
    PyObject *x;
    PyObject *memo = Py_None;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser,
            /*minpos*/ 1, /*maxpos*/ 2, /*minkw*/ 0, /*varpos*/ 0, argsbuf);
    if (!args) {
        goto exit;
    }
    x = args[0];
    if (!noptargs) {
        goto skip_optional_pos;
    }
    memo = args[1];
skip_optional_pos:
    return_value = _copy_deepcopy_impl(module, x, memo);

exit:
    return return_value;
}
/*[clinic end generated code: output=1c4ecd6a0791f98c input=a9049054013a1b77]*/
