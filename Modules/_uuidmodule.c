/*
 * Python UUID module:
 * - wraps libuuid or Windows rpcrt4.dll.
 * - provides gen_random() with cached entropy for fast UUID generation.
 *
 * DCE compatible Universally Unique Identifier library.
 */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "pyconfig.h"   // Py_GIL_DISABLED
#include "Python.h"

#include "pycore_pylifecycle.h"   // _PyOS_URandom()

#if defined(HAVE_UUID_H)
  // AIX, FreeBSD, libuuid with pkgconf
  #include <uuid.h>
#elif defined(HAVE_UUID_UUID_H)
  // libuuid without pkgconf
  #include <uuid/uuid.h>
#endif

#ifdef MS_WINDOWS
#include <rpc.h>
#endif

#ifdef HAVE_UNISTD_H
#  include <unistd.h>             // getpid()
#endif
#ifdef HAVE_PROCESS_H
#  include <process.h>            // getpid() on Windows
#endif

#include <string.h>

#define RANDOM_BUF_SIZE 256

static inline uint64_t
uuid_getpid(void)
{
#if defined(MS_WINDOWS)
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

typedef struct {
    uint8_t random_buf[RANDOM_BUF_SIZE];
    Py_ssize_t random_idx;
    uint64_t random_last_pid;
} uuid_state;

static inline uuid_state *
get_uuid_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (uuid_state *)state;
}

/* Fill 'bytes' with 'size' random bytes from the entropy cache.
 * The cache is refilled from _PyOS_URandom when exhausted.
 * Fork-safe: the cache is invalidated on PID change.
 *
 * Returns 0 on success, -1 on error (with exception set).
 */
static int
gen_random(uuid_state *state, uint8_t *bytes, Py_ssize_t size)
{
    // Overfetching & caching entropy improves the performance ~10x.

    // IMPORTANT: callers should have a critical section or a lock
    // around this function.

    uint64_t pid = uuid_getpid();
    if (pid != state->random_last_pid) {
        // Invalidate cache after fork so child doesn't share entropy.
        state->random_last_pid = pid;
        state->random_idx = RANDOM_BUF_SIZE;
    }

    if (state->random_idx + size <= RANDOM_BUF_SIZE) {
        memcpy(bytes, state->random_buf + state->random_idx, size);
        state->random_idx += size;
    }
    else {
        if (state->random_idx < RANDOM_BUF_SIZE) {
            // Consume remaining cached entropy first.
            Py_ssize_t partial = RANDOM_BUF_SIZE - state->random_idx;
            memcpy(bytes, state->random_buf + state->random_idx, partial);
            bytes += partial;
            size -= partial;
        }

        if (_PyOS_URandom(state->random_buf, RANDOM_BUF_SIZE) < 0) {
            return -1;
        }

        memcpy(bytes, state->random_buf, size);
        state->random_idx = size;
    }
    return 0;
}

/*[clinic input]
module _uuid
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=7cbed123a45a3859]*/

/*[clinic input]
@critical_section
_uuid.gen_random

    size: Py_ssize_t

Generate 'size' random bytes using cached entropy.

Caches entropy from the OS in blocks of 256 bytes for performance.
Fork-safe: the cache is invalidated when the PID changes.
[clinic start generated code]*/

static PyObject *
_uuid_gen_random_impl(PyObject *module, Py_ssize_t size)
/*[clinic end generated code: output=f25a5a06648dd6e8 input=66c91f008919ae73]*/
{
    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be non-negative");
        return NULL;
    }
    if (size > RANDOM_BUF_SIZE) {
        PyErr_Format(PyExc_ValueError,
                     "size must be at most %d", RANDOM_BUF_SIZE);
        return NULL;
    }

    uuid_state *state = get_uuid_state(module);
    uint8_t buf[RANDOM_BUF_SIZE];

    if (gen_random(state, buf, size) < 0) {
        return NULL;
    }

    return PyBytes_FromStringAndSize((const char *)buf, size);
}

#ifndef MS_WINDOWS

