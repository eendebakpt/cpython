/* C accelerator for copy.deepcopy()

   Atomic types and exact lists, tuples and dicts are copied here; everything
   else is delegated to copy._deepcopy_fallback(), the pure Python
   implementation, which calls back into this module for nested objects. */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "pycore_critical_section.h"  // Py_BEGIN_CRITICAL_SECTION()
#include "pycore_dict.h"              // _PyDict_GetItemRef_KnownHash()
#include "pycore_moduleobject.h"      // _PyModule_GetState()
#include "pycore_object.h"            // _PyNone_Type, _PyNotImplemented_Type

#include "clinic/_copy.c.h"

/*[clinic input]
module _copy
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=b34c1b75f49dbfff]*/

typedef struct {
    PyObject *copy_module;  // the Python 'copy' module
    PyObject *str_fallback; // interned "_deepcopy_fallback"
} copy_state;

static inline copy_state *
get_copy_state(PyObject *module)
{
    void *state = _PyModule_GetState(module);
    assert(state != NULL);
    return (copy_state *)state;
}

static PyObject *do_deepcopy(PyObject *module, PyObject *x, PyObject *memo);

static PyObject *
deepcopy_fallback(PyObject *module, PyObject *x, PyObject *memo)
{
    copy_state *state = get_copy_state(module);
    PyObject *args[] = {state->copy_module, x, memo};
    return PyObject_VectorcallMethod(state->str_fallback, args, 3, NULL);
}

/* Same types as copy._atomic_types. */
static inline int
is_atomic(PyObject *x)
{
    PyTypeObject *tp = Py_TYPE(x);
    return (tp == &PyUnicode_Type
            || tp == &PyLong_Type
            || tp == &_PyNone_Type
            || tp == &PyFloat_Type
            || tp == &PyBool_Type
            || tp == &PyBytes_Type
            || tp == &PyType_Type
            || tp == &PyComplex_Type
            || tp == &PyFunction_Type
            || tp == &PyCFunction_Type
            || tp == &PyCode_Type
            || tp == &PyRange_Type
            || tp == &_PyWeakref_RefType
            || tp == &PyProperty_Type
            || tp == &PyEllipsis_Type
            || tp == &_PyNotImplemented_Type);
}

/* Same as copy._keep_alive(): memo[id(memo)].append(x) */
static int
memo_keep_alive(PyObject *memo, PyObject *x)
{
    PyObject *key = PyLong_FromVoidPtr(memo);
    if (key == NULL) {
        return -1;
    }
    PyObject *list;
    int rc = PyDict_GetItemRef(memo, key, &list);
    if (rc == 0) {
        list = PyList_New(0);
        if (list == NULL || PyDict_SetItem(memo, key, list) < 0) {
            Py_XDECREF(list);
            rc = -1;
        }
    }
    Py_DECREF(key);
    if (rc < 0) {
        return -1;
    }
    rc = PyList_Append(list, x);
    Py_DECREF(list);
    return rc;
}

static PyObject *
deepcopy_list(PyObject *module, PyObject *x, PyObject *memo,
              PyObject *key, Py_hash_t hash)
{
    /* Start from a shallow copy and replace the items in place. The copy is a
       valid list at all times, so it can be put in the memo before its items
       are copied (needed for self-referencing lists), and mutation of x by a
       user __deepcopy__() method cannot confuse the loop. */
    PyObject *y = PyList_GetSlice(x, 0, PY_SSIZE_T_MAX);
    if (y == NULL) {
        return NULL;
    }
    if (_PyDict_SetItem_KnownHash(memo, key, y, hash) < 0) {
        goto error;
    }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(y); i++) {
        PyObject *item = PyList_GetItemRef(y, i);
        if (item == NULL) {
            goto error;
        }
        PyObject *copy = do_deepcopy(module, item, memo);
        Py_DECREF(item);
        if (copy == NULL || PyList_SetItem(y, i, copy) < 0) {
            goto error;
        }
    }
    return y;

