/* stringlib: bytes joining implementation */

#if STRINGLIB_IS_UNICODE
#error join.h only compatible with byte-wise strings
#endif

#define NB_STATIC_BUFFERS 10
/* Release the GIL while copying results of at least this size. */
#define GIL_THRESHOLD 1048576

/* Copy the exact bytes objects items, separated by sep, to p. */
static inline void
STRINGLIB(bytes_join_copy)(char *p, const char *sepstr, Py_ssize_t seplen,
                           PyObject *const *items, Py_ssize_t seqlen)
{
    for (Py_ssize_t i = 0; i < seqlen; i++) {
        if (i != 0 && seplen != 0) {
            if (seplen == 1) {
                *p++ = *sepstr;
            }
            else {
                memcpy(p, sepstr, seplen);
                p += seplen;
            }
        }
        PyObject *item = items[i];
        memcpy(p, PyBytes_AS_STRING(item), PyBytes_GET_SIZE(item));
        p += PyBytes_GET_SIZE(item);
    }
}

/* Join a sequence of exact bytes objects without creating buffer views.

   Return 1 and set *result (NULL with an exception set on error) if all
   items are exact bytes objects. Return 0 if an item is not an exact bytes
   object: the caller then uses the general path. */
static int
STRINGLIB(bytes_join_exact)(const char *sepstr, Py_ssize_t seplen,
                            PyObject *seq, Py_ssize_t seqlen,
                            PyObject **result)
{
    PyObject *const *items = PySequence_Fast_ITEMS(seq);
    /* sz counts a separator per item: the result length is sz - seplen. */
    Py_ssize_t sz = 0;
    for (Py_ssize_t i = 0; i < seqlen; i++) {
        PyObject *item = items[i];
        if (!PyBytes_CheckExact(item)) {
            return 0;
        }
        if (PyBytes_GET_SIZE(item) > PY_SSIZE_T_MAX - seplen - sz) {
            PyErr_SetString(PyExc_OverflowError,
                            "join() result is too long");
            *result = NULL;
            return 1;
        }
        sz += PyBytes_GET_SIZE(item) + seplen;
    }
    sz -= seplen;

    PyObject *res = STRINGLIB_NEW(NULL, sz);
    if (res == NULL) {
        *result = NULL;
        return 1;
    }
    char *p = STRINGLIB_STR(res);
    if (sz < GIL_THRESHOLD) {
        /* No Python code runs until the copy is done, so the sequence and
           its (immutable) items cannot change. */
        STRINGLIB(bytes_join_copy)(p, sepstr, seplen, items, seqlen);
    }
    else {
        /* Release the GIL while copying. Other threads can then mutate the
           sequence, so copy from a tuple which owns the items. */
        PyObject *tuple = PySequence_Tuple(seq);
        if (tuple == NULL) {
            Py_DECREF(res);
            *result = NULL;
            return 1;
        }
        PyThreadState *save = PyEval_SaveThread();
        STRINGLIB(bytes_join_copy)(p, sepstr, seplen,
                                   PySequence_Fast_ITEMS(tuple), seqlen);
        PyEval_RestoreThread(save);
        Py_DECREF(tuple);
    }
    *result = res;
    return 1;
}