static PyObject *
py_uuid_generate_time_safe(PyObject *Py_UNUSED(context),
                           PyObject *Py_UNUSED(ignored))
{
    uuid_t uuid;
#ifdef HAVE_UUID_GENERATE_TIME_SAFE
    int res;

    res = uuid_generate_time_safe(uuid);
    return Py_BuildValue("y#i", (const char *) uuid, sizeof(uuid), res);
#elif defined(HAVE_UUID_CREATE)
    uint32_t status;
    uuid_create(&uuid, &status);
# if defined(HAVE_UUID_ENC_BE)
    unsigned char buf[sizeof(uuid)];
    uuid_enc_be(buf, &uuid);
    return Py_BuildValue("y#i", buf, sizeof(uuid), (int) status);
# else
    return Py_BuildValue("y#i", (const char *) &uuid, sizeof(uuid), (int) status);
# endif /* HAVE_UUID_CREATE */
#else /* HAVE_UUID_GENERATE_TIME_SAFE */
    uuid_generate_time(uuid);
    return Py_BuildValue("y#O", (const char *) uuid, sizeof(uuid), Py_None);
#endif /* HAVE_UUID_GENERATE_TIME_SAFE */
}

#else /* MS_WINDOWS */

static PyObject *
py_UuidCreate(PyObject *Py_UNUSED(context),
              PyObject *Py_UNUSED(ignored))
{
    UUID uuid;
    RPC_STATUS res;

    Py_BEGIN_ALLOW_THREADS
    res = UuidCreateSequential(&uuid);
    Py_END_ALLOW_THREADS

    switch (res) {
    case RPC_S_OK:
    case RPC_S_UUID_LOCAL_ONLY:
    case RPC_S_UUID_NO_ADDRESS:
        /*
        All success codes, but the latter two indicate that the UUID is random
        rather than based on the MAC address. If the OS can't figure this out,
        neither can we, so we'll take it anyway.
        */
        return Py_BuildValue("y#", (const char *)&uuid, sizeof(uuid));
    }
    PyErr_SetFromWindowsErr(res);
    return NULL;
}

static int
py_windows_has_stable_node(void)
{
    UUID uuid;
    RPC_STATUS res;
    Py_BEGIN_ALLOW_THREADS
    res = UuidCreateSequential(&uuid);
    Py_END_ALLOW_THREADS
    return res == RPC_S_OK;
}
#endif /* MS_WINDOWS */


static int
uuid_exec(PyObject *module)
{
#define ADD_INT(NAME, VALUE)                                        \
    do {                                                            \
        if (PyModule_AddIntConstant(module, (NAME), (VALUE)) < 0) { \
           return -1;                                               \
        }                                                           \
    } while (0)

    assert(sizeof(uuid_t) == 16);
#if defined(MS_WINDOWS)
    ADD_INT("has_uuid_generate_time_safe", 0);
#elif defined(HAVE_UUID_GENERATE_TIME_SAFE)
    ADD_INT("has_uuid_generate_time_safe", 1);
#else
    ADD_INT("has_uuid_generate_time_safe", 0);
#endif

#if defined(MS_WINDOWS)
    ADD_INT("has_stable_extractable_node", py_windows_has_stable_node());
#elif defined(HAVE_UUID_GENERATE_TIME_SAFE_STABLE_MAC)
    ADD_INT("has_stable_extractable_node", 1);
#else
    ADD_INT("has_stable_extractable_node", 0);
#endif

#undef ADD_INT

    uuid_state *state = get_uuid_state(module);
    state->random_idx = RANDOM_BUF_SIZE;
    state->random_last_pid = 0;

    return 0;
}

#include "clinic/_uuidmodule.c.h"

static PyMethodDef uuid_methods[] = {
#if defined(HAVE_UUID_UUID_H) || defined(HAVE_UUID_H)
    {"generate_time_safe", py_uuid_generate_time_safe, METH_NOARGS, NULL},
#endif
#if defined(MS_WINDOWS)
    {"UuidCreate", py_UuidCreate, METH_NOARGS, NULL},
#endif
    _UUID_GEN_RANDOM_METHODDEF
    {NULL, NULL, 0, NULL}           /* sentinel */
};

static PyModuleDef_Slot uuid_slots[] = {
    {Py_mod_exec, uuid_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef uuidmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_uuid",
    .m_size = sizeof(uuid_state),
    .m_methods = uuid_methods,
    .m_slots = uuid_slots,
};

PyMODINIT_FUNC
PyInit__uuid(void)
{
    return PyModuleDef_Init(&uuidmodule);
}