error:
    Py_DECREF(y);
    return NULL;
}

/* Like PyDict_Next(), but return strong references and, like a dict
   iterator, fail if the size of the dict changed during the iteration.
   Return 1 if an item was fetched, 0 when exhausted and -1 on error. */
static int
dict_next_ref(PyObject *d, Py_ssize_t *pos, Py_ssize_t size,
              PyObject **key, PyObject **value)
{
    int rc;
    Py_BEGIN_CRITICAL_SECTION(d);
    if (PyDict_GET_SIZE(d) != size) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        rc = -1;
    }
    else {
        rc = PyDict_Next(d, pos, key, value);
        if (rc) {
            Py_INCREF(*key);
            Py_INCREF(*value);
        }
    }
    Py_END_CRITICAL_SECTION();
    return rc;
}

static PyObject *
deepcopy_dict(PyObject *module, PyObject *x, PyObject *memo,
              PyObject *key, Py_hash_t hash)
{
    Py_ssize_t size = PyDict_Size(x);
    PyObject *y = _PyDict_NewPresized(size);
    if (y == NULL) {
        return NULL;
    }
    if (_PyDict_SetItem_KnownHash(memo, key, y, hash) < 0) {
        goto error;
    }
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    int rc;
    while ((rc = dict_next_ref(x, &pos, size, &k, &v)) > 0) {
        PyObject *kc = do_deepcopy(module, k, memo);
        Py_DECREF(k);
        if (kc == NULL) {
            Py_DECREF(v);
            goto error;
        }
        PyObject *vc = do_deepcopy(module, v, memo);
        Py_DECREF(v);
        if (vc == NULL) {
            Py_DECREF(kc);
            goto error;
        }
        rc = PyDict_SetItem(y, kc, vc);
        Py_DECREF(kc);
        Py_DECREF(vc);
        if (rc < 0) {
            goto error;
        }
    }
    if (rc < 0) {
        goto error;
    }
    return y;

error:
    Py_DECREF(y);
    return NULL;
}

static PyObject *
deepcopy_tuple(PyObject *module, PyObject *x, PyObject *memo,
               PyObject *key, Py_hash_t hash)
{
    Py_ssize_t size = PyTuple_GET_SIZE(x);
    PyObject *y = PyTuple_New(size);
    if (y == NULL) {
        return NULL;
    }
    /* Unlike a list or dict, the copy cannot be put in the memo before it is
       complete: a tuple must never be observed changing. */
    int all_identical = 1;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyTuple_GET_ITEM(x, i);
        PyObject *copy = do_deepcopy(module, item, memo);
        if (copy == NULL) {
            Py_DECREF(y);
            return NULL;
        }
        if (copy != item) {
            all_identical = 0;
        }
        PyTuple_SET_ITEM(y, i, copy);
    }
    if (all_identical) {
        /* A tuple whose items are their own deep copies is its own deep
           copy and, like in the Python implementation, is not memoized. */
        Py_SETREF(y, Py_NewRef(x));
        return y;
    }
    /* If x is reachable from one of its own items, copying that item has
       already produced a copy of x; return it so that all references end
       up pointing to the same copy. */
    PyObject *z;
    if (_PyDict_GetItemRef_KnownHash((PyDictObject *)memo, key, hash, &z) != 0) {
        Py_DECREF(y);
        return z;  // NULL on error
    }
    if (_PyDict_SetItem_KnownHash(memo, key, y, hash) < 0) {
        Py_DECREF(y);
        return NULL;
    }
    return y;
}

