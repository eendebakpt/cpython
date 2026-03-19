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

#include "pycore_long.h"          // _PyLong_FromByteArray()
#include "pycore_pylifecycle.h"   // _PyOS_URandom()
#include "pycore_time.h"          // PyTime_TimeRaw()

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

#define RANDOM_BUF_SIZE 1024

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

    // UUID v7 state
    uint64_t last_timestamp_v7;
    uint64_t last_counter_v7;
    int last_timestamp_v7_init;  // 0 = not set (equivalent to Python's None)

    // Python UUID class reference (set via _register_type)
    PyObject *UuidType;
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
_uuid.gen_random

    size: Py_ssize_t
    /

Generate 'size' random bytes using cached entropy.

Caches entropy from the OS in blocks of 256 bytes for performance.
Fork-safe: the cache is invalidated when the PID changes.
[clinic start generated code]*/

static PyObject *
_uuid_gen_random_impl(PyObject *module, Py_ssize_t size)
/*[clinic end generated code: output=f25a5a06648dd6e8 input=947a12bb32db2215]*/
{
    if (size < 0 || size > RANDOM_BUF_SIZE) {
        PyErr_Format(PyExc_ValueError,
                     "size must be between 0 and %d", RANDOM_BUF_SIZE);
        return NULL;
    }

    uuid_state *state = get_uuid_state(module);

    PyObject *result = PyBytes_FromStringAndSize(NULL, size);
    if (result == NULL) {
        return NULL;
    }

    if (gen_random(state, (uint8_t *)PyBytes_AS_STRING(result), size) < 0) {
        Py_DECREF(result);
        return NULL;
    }

    return result;
}

/* Create a UUID object from a 16-byte array via UUID._from_int(). */
static PyObject *
uuid_from_bytes(uuid_state *state, const uint8_t *bytes)
{
    if (state->UuidType == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "_uuid: UUID type not registered");
        return NULL;
    }
    PyObject *int_obj = _PyLong_FromByteArray(bytes, 16,
                                              /* little_endian */ 0,
                                              /* signed */ 0);
    if (int_obj == NULL) {
        return NULL;
    }
    // "N" steals the reference to int_obj
    return PyObject_CallMethod(
        state->UuidType, "_from_int", "N", int_obj);
}

static inline int
uuid7_get_counter_and_tail(uuid_state *state, uint64_t *counter, uint8_t *tail)
{
    uint8_t rand_bytes[10];
    if (gen_random(state, rand_bytes, 10) < 0) {
        return -1;
    }
    // 42-bit counter with MSB set to 0 (from top 42 bits of 80 random bits)
    *counter = (((uint64_t)rand_bytes[0] & 0x01) << 40) |
                ((uint64_t)rand_bytes[1] << 32) |
                ((uint64_t)rand_bytes[2] << 24) |
                ((uint64_t)rand_bytes[3] << 16) |
                ((uint64_t)rand_bytes[4] << 8) |
                ((uint64_t)rand_bytes[5]);
    // 32-bit random tail
    memcpy(tail, rand_bytes + 6, 4);
    return 0;
}

/*[clinic input]
@critical_section
_uuid.uuid7

Generate a UUID from a Unix timestamp in milliseconds and random bits.

UUIDv7 objects feature monotonicity within a millisecond.
Returns the 16 UUID bytes.
[clinic start generated code]*/

static PyObject *
_uuid_uuid7_impl(PyObject *module)
/*[clinic end generated code: output=f301accc11162c91 input=8c7d56d5b0a479a1]*/
{
    uuid_state *state = get_uuid_state(module);
    uint8_t bytes[16];
    uint64_t timestamp_ms, counter;

    PyTime_t pytime;
    if (PyTime_TimeRaw(&pytime) < 0) {
        return NULL;
    }
    // PyTime_t is in nanoseconds; convert to milliseconds
    timestamp_ms = (uint64_t)(pytime / 1000000);

    if (!state->last_timestamp_v7_init || timestamp_ms > state->last_timestamp_v7) {
        if (uuid7_get_counter_and_tail(state, &counter, bytes + 12) < 0) {
            return NULL;
        }
    }
    else {
        if (timestamp_ms < state->last_timestamp_v7) {
            // Clock went backwards; advance past last known timestamp
            timestamp_ms = state->last_timestamp_v7 + 1;
        }
        // Advance the 42-bit counter
        counter = state->last_counter_v7 + 1;
        if (counter > 0x3ffffffffffULL) {
            // Counter overflow; advance timestamp and reset
            timestamp_ms += 1;
            if (uuid7_get_counter_and_tail(state, &counter, bytes + 12) < 0) {
                return NULL;
            }
        }
        else {
            // 32-bit random tail
            if (gen_random(state, bytes + 12, 4) < 0) {
                return NULL;
            }
        }
    }

    // Build the UUID byte array (big-endian)
    // Bytes 0-5: 48-bit timestamp
    uint64_t ts = timestamp_ms & 0xffffffffffffULL;
    bytes[0] = (ts >> 40);
    bytes[1] = (ts >> 32);
    bytes[2] = (ts >> 24);
    bytes[3] = (ts >> 16);
    bytes[4] = (ts >> 8);
    bytes[5] = ts;

    // Bytes 6-7: version (7 = 0111) | counter_hi (top 12 bits of counter)
    uint16_t counter_hi = (counter >> 30) & 0x0fff;
    bytes[6] = 0x70 | (counter_hi >> 8);   // version 7
    bytes[7] = counter_hi;

    // Bytes 8-9: variant (10) | counter_mid (next 14 bits)
    uint16_t counter_mid = (counter >> 16) & 0x3fff;
    bytes[8] = 0x80 | (counter_mid >> 8);  // variant
    bytes[9] = counter_mid;

    // Bytes 10-11: counter_lo (bottom 16 bits)
    uint16_t counter_lo = counter & 0xffff;
    bytes[10] = counter_lo >> 8;
    bytes[11] = counter_lo;

    // Bytes 12-15: random tail (already filled by gen_random)

    // Update state
    state->last_timestamp_v7_init = 1;
    state->last_timestamp_v7 = timestamp_ms;
    state->last_counter_v7 = counter;

    return uuid_from_bytes(state, bytes);
}