Py_LOCAL_INLINE(PyObject *)
STRINGLIB(bytes_join_lock_held)(PyObject *sep, PyObject *seq)
{
    const char *sepstr = STRINGLIB_STR(sep);
    Py_ssize_t seplen = STRINGLIB_LEN(sep);
    PyObject *res = NULL;
    char *p;
    Py_ssize_t seqlen = 0;
    Py_ssize_t sz = 0;
    Py_ssize_t i, nbufs;
    PyObject *item;
    Py_buffer *buffers = NULL;
    Py_buffer static_buffers[NB_STATIC_BUFFERS];

    seqlen = PySequence_Fast_GET_SIZE(seq);
    if (seqlen == 0) {
        return STRINGLIB_NEW(NULL, 0);
    }
#if !STRINGLIB_MUTABLE
    if (seqlen == 1) {
        item = PySequence_Fast_GET_ITEM(seq, 0);
        if (STRINGLIB_CHECK_EXACT(item)) {
            return Py_NewRef(item);
        }
    }
#endif

    if (STRINGLIB(bytes_join_exact)(sepstr, seplen, seq, seqlen, &res)) {
        return res;
    }

    if (seqlen > NB_STATIC_BUFFERS) {
        buffers = PyMem_NEW(Py_buffer, seqlen);
        if (buffers == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
    }
    else {
        buffers = static_buffers;
    }

    /* Here is the general case.  Do a pre-pass to figure out the total
     * amount of space we'll need (sz), and see whether all arguments are
     * bytes-like.
     */
    for (i = 0, nbufs = 0; i < seqlen; i++) {
        Py_ssize_t itemlen;
        item = PySequence_Fast_GET_ITEM(seq, i);
        if (PyBytes_CheckExact(item)) {
            /* Use the bytes object's buffer directly. */
            buffers[i].obj = Py_NewRef(item);
            buffers[i].buf = PyBytes_AS_STRING(item);
            buffers[i].len = PyBytes_GET_SIZE(item);
        }
        else {
            /* item is only borrowed; its __buffer__() may run Python that
               drops the sequence's last reference to it. */
            Py_INCREF(item);
            if (PyObject_GetBuffer(item, &buffers[i], PyBUF_SIMPLE) != 0) {
                PyErr_Format(PyExc_TypeError,
                             "sequence item %zd: expected a bytes-like object, "
                             "%.80s found",
                             i, Py_TYPE(item)->tp_name);
                Py_DECREF(item);
                goto error;
            }
            Py_DECREF(item);
        }
        nbufs = i + 1;  /* for error cleanup */
        itemlen = buffers[i].len;
        if (itemlen > PY_SSIZE_T_MAX - sz) {
            PyErr_SetString(PyExc_OverflowError,
                            "join() result is too long");
            goto error;
        }
        sz += itemlen;
        if (i != 0) {
            if (seplen > PY_SSIZE_T_MAX - sz) {
                PyErr_SetString(PyExc_OverflowError,
                                "join() result is too long");
                goto error;
            }
            sz += seplen;
        }
        if (seqlen != PySequence_Fast_GET_SIZE(seq)) {
            PyErr_SetString(PyExc_RuntimeError,
                            "sequence changed size during iteration");
            goto error;
        }
    }

    /* Allocate result space. */
    res = STRINGLIB_NEW(NULL, sz);
    if (res == NULL)
        goto error;

    /* Catenate everything. */
    p = STRINGLIB_STR(res);
    if (!seplen) {
        /* fast path */
        for (i = 0; i < nbufs; i++) {
            Py_ssize_t n = buffers[i].len;
            char *q = buffers[i].buf;
            memcpy(p, q, n);
            p += n;
        }
    }
    else {
        Py_ssize_t n = buffers[0].len;
        char *q = buffers[0].buf;
        memcpy(p, q, n);
        p += n;

        for (i = 1; i < nbufs; i++) {
            memcpy(p, sepstr, seplen);
            p += seplen;

            n = buffers[i].len;
            q = buffers[i].buf;
            memcpy(p, q, n);
            p += n;
        }
    }
    goto done;

error:
    res = NULL;
done:
    for (i = 0; i < nbufs; i++) {
        PyObject *obj = buffers[i].obj;
        if (obj != NULL && PyBytes_CheckExact(obj)) {
            /* bytes has no bf_releasebuffer: skip PyBuffer_Release() */
            Py_DECREF(obj);
        }
        else {
            PyBuffer_Release(&buffers[i]);
        }
    }
    if (buffers != static_buffers)
        PyMem_Free(buffers);
    return res;
}

Py_LOCAL_INLINE(PyObject *)
STRINGLIB(bytes_join)(PyObject *sep, PyObject *iterable)
{
    PyObject *seq, *res;

    seq = PySequence_Fast(iterable, "can only join an iterable");
    if (seq == NULL) {
        return NULL;
    }

    Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST(iterable);
    res = STRINGLIB(bytes_join_lock_held)(sep, seq);
    Py_END_CRITICAL_SECTION_SEQUENCE_FAST();

    Py_DECREF(seq);
    return res;
}

#undef NB_STATIC_BUFFERS
#undef GIL_THRESHOLD