static PyObject *
do_deepcopy(PyObject *module, PyObject *x, PyObject *memo)
{
    assert(PyDict_CheckExact(memo));

    if (is_atomic(x)) {
        return Py_NewRef(x);
    }

    PyObject *key = PyLong_FromVoidPtr(x);  // id(x)
    if (key == NULL) {
        return NULL;
    }
    Py_hash_t hash = PyObject_Hash(key);
    assert(hash != -1);

    PyObject *y;
    if (_PyDict_GetItemRef_KnownHash((PyDictObject *)memo, key, hash, &y) != 0) {
        Py_DECREF(key);
        return y;  // already copied, or NULL on error
    }

    PyTypeObject *tp = Py_TYPE(x);
    if (tp != &PyList_Type && tp != &PyDict_Type && tp != &PyTuple_Type) {
        Py_DECREF(key);
        if (PyType_Check(x)) {
            return Py_NewRef(x);  // classes are copied atomically
        }
        return deepcopy_fallback(module, x, memo);
    }

    if (Py_EnterRecursiveCall(" in deepcopy")) {
        Py_DECREF(key);
        return NULL;
    }
    if (tp == &PyList_Type) {
        y = deepcopy_list(module, x, memo, key, hash);
    }
    else if (tp == &PyDict_Type) {
        y = deepcopy_dict(module, x, memo, key, hash);
    }
    else {
        y = deepcopy_tuple(module, x, memo, key, hash);
    }
    Py_LeaveRecursiveCall();
    Py_DECREF(key);

    if (y != NULL && y != x && memo_keep_alive(memo, x) < 0) {
        Py_CLEAR(y);
    }
    return y;
}

/*[clinic input]
_copy.deepcopy

    x: object
    memo: object = None

Deep copy operation on arbitrary Python objects.

See the module's __doc__ string for more info.
[clinic start generated code]*/

static PyObject *
_copy_deepcopy_impl(PyObject *module, PyObject *x, PyObject *memo)
/*[clinic end generated code: output=825a9c8dd4bfc002 input=8a669d85dca14f27]*/
{
    if (is_atomic(x)) {
        return Py_NewRef(x);
    }
    if (memo == Py_None) {
        memo = PyDict_New();
        if (memo == NULL) {
            return NULL;
        }
        PyObject *y = do_deepcopy(module, x, memo);
        Py_DECREF(memo);
        return y;
    }
    if (!PyDict_CheckExact(memo)) {
        // The Python implementation supports any mapping as memo.
        return deepcopy_fallback(module, x, memo);
    }
    return do_deepcopy(module, x, memo);
}

static PyMethodDef copy_methods[] = {
    _COPY_DEEPCOPY_METHODDEF
    {NULL, NULL}
};

static int
copy_exec(PyObject *module)
{
    copy_state *state = get_copy_state(module);
    state->str_fallback = PyUnicode_InternFromString("_deepcopy_fallback");
    if (state->str_fallback == NULL) {
        return -1;
    }
    // The copy module is normally the importer of this module; in that case
    // the partially initialized copy module is returned here.
    state->copy_module = PyImport_ImportModule("copy");
    if (state->copy_module == NULL) {
        return -1;
    }
    return 0;
}

static int
copy_traverse(PyObject *module, visitproc visit, void *arg)
{
    copy_state *state = get_copy_state(module);
    Py_VISIT(state->copy_module);
    return 0;
}

static int
copy_clear(PyObject *module)
{
    copy_state *state = get_copy_state(module);
    Py_CLEAR(state->copy_module);
    Py_CLEAR(state->str_fallback);
    return 0;
}

static void
copy_free(void *module)
{
    (void)copy_clear((PyObject *)module);
}

static PyModuleDef_Slot copy_slots[] = {
    {Py_mod_exec, copy_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef copy_module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_copy",
    .m_doc = PyDoc_STR("C implementation of copy.deepcopy()"),
    .m_size = sizeof(copy_state),
    .m_methods = copy_methods,
    .m_slots = copy_slots,
    .m_traverse = copy_traverse,
    .m_clear = copy_clear,
    .m_free = copy_free,
};

PyMODINIT_FUNC
PyInit__copy(void)
{
    return PyModuleDef_Init(&copy_module);
}