/*[clinic input]
@critical_section
_uuid.uuid8

    a: object = None
    b: object = None
    c: object = None

Generate a UUID from three custom blocks.

'a' is the first 48-bit chunk (octets 0-5),
'b' is the mid 12-bit chunk (octets 6-7),
'c' is the last 62-bit chunk (octets 8-15).
When a value is None, a random value is generated.
[clinic start generated code]*/

static PyObject *
_uuid_uuid8_impl(PyObject *module, PyObject *a, PyObject *b, PyObject *c)
/*[clinic end generated code: output=c7fe92979af51fe9 input=dc42f778e2aa4fdc]*/
{
    uuid_state *state = get_uuid_state(module);
    uint64_t va, vb, vc;

    if (a == Py_None) {
        uint8_t buf[6];
        if (gen_random(state, buf, 6) < 0) return NULL;
        va = ((uint64_t)buf[0] << 40) | ((uint64_t)buf[1] << 32) |
             ((uint64_t)buf[2] << 24) | ((uint64_t)buf[3] << 16) |
             ((uint64_t)buf[4] << 8)  |  (uint64_t)buf[5];
    }
    else {
        va = PyLong_AsUnsignedLongLong(a);
        if (va == (uint64_t)-1 && PyErr_Occurred()) return NULL;
    }

    if (b == Py_None) {
        uint8_t buf[2];
        if (gen_random(state, buf, 2) < 0) return NULL;
        vb = ((uint64_t)buf[0] << 8) | (uint64_t)buf[1];
    }
    else {
        vb = PyLong_AsUnsignedLongLong(b);
        if (vb == (uint64_t)-1 && PyErr_Occurred()) return NULL;
    }

    if (c == Py_None) {
        uint8_t buf[8];
        if (gen_random(state, buf, 8) < 0) return NULL;
        vc = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
             ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
             ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
             ((uint64_t)buf[6] << 8)  |  (uint64_t)buf[7];
    }
    else {
        vc = PyLong_AsUnsignedLongLong(c);
        if (vc == (uint64_t)-1 && PyErr_Occurred()) return NULL;
    }

    va &= 0xffffffffffffULL;   // 48 bits
    vb &= 0xfff;               // 12 bits
    vc &= 0x3fffffffffffffffULL; // 62 bits

    uint8_t bytes[16];
    // Bytes 0-5: 'a' (48 bits)
    bytes[0] = va >> 40;
    bytes[1] = va >> 32;
    bytes[2] = va >> 24;
    bytes[3] = va >> 16;
    bytes[4] = va >> 8;
    bytes[5] = va;

    // Bytes 6-7: version (8 = 1000) | 'b' (12 bits)
    bytes[6] = 0x80 | (vb >> 8);   // version 8
    bytes[7] = vb;

    // Bytes 8-15: variant (10) | 'c' (62 bits)
    bytes[8]  = 0x80 | ((vc >> 56) & 0x3f);  // variant
    bytes[9]  = vc >> 48;
    bytes[10] = vc >> 40;
    bytes[11] = vc >> 32;
    bytes[12] = vc >> 24;
    bytes[13] = vc >> 16;
    bytes[14] = vc >> 8;
    bytes[15] = vc;

    return uuid_from_bytes(state, bytes);
}

/*[clinic input]
_uuid._register_type

    type: object
    /

Register the UUID type for use by C implementations.
[clinic start generated code]*/

static PyObject *
_uuid__register_type(PyObject *module, PyObject *type)
/*[clinic end generated code: output=3be5b63c53e76844 input=f7f7aa374f2951bd]*/
{
    uuid_state *state = get_uuid_state(module);
    Py_XSETREF(state->UuidType, Py_NewRef(type));
    Py_RETURN_NONE;
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


#include "clinic/_uuidmodule.c.h"

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
    state->last_timestamp_v7_init = 0;
    state->last_timestamp_v7 = 0;
    state->last_counter_v7 = 0;
    state->UuidType = NULL;

    return 0;
}

static int
uuid_module_traverse(PyObject *module, visitproc visit, void *arg)
{
    uuid_state *state = get_uuid_state(module);
    Py_VISIT(state->UuidType);
    return 0;
}

static int
uuid_module_clear(PyObject *module)
{
    uuid_state *state = get_uuid_state(module);
    Py_CLEAR(state->UuidType);
    return 0;
}

static PyMethodDef uuid_methods[] = {
#if defined(HAVE_UUID_UUID_H) || defined(HAVE_UUID_H)
    {"generate_time_safe", py_uuid_generate_time_safe, METH_NOARGS, NULL},
#endif
#if defined(MS_WINDOWS)
    {"UuidCreate", py_UuidCreate, METH_NOARGS, NULL},
#endif
    _UUID_GEN_RANDOM_METHODDEF
    _UUID_UUID7_METHODDEF
    _UUID_UUID8_METHODDEF
    _UUID__REGISTER_TYPE_METHODDEF
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
    .m_traverse = uuid_module_traverse,
    .m_clear = uuid_module_clear,
};

PyMODINIT_FUNC
PyInit__uuid(void)
{
    return PyModuleDef_Init(&uuidmodule);
}
