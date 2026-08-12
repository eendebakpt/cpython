/* JSON accelerator C extensor: _json module.
 *
 * It is built as a built-in module (Py_BUILD_CORE_BUILTIN define) on Windows
 * and as an extension module (Py_BUILD_CORE_MODULE define) on other
 * platforms. */

#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "pycore_ceval.h"         // _Py_EnterRecursiveCall()
#include "pycore_critical_section.h" // Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST()
#include "pycore_dict.h"          // _PyDict_SetItem_Take2()
#include "pycore_list.h"          // _PyList_AppendTakeRef()
#include "pycore_global_strings.h" // _Py_ID()
#include "pycore_pyerrors.h"      // _PyErr_FormatNote
#include "pycore_runtime.h"       // _PyRuntime
#include "pycore_tuple.h"         // _PyTuple_FromPair
#include "pycore_unicodeobject.h" // _PyUnicode_CheckConsistency()

#include <stdbool.h>              // bool

#include "clinic/_json.c.h"

/*[clinic input]
module _json
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=549fa53592c925b2]*/

typedef struct _PyScannerObject {
    PyObject_HEAD
    signed char strict;
    PyObject *object_hook;
    PyObject *object_pairs_hook;
    PyObject *array_hook;
    PyObject *parse_float;
    PyObject *parse_int;
    PyObject *parse_constant;
} PyScannerObject;

#define PyScannerObject_CAST(op)    ((PyScannerObject *)(op))

static PyMemberDef scanner_members[] = {
    {"strict", Py_T_BOOL, offsetof(PyScannerObject, strict), Py_READONLY, "strict"},
    {"object_hook", _Py_T_OBJECT, offsetof(PyScannerObject, object_hook), Py_READONLY, "object_hook"},
    {"object_pairs_hook", _Py_T_OBJECT, offsetof(PyScannerObject, object_pairs_hook), Py_READONLY},
    {"array_hook", _Py_T_OBJECT, offsetof(PyScannerObject, array_hook), Py_READONLY},
    {"parse_float", _Py_T_OBJECT, offsetof(PyScannerObject, parse_float), Py_READONLY, "parse_float"},
    {"parse_int", _Py_T_OBJECT, offsetof(PyScannerObject, parse_int), Py_READONLY, "parse_int"},
    {"parse_constant", _Py_T_OBJECT, offsetof(PyScannerObject, parse_constant), Py_READONLY, "parse_constant"},
    {NULL}
};

typedef struct _PyEncoderObject {
    PyObject_HEAD
    PyObject *markers;
    PyObject *defaultfn;
    PyObject *encoder;
    PyObject *indent;
    PyObject *key_separator;
    PyObject *item_separator;
    char sort_keys;
    char skipkeys;
    int allow_nan;
    int (*fast_encode)(PyUnicodeWriter *, PyObject *);
} PyEncoderObject;

#define PyEncoderObject_CAST(op)    ((PyEncoderObject *)(op))

/* --- Streaming encoder iterator --- */

/* One frame on the encoding stack, representing an in-progress container. */
typedef enum { FRAME_LIST, FRAME_DICT } FrameKind;

typedef struct {
    FrameKind kind;
    PyObject *container;   /* original list/dict (for error notes) */
    PyObject *items;       /* list frame: the list/tuple itself; dict frame:
                            * snapshot items list, or NULL when iterating an
                            * exact dict lazily */
    Py_ssize_t index;      /* index (or PyDict_Next pos) of the next item */
    Py_ssize_t length;     /* item count when the frame was created */
    Py_ssize_t indent_level;
    bool first;            /* true until the first item separator is emitted */
    bool need_value;       /* dict: key was emitted, value is next */
    PyObject *cur_key;     /* dict: current original key (owned), or NULL */
    PyObject *cur_keystr;  /* dict: current key as a str (owned), or NULL */
    PyObject *cur_value;   /* dict: current value (owned), or NULL */
    PyObject *ident;       /* circular reference marker key, or NULL */
    PyObject *default_idents;  /* marker keys registered while resolving a
                                * default() chain, or NULL */
    PyObject *default_sources; /* objects handed to default() to produce this
                                * container, or NULL; kept for error notes */
} EncoderFrame;

typedef struct {
    PyObject_HEAD
    PyEncoderObject *encoder;
    PyObject *root_obj;        /* pending root object before first next() */
    PyObject *indent_cache;    /* shared indent cache, or NULL */
    EncoderFrame *stack;       /* frame stack (heap-allocated) */
    Py_ssize_t stack_depth;    /* current depth */
    Py_ssize_t stack_alloc;    /* allocated capacity */
    bool running;              /* guards against reentrant next() */
} PyEncoderIterObject;

#define PyEncoderIterObject_CAST(op)  ((PyEncoderIterObject *)(op))

static PyMemberDef encoder_members[] = {
    {"markers", _Py_T_OBJECT, offsetof(PyEncoderObject, markers), Py_READONLY, "markers"},
    {"default", _Py_T_OBJECT, offsetof(PyEncoderObject, defaultfn), Py_READONLY, "default"},
    {"encoder", _Py_T_OBJECT, offsetof(PyEncoderObject, encoder), Py_READONLY, "encoder"},
    {"indent", _Py_T_OBJECT, offsetof(PyEncoderObject, indent), Py_READONLY, "indent"},
    {"key_separator", _Py_T_OBJECT, offsetof(PyEncoderObject, key_separator), Py_READONLY, "key_separator"},
    {"item_separator", _Py_T_OBJECT, offsetof(PyEncoderObject, item_separator), Py_READONLY, "item_separator"},
    {"sort_keys", Py_T_BOOL, offsetof(PyEncoderObject, sort_keys), Py_READONLY, "sort_keys"},
    {"skipkeys", Py_T_BOOL, offsetof(PyEncoderObject, skipkeys), Py_READONLY, "skipkeys"},
    {NULL}
};

/* Forward decls */

static PyObject *
ascii_escape_unicode(PyObject *pystr);
static PyObject *
py_encode_basestring_ascii(PyObject* Py_UNUSED(self), PyObject *pystr);

static PyObject *
scan_once_unicode(PyScannerObject *s, PyObject *memo, PyObject *pystr, Py_ssize_t idx, Py_ssize_t *next_idx_ptr);
static PyObject *
_build_rval_index_tuple(PyObject *rval, Py_ssize_t idx);
static PyObject *
scanner_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void
scanner_dealloc(PyObject *self);
static int
scanner_clear(PyObject *self);

static struct PyModuleDef jsonmodule;  /* forward declaration */

typedef struct {
    PyTypeObject *EncoderIterType;
} _jsonstate;

static inline _jsonstate *
get_json_state(PyObject *module)
{
    void *state = PyModule_GetState(module);
    assert(state != NULL);
    return (_jsonstate *)state;
}

static PyObject *
encoder_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void
encoder_dealloc(PyObject *self);
static PyObject *
encoder_iter_new(PyEncoderObject *encoder, PyObject *obj, Py_ssize_t indent_level);
static int
encoder_clear(PyObject *self);
static PyObject *
encoder_encode_string(PyEncoderObject *s, PyObject *obj);
static int
encoder_listencode_list(PyEncoderObject *s, PyUnicodeWriter *writer, PyObject *seq, Py_ssize_t indent_level, PyObject *indent_cache);
static int
encoder_listencode_obj(PyEncoderObject *s, PyUnicodeWriter *writer, PyObject *obj, Py_ssize_t indent_level, PyObject *indent_cache);
static int
encoder_listencode_dict(PyEncoderObject *s, PyUnicodeWriter *writer, PyObject *dct, Py_ssize_t indent_level, PyObject *indent_cache);
static PyObject *
_encoded_const(PyObject *obj);
static void
raise_errmsg(const char *msg, PyObject *s, Py_ssize_t end);
static int
_steal_accumulate(PyUnicodeWriter *writer, PyObject *stolen);
static int
encoder_write_string(PyEncoderObject *s, PyUnicodeWriter *writer, PyObject *obj);
static PyObject *
encoder_encode_float(PyEncoderObject *s, PyObject *obj);

#define S_CHAR(c) (c >= ' ' && c <= '~' && c != '\\' && c != '"')
#define IS_WHITESPACE(c) (((c) == ' ') || ((c) == '\t') || ((c) == '\n') || ((c) == '\r'))

static Py_ssize_t
ascii_escape_unichar(Py_UCS4 c, unsigned char *output, Py_ssize_t chars)
{
    /* Escape unicode code point c to ASCII escape sequences
    in char *output. output must have at least 12 bytes unused to
    accommodate an escaped surrogate pair "\uXXXX\uXXXX" */
    output[chars++] = '\\';
    switch (c) {
        case '\\': output[chars++] = c; break;
        case '"': output[chars++] = c; break;
        case '\b': output[chars++] = 'b'; break;
        case '\f': output[chars++] = 'f'; break;
        case '\n': output[chars++] = 'n'; break;
        case '\r': output[chars++] = 'r'; break;
        case '\t': output[chars++] = 't'; break;
        default:
            if (c >= 0x10000) {
                /* UTF-16 surrogate pair */
                Py_UCS4 v = Py_UNICODE_HIGH_SURROGATE(c);
                output[chars++] = 'u';
                output[chars++] = Py_hexdigits[(v >> 12) & 0xf];
                output[chars++] = Py_hexdigits[(v >>  8) & 0xf];
                output[chars++] = Py_hexdigits[(v >>  4) & 0xf];
                output[chars++] = Py_hexdigits[(v      ) & 0xf];
                c = Py_UNICODE_LOW_SURROGATE(c);
                output[chars++] = '\\';
            }
            output[chars++] = 'u';
            output[chars++] = Py_hexdigits[(c >> 12) & 0xf];
            output[chars++] = Py_hexdigits[(c >>  8) & 0xf];
            output[chars++] = Py_hexdigits[(c >>  4) & 0xf];
            output[chars++] = Py_hexdigits[(c      ) & 0xf];
    }
    return chars;
}

static Py_ssize_t
ascii_escape_size(const void *input, int kind, Py_ssize_t input_chars)
{
    Py_ssize_t i;
    Py_ssize_t output_size;

    /* Compute the output size */
    for (i = 0, output_size = 2; i < input_chars; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, input, i);
        Py_ssize_t d;
        if (S_CHAR(c)) {
            d = 1;
        }
        else {
            switch(c) {
            case '\\': case '"': case '\b': case '\f':
            case '\n': case '\r': case '\t':
                d = 2; break;
            default:
                d = c >= 0x10000 ? 12 : 6;
            }
        }
        if (output_size > PY_SSIZE_T_MAX - d) {
            PyErr_SetString(PyExc_OverflowError, "string is too long to escape");
            return -1;
        }
        output_size += d;
    }

    return output_size;
}

static PyObject *
ascii_escape_unicode_and_size(const void *input, int kind, Py_ssize_t input_chars, Py_ssize_t output_size)
{
    Py_ssize_t i;
    Py_ssize_t chars;
    PyObject *rval;
    Py_UCS1 *output;

    rval = PyUnicode_New(output_size, 127);
    if (rval == NULL) {
        return NULL;
    }
    output = PyUnicode_1BYTE_DATA(rval);
    chars = 0;
    output[chars++] = '"';
    for (i = 0; i < input_chars; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, input, i);
        if (S_CHAR(c)) {
            output[chars++] = c;
        }
        else {
            chars = ascii_escape_unichar(c, output, chars);
        }
    }
    output[chars++] = '"';
#ifdef Py_DEBUG
    assert(_PyUnicode_CheckConsistency(rval, 1));
#endif
    return rval;
}

static PyObject *
ascii_escape_unicode(PyObject *pystr)
{
    /* Take a PyUnicode pystr and return a new ASCII-only escaped PyUnicode */
    Py_ssize_t input_chars = PyUnicode_GET_LENGTH(pystr);
    const void *input = PyUnicode_DATA(pystr);
    int kind = PyUnicode_KIND(pystr);

    Py_ssize_t output_size = ascii_escape_size(input, kind, input_chars);
    if (output_size < 0) {
        return NULL;
    }

    if (output_size == input_chars + 2) {
        /* No need to escape anything: bulk-copy with surrounding quotes. */
        assert(PyUnicode_IS_ASCII(pystr));
        PyObject *rval = PyUnicode_New(output_size, 127);
        if (rval == NULL) {
            return NULL;
        }
        Py_UCS1 *output = PyUnicode_1BYTE_DATA(rval);
        output[0] = '"';
        memcpy(output + 1, input, input_chars);
        output[input_chars + 1] = '"';
#ifdef Py_DEBUG
        assert(_PyUnicode_CheckConsistency(rval, 1));
#endif
        return rval;
    }

    return ascii_escape_unicode_and_size(input, kind, input_chars, output_size);
}

static int
write_escaped_ascii(PyUnicodeWriter *writer, PyObject *pystr)
{
    Py_ssize_t input_chars;
    const void *input;
    int kind;

    input_chars = PyUnicode_GET_LENGTH(pystr);
    input = PyUnicode_DATA(pystr);
    kind = PyUnicode_KIND(pystr);

    Py_ssize_t output_size = ascii_escape_size(input, kind, input_chars);
    if (output_size < 0) {
        return -1;
    }

    if (output_size == input_chars + 2) {
        /* No need to escape anything */
        if (PyUnicodeWriter_WriteChar(writer, '"') < 0) {
            return -1;
        }
        // gh-148241: Avoid PyUnicodeWriter_WriteStr() which calls str(obj)
        // on str subclasses
        assert(PyUnicode_IS_ASCII(pystr));
        if (PyUnicodeWriter_WriteASCII(writer, input, input_chars) < 0) {
            return -1;
        }
        return PyUnicodeWriter_WriteChar(writer, '"');
    }

    PyObject *rval = ascii_escape_unicode_and_size(input, kind, input_chars, output_size);
    if (rval == NULL) {
        return -1;
    }

    return _steal_accumulate(writer, rval);
}

static Py_ssize_t
escape_size(const void *input, int kind, Py_ssize_t input_chars)
{
    Py_ssize_t i;
    Py_ssize_t output_size;

    /* Compute the output size */
    for (i = 0, output_size = 2; i < input_chars; i++) {
        Py_UCS4 c = PyUnicode_READ(kind, input, i);
        Py_ssize_t d;
        switch (c) {
        case '\\': case '"': case '\b': case '\f':
        case '\n': case '\r': case '\t':
            d = 2;
            break;
        default:
            if (c <= 0x1f)
                d = 6;
            else
                d = 1;
        }
        if (output_size > PY_SSIZE_T_MAX - d) {
            PyErr_SetString(PyExc_OverflowError, "string is too long to escape");
            return -1;
        }
        output_size += d;
    }

    return output_size;
}

static PyObject *
escape_unicode_and_size(const void *input, int kind, Py_UCS4 maxchar, Py_ssize_t input_chars, Py_ssize_t output_size)
{
    Py_ssize_t i;
    Py_ssize_t chars;
    PyObject *rval;

    rval = PyUnicode_New(output_size, maxchar);
    if (rval == NULL)
        return NULL;

    kind = PyUnicode_KIND(rval);

#define ENCODE_OUTPUT do { \
        chars = 0; \
        output[chars++] = '"'; \
        for (i = 0; i < input_chars; i++) { \
            Py_UCS4 c = PyUnicode_READ(kind, input, i); \
            switch (c) { \
            case '\\': output[chars++] = '\\'; output[chars++] = c; break; \
            case '"':  output[chars++] = '\\'; output[chars++] = c; break; \
            case '\b': output[chars++] = '\\'; output[chars++] = 'b'; break; \
            case '\f': output[chars++] = '\\'; output[chars++] = 'f'; break; \
            case '\n': output[chars++] = '\\'; output[chars++] = 'n'; break; \
            case '\r': output[chars++] = '\\'; output[chars++] = 'r'; break; \
            case '\t': output[chars++] = '\\'; output[chars++] = 't'; break; \
            default: \
                if (c <= 0x1f) { \
                    output[chars++] = '\\'; \
                    output[chars++] = 'u'; \
                    output[chars++] = '0'; \
                    output[chars++] = '0'; \
                    output[chars++] = Py_hexdigits[(c >> 4) & 0xf]; \
                    output[chars++] = Py_hexdigits[(c     ) & 0xf]; \
                } else { \
                    output[chars++] = c; \
                } \
            } \
        } \
        output[chars++] = '"'; \
    } while (0)

    if (kind == PyUnicode_1BYTE_KIND) {
        Py_UCS1 *output = PyUnicode_1BYTE_DATA(rval);
        ENCODE_OUTPUT;
    } else if (kind == PyUnicode_2BYTE_KIND) {
        Py_UCS2 *output = PyUnicode_2BYTE_DATA(rval);
        ENCODE_OUTPUT;
    } else {
        Py_UCS4 *output = PyUnicode_4BYTE_DATA(rval);
        assert(kind == PyUnicode_4BYTE_KIND);
        ENCODE_OUTPUT;
    }
#undef ENCODE_OUTPUT

#ifdef Py_DEBUG
    assert(_PyUnicode_CheckConsistency(rval, 1));
#endif
    return rval;
}

static PyObject *
escape_unicode(PyObject *pystr)
{
    /* Take a PyUnicode pystr and return a new escaped PyUnicode */
    Py_ssize_t input_chars = PyUnicode_GET_LENGTH(pystr);
    const void *input = PyUnicode_DATA(pystr);
    int kind = PyUnicode_KIND(pystr);
    Py_UCS4 maxchar = PyUnicode_MAX_CHAR_VALUE(pystr);

    Py_ssize_t output_size = escape_size(input, kind, input_chars);
    if (output_size < 0) {
        return NULL;
    }

    if (output_size == input_chars + 2) {
        /* No need to escape anything: bulk-copy with surrounding quotes. */
        PyObject *rval = PyUnicode_New(output_size, maxchar);
        if (rval == NULL) {
            return NULL;
        }
        int rkind = PyUnicode_KIND(rval);
        void *output = PyUnicode_DATA(rval);
        PyUnicode_WRITE(rkind, output, 0, '"');
        if (PyUnicode_CopyCharacters(rval, 1, pystr, 0, input_chars) < 0) {
            Py_DECREF(rval);
            return NULL;
        }
        PyUnicode_WRITE(rkind, output, input_chars + 1, '"');
#ifdef Py_DEBUG
        assert(_PyUnicode_CheckConsistency(rval, 1));
#endif
        return rval;
    }

    return escape_unicode_and_size(input, kind, maxchar, input_chars, output_size);
}

static int
write_escaped_unicode(PyUnicodeWriter *writer, PyObject *pystr)
{
    Py_ssize_t input_chars = PyUnicode_GET_LENGTH(pystr);
    const void *input = PyUnicode_DATA(pystr);
    int kind = PyUnicode_KIND(pystr);
    Py_UCS4 maxchar = PyUnicode_MAX_CHAR_VALUE(pystr);

    Py_ssize_t output_size = escape_size(input, kind, input_chars);
    if (output_size < 0) {
        return -1;
    }

    if (output_size == input_chars + 2) {
        /* No need to escape anything */
        if (PyUnicodeWriter_WriteChar(writer, '"') < 0) {
            return -1;
        }
        // gh-148241: Avoid PyUnicodeWriter_WriteStr() which calls str(obj)
        // on str subclasses
        if (_PyUnicodeWriter_WriteStr((_PyUnicodeWriter*)writer, pystr) < 0) {
            return -1;
        }
        return PyUnicodeWriter_WriteChar(writer, '"');
    }

    PyObject *rval = escape_unicode_and_size(input, kind, maxchar, input_chars, output_size);
    if (rval == NULL) {
        return -1;
    }

    return _steal_accumulate(writer, rval);
}

static void
raise_errmsg(const char *msg, PyObject *s, Py_ssize_t end)
{
    /* Use JSONDecodeError exception to raise a nice looking ValueError subclass */
    _Py_DECLARE_STR(json_decoder, "json.decoder");
    PyObject *JSONDecodeError =
         PyImport_ImportModuleAttr(&_Py_STR(json_decoder), &_Py_ID(JSONDecodeError));
    if (JSONDecodeError == NULL) {
        return;
    }

    PyObject *exc;
    exc = PyObject_CallFunction(JSONDecodeError, "zOn", msg, s, end);
    if (exc) {
        PyErr_SetObject(JSONDecodeError, exc);
        Py_DECREF(exc);
    }

    Py_DECREF(JSONDecodeError);
}

static void
raise_stop_iteration(Py_ssize_t idx)
{
    PyObject *value = PyLong_FromSsize_t(idx);
    if (value != NULL) {
        PyErr_SetObject(PyExc_StopIteration, value);
        Py_DECREF(value);
    }
}

static PyObject *
_build_rval_index_tuple(PyObject *rval, Py_ssize_t idx) {
    /* return (rval, idx) tuple, stealing reference to rval */
    PyObject *pyidx;
    /*
    steal a reference to rval, returns (rval, idx)
    */
    if (rval == NULL) {
        return NULL;
    }
    pyidx = PyLong_FromSsize_t(idx);
    if (pyidx == NULL) {
        Py_DECREF(rval);
        return NULL;
    }
    return _PyTuple_FromPairSteal(rval, pyidx);
}

static PyObject *
scanstring_unicode(PyObject *pystr, Py_ssize_t end, int strict, Py_ssize_t *next_end_ptr)
{
    /* Read the JSON string from PyUnicode pystr.
    end is the index of the first character after the quote.
    if strict is zero then literal control characters are allowed
    *next_end_ptr is a return-by-reference index of the character
        after the end quote

    Return value is a new PyUnicode
    */
    PyObject *rval = NULL;
    Py_ssize_t len;
    Py_ssize_t begin = end - 1;
    Py_ssize_t next /* = begin */;
    const void *buf;
    int kind;

    PyUnicodeWriter *writer = NULL;

    len = PyUnicode_GET_LENGTH(pystr);
    buf = PyUnicode_DATA(pystr);
    kind = PyUnicode_KIND(pystr);

    if (end < 0 || len < end) {
        PyErr_SetString(PyExc_ValueError, "end is out of bounds");
        goto bail;
    }
    while (1) {
        /* Find the end of the string or the next escape */
        Py_UCS4 c;
        {
            // Use tight scope variable to help register allocation.
            Py_UCS4 d = 0;
            for (next = end; next < len; next++) {
                d = PyUnicode_READ(kind, buf, next);
                if (d == '"' || d == '\\') {
                    break;
                }
                if (d <= 0x1f && strict) {
                    raise_errmsg("Invalid control character at", pystr, next);
                    goto bail;
                }
            }
            c = d;
        }

        if (c == '"') {
            // Fast path for simple case.
            if (writer == NULL) {
                PyObject *ret = PyUnicode_Substring(pystr, end, next);
                if (ret == NULL) {
                    goto bail;
                }
                *next_end_ptr = next + 1;;
                return ret;
            }
        }
        else if (c != '\\') {
            raise_errmsg("Unterminated string starting at", pystr, begin);
            goto bail;
        } else if (writer == NULL) {
            writer = PyUnicodeWriter_Create(0);
            if (writer == NULL) {
                goto bail;
            }
        }

        /* Pick up this chunk if it's not zero length */
        if (next != end) {
            if (PyUnicodeWriter_WriteSubstring(writer, pystr, end, next) < 0) {
                goto bail;
            }
        }
        next++;
        if (c == '"') {
            end = next;
            break;
        }
        if (next == len) {
            raise_errmsg("Unterminated string starting at", pystr, begin);
            goto bail;
        }
        c = PyUnicode_READ(kind, buf, next);
        if (c != 'u') {
            /* Non-unicode backslash escapes */
            end = next + 1;
            switch (c) {
                case '"': break;
                case '\\': break;
                case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: c = 0;
            }
            if (c == 0) {
                raise_errmsg("Invalid \\escape", pystr, end - 2);
                goto bail;
            }
        }
        else {
            c = 0;
            next++;
            end = next + 4;
            if (end > len) {
                raise_errmsg("Invalid \\uXXXX escape", pystr, next - 1);
                goto bail;
            }
            /* Decode 4 hex digits */
            for (; next < end; next++) {
                Py_UCS4 digit = PyUnicode_READ(kind, buf, next);
                c <<= 4;
                switch (digit) {
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                        c |= (digit - '0'); break;
                    case 'a': case 'b': case 'c': case 'd': case 'e':
                    case 'f':
                        c |= (digit - 'a' + 10); break;
                    case 'A': case 'B': case 'C': case 'D': case 'E':
                    case 'F':
                        c |= (digit - 'A' + 10); break;
                    default:
                        raise_errmsg("Invalid \\uXXXX escape", pystr, end - 5);
                        goto bail;
                }
            }
            /* Surrogate pair */
            if (Py_UNICODE_IS_HIGH_SURROGATE(c) && end + 6 < len &&
                PyUnicode_READ(kind, buf, next++) == '\\' &&
                PyUnicode_READ(kind, buf, next++) == 'u') {
                Py_UCS4 c2 = 0;
                end += 6;
                /* Decode 4 hex digits */
                for (; next < end; next++) {
                    Py_UCS4 digit = PyUnicode_READ(kind, buf, next);
                    c2 <<= 4;
                    switch (digit) {
                        case '0': case '1': case '2': case '3': case '4':
                        case '5': case '6': case '7': case '8': case '9':
                            c2 |= (digit - '0'); break;
                        case 'a': case 'b': case 'c': case 'd': case 'e':
                        case 'f':
                            c2 |= (digit - 'a' + 10); break;
                        case 'A': case 'B': case 'C': case 'D': case 'E':
                        case 'F':
                            c2 |= (digit - 'A' + 10); break;
                        default:
                            raise_errmsg("Invalid \\uXXXX escape", pystr, end - 5);
                            goto bail;
                    }
                }
                if (Py_UNICODE_IS_LOW_SURROGATE(c2))
                    c = Py_UNICODE_JOIN_SURROGATES(c, c2);
                else
                    end -= 6;
            }
        }
        if (PyUnicodeWriter_WriteChar(writer, c) < 0) {
            goto bail;
        }
    }

    rval = PyUnicodeWriter_Finish(writer);
    *next_end_ptr = end;
    return rval;

bail:
    *next_end_ptr = -1;
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

/*[clinic input]
_json.scanstring as py_scanstring
    pystr: unicode
    end: Py_ssize_t
    strict: bool = True
    /

Scan the string s for a JSON string.

End is the index of the character in s after the quote that started the
JSON string. Unescapes all valid JSON string escape sequences and raises
ValueError on attempt to decode an invalid string. If strict is False
then literal control characters are allowed in the string.

Returns a tuple of the decoded string and the index of the character in
s after the end quote.
[clinic start generated code]*/

static PyObject *
py_scanstring_impl(PyObject *module, PyObject *pystr, Py_ssize_t end,
                   int strict)
/*[clinic end generated code: output=961740cfae07cdb3 input=6d5abb5947ccc297]*/
{
    Py_ssize_t next_end = -1;
    PyObject *rval = scanstring_unicode(pystr, end, strict, &next_end);
    return _build_rval_index_tuple(rval, next_end);
}

/*[clinic input]
_json.encode_basestring_ascii as py_encode_basestring_ascii
    pystr: unicode
    /

Return an ASCII-only JSON representation of a Python string
[clinic start generated code]*/

static PyObject *
py_encode_basestring_ascii_impl(PyObject *module, PyObject *pystr)
/*[clinic end generated code: output=7b3841287cf211df input=4f3609498aff2de5]*/
{
    return ascii_escape_unicode(pystr);
}

/*[clinic input]
_json.encode_basestring as py_encode_basestring
    pystr: unicode
    /

Return a JSON representation of a Python string
[clinic start generated code]*/

static PyObject *
py_encode_basestring_impl(PyObject *module, PyObject *pystr)
/*[clinic end generated code: output=900950f95df3f1c9 input=d42ef714b2c07386]*/
{
    return escape_unicode(pystr);
}

static void
scanner_dealloc(PyObject *self)
{
    PyTypeObject *tp = Py_TYPE(self);
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyObject_GC_UnTrack(self);
    (void)scanner_clear(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static int
scanner_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyScannerObject *self = PyScannerObject_CAST(op);
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->object_hook);
    Py_VISIT(self->object_pairs_hook);
    Py_VISIT(self->array_hook);
    Py_VISIT(self->parse_float);
    Py_VISIT(self->parse_int);
    Py_VISIT(self->parse_constant);
    return 0;
}

static int
scanner_clear(PyObject *op)
{
    PyScannerObject *self = PyScannerObject_CAST(op);
    Py_CLEAR(self->object_hook);
    Py_CLEAR(self->object_pairs_hook);
    Py_CLEAR(self->array_hook);
    Py_CLEAR(self->parse_float);
    Py_CLEAR(self->parse_int);
    Py_CLEAR(self->parse_constant);
    return 0;
}

static PyObject *
_parse_object_unicode(PyScannerObject *s, PyObject *memo, PyObject *pystr, Py_ssize_t idx, Py_ssize_t *next_idx_ptr)
{
    /* Read a JSON object from PyUnicode pystr.
    idx is the index of the first character after the opening curly brace.
    *next_idx_ptr is a return-by-reference index to the first character after
        the closing curly brace.

    Returns a new PyObject (usually a dict, but object_hook can change that)
    */
    const void *str;
    int kind;
    Py_ssize_t end_idx;
    PyObject *rval = NULL;
    PyObject *key = NULL;
    int has_pairs_hook = (s->object_pairs_hook != Py_None);
    Py_ssize_t next_idx;
    Py_ssize_t comma_idx;

    str = PyUnicode_DATA(pystr);
    kind = PyUnicode_KIND(pystr);
    end_idx = PyUnicode_GET_LENGTH(pystr) - 1;

    if (has_pairs_hook)
        rval = PyList_New(0);
    else
        rval = PyDict_New();
    if (rval == NULL)
        return NULL;

    /* skip whitespace after { */
    while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind,str, idx))) idx++;

    /* only loop if the object is non-empty */
    if (idx > end_idx || PyUnicode_READ(kind, str, idx) != '}') {
        while (1) {
            PyObject *memokey;

            /* read key */
            if (idx > end_idx || PyUnicode_READ(kind, str, idx) != '"') {
                raise_errmsg("Expecting property name enclosed in double quotes", pystr, idx);
                goto bail;
            }
            key = scanstring_unicode(pystr, idx + 1, s->strict, &next_idx);
            if (key == NULL)
                goto bail;
            if (PyDict_SetDefaultRef(memo, key, key, &memokey) < 0) {
                goto bail;
            }
            Py_SETREF(key, memokey);
            idx = next_idx;

            /* skip whitespace between key and : delimiter, read :, skip whitespace */
            while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;
            if (idx > end_idx || PyUnicode_READ(kind, str, idx) != ':') {
                raise_errmsg("Expecting ':' delimiter", pystr, idx);
                goto bail;
            }
            idx++;
            while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;

            /* read any JSON term */
            PyObject *val = scan_once_unicode(s, memo, pystr, idx, &next_idx);
            if (val == NULL)
                goto bail;

            /* The steal below takes our references to both key and val
               (releasing them on failure).  Only key is reset for the bail
               path; val is never live there, so it needs no cleanup. */
            if (has_pairs_hook) {
                PyObject *item = _PyTuple_FromPairSteal(key, val);
                key = NULL;
                if (item == NULL)
                    goto bail;
                if (PyList_Append(rval, item) == -1) {
                    Py_DECREF(item);
                    goto bail;
                }
                Py_DECREF(item);
            }
            else {
                int err = _PyDict_SetItem_Take2((PyDictObject *)rval, key, val);
                key = NULL;
                if (err < 0)
                    goto bail;
            }
            idx = next_idx;

            /* skip whitespace before } or , */
            while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;

            /* bail if the object is closed or we didn't get the , delimiter */
            if (idx <= end_idx && PyUnicode_READ(kind, str, idx) == '}')
                break;
            if (idx > end_idx || PyUnicode_READ(kind, str, idx) != ',') {
                raise_errmsg("Expecting ',' delimiter", pystr, idx);
                goto bail;
            }
            comma_idx = idx;
            idx++;

            /* skip whitespace after , delimiter */
            while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;

            if (idx <= end_idx && PyUnicode_READ(kind, str, idx) == '}') {
                raise_errmsg("Illegal trailing comma before end of object", pystr, comma_idx);
                goto bail;
            }
        }
    }

    *next_idx_ptr = idx + 1;

    if (has_pairs_hook) {
        PyObject *res = PyObject_CallOneArg(s->object_pairs_hook, rval);
        Py_DECREF(rval);
        return res;
    }

    /* if object_hook is not None: rval = object_hook(rval) */
    if (s->object_hook != Py_None) {
        PyObject *res = PyObject_CallOneArg(s->object_hook, rval);
        Py_DECREF(rval);
        return res;
    }
    return rval;
bail:
    Py_XDECREF(key);
    Py_XDECREF(rval);
    return NULL;
}

static PyObject *
_parse_array_unicode(PyScannerObject *s, PyObject *memo, PyObject *pystr, Py_ssize_t idx, Py_ssize_t *next_idx_ptr) {
    /* Read a JSON array from PyUnicode pystr.
    idx is the index of the first character after the opening brace.
    *next_idx_ptr is a return-by-reference index to the first character after
        the closing brace.

    Returns a new PyList
    */
    const void *str;
    int kind;
    Py_ssize_t end_idx;
    PyObject *rval;
    Py_ssize_t next_idx;
    Py_ssize_t comma_idx;

    rval = PyList_New(0);
    if (rval == NULL)
        return NULL;

    str = PyUnicode_DATA(pystr);
    kind = PyUnicode_KIND(pystr);
    end_idx = PyUnicode_GET_LENGTH(pystr) - 1;

    /* skip whitespace after [ */
    while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;

    /* only loop if the array is non-empty */
    if (idx > end_idx || PyUnicode_READ(kind, str, idx) != ']') {
        while (1) {

            /* read any JSON term  */
            PyObject *val = scan_once_unicode(s, memo, pystr, idx, &next_idx);
            if (val == NULL)
                goto bail;

            if (_PyList_AppendTakeRef((PyListObject *)rval, val) < 0)
                goto bail;
            idx = next_idx;

            /* skip whitespace between term and , */
            while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;

            /* bail if the array is closed or we didn't get the , delimiter */
            if (idx <= end_idx && PyUnicode_READ(kind, str, idx) == ']')
                break;
            if (idx > end_idx || PyUnicode_READ(kind, str, idx) != ',') {
                raise_errmsg("Expecting ',' delimiter", pystr, idx);
                goto bail;
            }
            comma_idx = idx;
            idx++;

            /* skip whitespace after , */
            while (idx <= end_idx && IS_WHITESPACE(PyUnicode_READ(kind, str, idx))) idx++;

            if (idx <= end_idx && PyUnicode_READ(kind, str, idx) == ']') {
                raise_errmsg("Illegal trailing comma before end of array", pystr, comma_idx);
                goto bail;
            }
        }
    }

    /* verify that idx < end_idx, PyUnicode_READ(kind, str, idx) should be ']' */
    if (idx > end_idx || PyUnicode_READ(kind, str, idx) != ']') {
        raise_errmsg("Expecting value", pystr, end_idx);
        goto bail;
    }
    *next_idx_ptr = idx + 1;
    /* if array_hook is not None: return array_hook(rval) */
    if (!Py_IsNone(s->array_hook)) {
        PyObject *res = PyObject_CallOneArg(s->array_hook, rval);
        Py_DECREF(rval);
        return res;
    }
    return rval;
bail:
    Py_DECREF(rval);
    return NULL;
}

static PyObject *
_parse_constant(PyScannerObject *s, const char *constant, Py_ssize_t idx, Py_ssize_t *next_idx_ptr) {
    /* Read a JSON constant.
    constant is the constant string that was found
        ("NaN", "Infinity", "-Infinity").
    idx is the index of the first character of the constant
    *next_idx_ptr is a return-by-reference index to the first character after
        the constant.

    Returns the result of parse_constant
    */
    PyObject *cstr;
    PyObject *rval;
    /* constant is "NaN", "Infinity", or "-Infinity" */
    cstr = PyUnicode_InternFromString(constant);
    if (cstr == NULL)
        return NULL;

    /* rval = parse_constant(constant) */
    rval = PyObject_CallOneArg(s->parse_constant, cstr);
    idx += PyUnicode_GET_LENGTH(cstr);
    Py_DECREF(cstr);
    *next_idx_ptr = idx;
    return rval;
}

static PyObject *
_match_number_unicode(PyScannerObject *s, PyObject *pystr, Py_ssize_t start, Py_ssize_t *next_idx_ptr) {
    /* Read a JSON number from PyUnicode pystr.
    idx is the index of the first character of the number
    *next_idx_ptr is a return-by-reference index to the first character after
        the number.

    Returns a new PyObject representation of that number:
        PyLong, or PyFloat.
        May return other types if parse_int or parse_float are set
    */
    const void *str;
    int kind;
    Py_ssize_t end_idx;
    Py_ssize_t idx = start;
    int is_float = 0;
    PyObject *rval;
    PyObject *numstr = NULL;
    PyObject *custom_func;

    str = PyUnicode_DATA(pystr);
    kind = PyUnicode_KIND(pystr);
    end_idx = PyUnicode_GET_LENGTH(pystr) - 1;

    /* read a sign if it's there, make sure it's not the end of the string */
    if (PyUnicode_READ(kind, str, idx) == '-') {
        idx++;
        if (idx > end_idx) {
            raise_stop_iteration(start);
            return NULL;
        }
    }

    /* read as many integer digits as we find as long as it doesn't start with 0 */
    if (PyUnicode_READ(kind, str, idx) >= '1' && PyUnicode_READ(kind, str, idx) <= '9') {
        idx++;
        while (idx <= end_idx && PyUnicode_READ(kind, str, idx) >= '0' && PyUnicode_READ(kind, str, idx) <= '9') idx++;
    }
    /* if it starts with 0 we only expect one integer digit */
    else if (PyUnicode_READ(kind, str, idx) == '0') {
        idx++;
    }
    /* no integer digits, error */
    else {
        raise_stop_iteration(start);
        return NULL;
    }

    /* if the next char is '.' followed by a digit then read all float digits */
    if (idx < end_idx && PyUnicode_READ(kind, str, idx) == '.' && PyUnicode_READ(kind, str, idx + 1) >= '0' && PyUnicode_READ(kind, str, idx + 1) <= '9') {
        is_float = 1;
        idx += 2;
        while (idx <= end_idx && PyUnicode_READ(kind, str, idx) >= '0' && PyUnicode_READ(kind, str, idx) <= '9') idx++;
    }

    /* if the next char is 'e' or 'E' then maybe read the exponent (or backtrack) */
    if (idx < end_idx && (PyUnicode_READ(kind, str, idx) == 'e' || PyUnicode_READ(kind, str, idx) == 'E')) {
        Py_ssize_t e_start = idx;
        idx++;

        /* read an exponent sign if present */
        if (idx < end_idx && (PyUnicode_READ(kind, str, idx) == '-' || PyUnicode_READ(kind, str, idx) == '+')) idx++;

        /* read all digits */
        while (idx <= end_idx && PyUnicode_READ(kind, str, idx) >= '0' && PyUnicode_READ(kind, str, idx) <= '9') idx++;

        /* if we got a digit, then parse as float. if not, backtrack */
        if (PyUnicode_READ(kind, str, idx - 1) >= '0' && PyUnicode_READ(kind, str, idx - 1) <= '9') {
            is_float = 1;
        }
        else {
            idx = e_start;
        }
    }

    if (is_float && s->parse_float != (PyObject *)&PyFloat_Type)
        custom_func = s->parse_float;
    else if (!is_float && s->parse_int != (PyObject *) &PyLong_Type)
        custom_func = s->parse_int;
    else
        custom_func = NULL;

    if (custom_func) {
        /* copy the section we determined to be a number */
        numstr = PyUnicode_FromKindAndData(kind,
                                           (char*)str + kind * start,
                                           idx - start);
        if (numstr == NULL)
            return NULL;
        rval = PyObject_CallOneArg(custom_func, numstr);
    }
    else {
        Py_ssize_t i, n;
        char *buf;
        /* Straight conversion to ASCII, to avoid costly conversion of
           decimal unicode digits (which cannot appear here) */
        n = idx - start;
        numstr = PyBytes_FromStringAndSize(NULL, n);
        if (numstr == NULL)
            return NULL;
        buf = PyBytes_AS_STRING(numstr);
        for (i = 0; i < n; i++) {
            buf[i] = (char) PyUnicode_READ(kind, str, i + start);
        }
        if (is_float)
            rval = PyFloat_FromString(numstr);
        else
            rval = PyLong_FromString(buf, NULL, 10);
    }
    Py_DECREF(numstr);
    *next_idx_ptr = idx;
    return rval;
}

static PyObject *
scan_once_unicode(PyScannerObject *s, PyObject *memo, PyObject *pystr, Py_ssize_t idx, Py_ssize_t *next_idx_ptr)
{
    /* Read one JSON term (of any kind) from PyUnicode pystr.
    idx is the index of the first character of the term
    *next_idx_ptr is a return-by-reference index to the first character after
        the number.

    Returns a new PyObject representation of the term.
    */
    PyObject *res;
    const void *str;
    int kind;
    Py_ssize_t length;

    str = PyUnicode_DATA(pystr);
    kind = PyUnicode_KIND(pystr);
    length = PyUnicode_GET_LENGTH(pystr);

    if (idx < 0) {
        PyErr_SetString(PyExc_ValueError, "idx cannot be negative");
        return NULL;
    }
    if (idx >= length) {
        raise_stop_iteration(idx);
        return NULL;
    }

    switch (PyUnicode_READ(kind, str, idx)) {
        case '"':
            /* string */
            return scanstring_unicode(pystr, idx + 1, s->strict, next_idx_ptr);
        case '{':
            /* object */
            if (_Py_EnterRecursiveCall(" while decoding a JSON object "
                                       "from a unicode string"))
                return NULL;
            res = _parse_object_unicode(s, memo, pystr, idx + 1, next_idx_ptr);
            _Py_LeaveRecursiveCall();
            return res;
        case '[':
            /* array */
            if (_Py_EnterRecursiveCall(" while decoding a JSON array "
                                       "from a unicode string"))
                return NULL;
            res = _parse_array_unicode(s, memo, pystr, idx + 1, next_idx_ptr);
            _Py_LeaveRecursiveCall();
            return res;
        case 'n':
            /* null */
            if ((idx + 3 < length) && PyUnicode_READ(kind, str, idx + 1) == 'u' && PyUnicode_READ(kind, str, idx + 2) == 'l' && PyUnicode_READ(kind, str, idx + 3) == 'l') {
                *next_idx_ptr = idx + 4;
                Py_RETURN_NONE;
            }
            break;
        case 't':
            /* true */
            if ((idx + 3 < length) && PyUnicode_READ(kind, str, idx + 1) == 'r' && PyUnicode_READ(kind, str, idx + 2) == 'u' && PyUnicode_READ(kind, str, idx + 3) == 'e') {
                *next_idx_ptr = idx + 4;
                Py_RETURN_TRUE;
            }
            break;
        case 'f':
            /* false */
            if ((idx + 4 < length) && PyUnicode_READ(kind, str, idx + 1) == 'a' &&
                PyUnicode_READ(kind, str, idx + 2) == 'l' &&
                PyUnicode_READ(kind, str, idx + 3) == 's' &&
                PyUnicode_READ(kind, str, idx + 4) == 'e') {
                *next_idx_ptr = idx + 5;
                Py_RETURN_FALSE;
            }
            break;
        case 'N':
            /* NaN */
            if ((idx + 2 < length) && PyUnicode_READ(kind, str, idx + 1) == 'a' &&
                PyUnicode_READ(kind, str, idx + 2) == 'N') {
                return _parse_constant(s, "NaN", idx, next_idx_ptr);
            }
            break;
        case 'I':
            /* Infinity */
            if ((idx + 7 < length) && PyUnicode_READ(kind, str, idx + 1) == 'n' &&
                PyUnicode_READ(kind, str, idx + 2) == 'f' &&
                PyUnicode_READ(kind, str, idx + 3) == 'i' &&
                PyUnicode_READ(kind, str, idx + 4) == 'n' &&
                PyUnicode_READ(kind, str, idx + 5) == 'i' &&
                PyUnicode_READ(kind, str, idx + 6) == 't' &&
                PyUnicode_READ(kind, str, idx + 7) == 'y') {
                return _parse_constant(s, "Infinity", idx, next_idx_ptr);
            }
            break;
        case '-':
            /* -Infinity */
            if ((idx + 8 < length) && PyUnicode_READ(kind, str, idx + 1) == 'I' &&
                PyUnicode_READ(kind, str, idx + 2) == 'n' &&
                PyUnicode_READ(kind, str, idx + 3) == 'f' &&
                PyUnicode_READ(kind, str, idx + 4) == 'i' &&
                PyUnicode_READ(kind, str, idx + 5) == 'n' &&
                PyUnicode_READ(kind, str, idx + 6) == 'i' &&
                PyUnicode_READ(kind, str, idx + 7) == 't' &&
                PyUnicode_READ(kind, str, idx + 8) == 'y') {
                return _parse_constant(s, "-Infinity", idx, next_idx_ptr);
            }
            break;
    }
    /* Didn't find a string, object, array, or named constant. Look for a number. */
    return _match_number_unicode(s, pystr, idx, next_idx_ptr);
}

static PyObject *
scanner_call(PyObject *self, PyObject *args, PyObject *kwds)
{
    /* Python callable interface to scan_once_{str,unicode} */
    PyObject *pystr;
    PyObject *rval;
    Py_ssize_t idx;
    Py_ssize_t next_idx = -1;
    static char *kwlist[] = {"string", "idx", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "On:scan_once", kwlist, &pystr, &idx))
        return NULL;

    if (!PyUnicode_Check(pystr)) {
        PyErr_Format(PyExc_TypeError,
                     "first argument must be a string, not %.80s",
                     Py_TYPE(pystr)->tp_name);
        return NULL;
    }

    PyObject *memo = PyDict_New();
    if (memo == NULL) {
        return NULL;
    }
    rval = scan_once_unicode(PyScannerObject_CAST(self),
                             memo, pystr, idx, &next_idx);
    Py_DECREF(memo);
    if (rval == NULL)
        return NULL;
    return _build_rval_index_tuple(rval, next_idx);
}

static PyObject *
scanner_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyScannerObject *s;
    PyObject *ctx;
    PyObject *strict;
    static char *kwlist[] = {"context", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O:make_scanner", kwlist, &ctx))
        return NULL;

    s = (PyScannerObject *)type->tp_alloc(type, 0);
    if (s == NULL) {
        return NULL;
    }

    /* All of these will fail "gracefully" so we don't need to verify them */
    strict = PyObject_GetAttrString(ctx, "strict");
    if (strict == NULL)
        goto bail;
    s->strict = PyObject_IsTrue(strict);
    Py_DECREF(strict);
    if (s->strict < 0)
        goto bail;
    s->object_hook = PyObject_GetAttrString(ctx, "object_hook");
    if (s->object_hook == NULL)
        goto bail;
    s->object_pairs_hook = PyObject_GetAttrString(ctx, "object_pairs_hook");
    if (s->object_pairs_hook == NULL)
        goto bail;
    s->array_hook = PyObject_GetAttrString(ctx, "array_hook");
    if (s->array_hook == NULL) {
        goto bail;
    }
    s->parse_float = PyObject_GetAttrString(ctx, "parse_float");
    if (s->parse_float == NULL)
        goto bail;
    s->parse_int = PyObject_GetAttrString(ctx, "parse_int");
    if (s->parse_int == NULL)
        goto bail;
    s->parse_constant = PyObject_GetAttrString(ctx, "parse_constant");
    if (s->parse_constant == NULL)
        goto bail;

    return (PyObject *)s;

bail:
    Py_DECREF(s);
    return NULL;
}

PyDoc_STRVAR(scanner_doc, "JSON scanner object");

static PyType_Slot PyScannerType_slots[] = {
    {Py_tp_doc, (void *)scanner_doc},
    {Py_tp_dealloc, scanner_dealloc},
    {Py_tp_call, scanner_call},
    {Py_tp_traverse, scanner_traverse},
    {Py_tp_clear, scanner_clear},
    {Py_tp_members, scanner_members},
    {Py_tp_new, scanner_new},
    {0, 0}
};

static PyType_Spec PyScannerType_spec = {
    .name = "_json.Scanner",
    .basicsize = sizeof(PyScannerObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = PyScannerType_slots,
};

static PyObject *
encoder_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"markers", "default", "encoder", "indent", "key_separator", "item_separator", "sort_keys", "skipkeys", "allow_nan", NULL};

    PyEncoderObject *s;
    PyObject *markers, *defaultfn, *encoder, *indent, *key_separator;
    PyObject *item_separator;
    int sort_keys, skipkeys, allow_nan;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOOOUUppp:make_encoder", kwlist,
        &markers, &defaultfn, &encoder, &indent,
        &key_separator, &item_separator,
        &sort_keys, &skipkeys, &allow_nan))
        return NULL;

    if (markers != Py_None && !PyDict_Check(markers)) {
        PyErr_Format(PyExc_TypeError,
                     "make_encoder() argument 1 must be dict or None, "
                     "not %.200s", Py_TYPE(markers)->tp_name);
        return NULL;
    }

    s = (PyEncoderObject *)type->tp_alloc(type, 0);
    if (s == NULL)
        return NULL;

    s->markers = Py_NewRef(markers);
    s->defaultfn = Py_NewRef(defaultfn);
    s->encoder = Py_NewRef(encoder);
    s->indent = Py_NewRef(indent);
    s->key_separator = Py_NewRef(key_separator);
    s->item_separator = Py_NewRef(item_separator);
    s->sort_keys = sort_keys;
    s->skipkeys = skipkeys;
    s->allow_nan = allow_nan;
    s->fast_encode = NULL;

    if (PyCFunction_Check(s->encoder)) {
        PyCFunction f = PyCFunction_GetFunction(s->encoder);
        if (f == py_encode_basestring_ascii) {
            s->fast_encode = write_escaped_ascii;
        }
        else if (f == py_encode_basestring) {
            s->fast_encode = write_escaped_unicode;
        }
    }

    return (PyObject *)s;
}


/* indent_cache is a list that contains intermixed values at even and odd
 * positions:
 *
 * 2*k   : '\n' + indent * (k + initial_indent_level)
 *         strings written after opening and before closing brackets
 * 2*k-1 : item_separator + '\n' + indent * (k + initial_indent_level)
 *         strings written between items
 *
 * Its size is always an odd number.
 */
static PyObject *
create_indent_cache(PyEncoderObject *s, Py_ssize_t indent_level)
{
    PyObject *newline_indent = PyUnicode_FromOrdinal('\n');
    if (newline_indent != NULL && indent_level) {
        PyUnicode_AppendAndDel(&newline_indent,
                               PySequence_Repeat(s->indent, indent_level));
    }
    if (newline_indent == NULL) {
        return NULL;
    }
    PyObject *indent_cache = PyList_New(1);
    if (indent_cache == NULL) {
        Py_DECREF(newline_indent);
        return NULL;
    }
    PyList_SET_ITEM(indent_cache, 0, newline_indent);
    return indent_cache;
}

/* Extend indent_cache by adding values for the next level.
 * It should have values for the indent_level-1 level before the call.
 */
static int
update_indent_cache(PyEncoderObject *s,
                    Py_ssize_t indent_level, PyObject *indent_cache)
{
    assert(indent_level * 2 == PyList_GET_SIZE(indent_cache) + 1);
    assert(indent_level > 0);
    PyObject *newline_indent = PyList_GET_ITEM(indent_cache, (indent_level - 1)*2);
    newline_indent = PyUnicode_Concat(newline_indent, s->indent);
    if (newline_indent == NULL) {
        return -1;
    }
    PyObject *separator_indent = PyUnicode_Concat(s->item_separator, newline_indent);
    if (separator_indent == NULL) {
        Py_DECREF(newline_indent);
        return -1;
    }

    if (PyList_Append(indent_cache, separator_indent) < 0 ||
        PyList_Append(indent_cache, newline_indent) < 0)
    {
        Py_DECREF(separator_indent);
        Py_DECREF(newline_indent);
        return -1;
    }
    Py_DECREF(separator_indent);
    Py_DECREF(newline_indent);
    return 0;
}

static PyObject *
get_item_separator(PyEncoderObject *s,
                   Py_ssize_t indent_level, PyObject *indent_cache)
{
    assert(indent_level > 0);
    if (indent_level * 2 > PyList_GET_SIZE(indent_cache)) {
        if (update_indent_cache(s, indent_level, indent_cache) < 0) {
            return NULL;
        }
    }
    assert(indent_level * 2 < PyList_GET_SIZE(indent_cache));
    return PyList_GET_ITEM(indent_cache, indent_level * 2 - 1);
}

static int
write_newline_indent(PyUnicodeWriter *writer,
                     Py_ssize_t indent_level, PyObject *indent_cache)
{
    PyObject *newline_indent = PyList_GET_ITEM(indent_cache, indent_level * 2);
    return PyUnicodeWriter_WriteStr(writer, newline_indent);
}


/* Get the newline+indent string for the given level (borrowed from
 * indent_cache, which is grown on demand).  Returns NULL on error. */
static PyObject *
get_newline_indent(PyEncoderObject *enc, Py_ssize_t level,
                   PyObject *indent_cache)
{
    if (level * 2 >= PyList_GET_SIZE(indent_cache) &&
        update_indent_cache(enc, level, indent_cache) < 0) {
        return NULL;
    }
    return PyList_GET_ITEM(indent_cache, level * 2);
}

/* --- Streaming encoder iterator --- */

/* Like encoder_write_string, but returns the encoded str for the streaming
 * iterator to yield, instead of writing to a writer. */
static PyObject *
encoder_encode_string(PyEncoderObject *s, PyObject *obj)
{
    if (s->fast_encode == write_escaped_ascii) {
        return ascii_escape_unicode(obj);
    }
    if (s->fast_encode == write_escaped_unicode) {
        return escape_unicode(obj);
    }
    PyObject *encoded = PyObject_CallOneArg(s->encoder, obj);
    if (encoded == NULL) {
        return NULL;
    }
    if (!PyUnicode_Check(encoded)) {
        PyErr_Format(PyExc_TypeError,
                     "encoder() must return a string, not %.80s",
                     Py_TYPE(encoded)->tp_name);
        Py_DECREF(encoded);
        return NULL;
    }
    return encoded;
}

/* Encode a non-container to a str.  Returns NULL+exception on error, or NULL
 * with no exception if obj is a container (the caller then pushes a frame). */
static PyObject *
iter_encode_scalar(PyEncoderObject *s, PyObject *obj)
{
    if (obj == Py_None || obj == Py_True || obj == Py_False) {
        return _encoded_const(obj);
    }
    if (PyUnicode_Check(obj)) {
        return encoder_encode_string(s, obj);
    }
    if (PyLong_Check(obj)) {
        return PyLong_Type.tp_repr(obj);
    }
    if (PyFloat_Check(obj)) {
        return encoder_encode_float(s, obj);
    }
    return NULL;
}

/* Register obj for circular-reference detection.  Stores a new marker key in
 * *ident_out (NULL if markers are disabled); returns 0, or -1 on a circular
 * reference or error. */
static int
json_marker_enter(PyObject *markers, PyObject *obj, PyObject **ident_out)
{
    *ident_out = NULL;
    if (markers == Py_None) {
        return 0;
    }
    PyObject *ident = PyLong_FromVoidPtr(obj);
    if (ident == NULL) {
        return -1;
    }
    int has_key = PyDict_Contains(markers, ident);
    if (has_key) {
        if (has_key != -1) {
            PyErr_SetString(PyExc_ValueError, "Circular reference detected");
        }
        Py_DECREF(ident);
        return -1;
    }
    if (PyDict_SetItem(markers, ident, obj) < 0) {
        Py_DECREF(ident);
        return -1;
    }
    *ident_out = ident;
    return 0;
}

/* Undo json_marker_enter: drop the marker and release ident (NULL-safe).
 * Returns 0, or -1 if removal fails; ident is released either way. */
static int
json_marker_leave(PyObject *markers, PyObject *ident)
{
    if (ident == NULL) {
        return 0;
    }
    int rv = 0;
    /* markers is NULL if the encoder's tp_clear ran before the iterator's
     * (both can sit in the same reference cycle). */
    if (markers != NULL) {
        rv = PyDict_DelItem(markers, ident);
    }
    Py_DECREF(ident);
    return rv;
}

/* Remove the markers registered by a default() chain.  Returns 0, or -1 with
 * an exception set (the first failure wins). */
static int
iter_leave_chain(PyEncoderObject *enc, PyObject *idents)
{
    if (idents == NULL || enc->markers == NULL || enc->markers == Py_None) {
        return 0;
    }
    int rv = 0;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(idents); i++) {
        if (PyDict_DelItem(enc->markers, PyList_GET_ITEM(idents, i)) < 0) {
            rv = -1;
        }
    }
    return rv;
}

/* Push a frame for a non-empty list/tuple or dict.  Returns 0 / -1 (exception
 * set).  The opener bracket is not yielded here; the caller emits it. */
static int
iter_push_frame(PyEncoderIterObject *self, PyObject *container,
                Py_ssize_t indent_level)
{
    if (self->stack_depth >= Py_GetRecursionLimit()) {
        PyErr_SetString(PyExc_RecursionError,
                        "maximum recursion depth exceeded "
                        "while encoding a JSON object");
        return -1;
    }
    if (self->stack_depth == self->stack_alloc) {
        Py_ssize_t new_alloc = self->stack_alloc ? self->stack_alloc * 2 : 8;
        EncoderFrame *new_stack = PyMem_Realloc(self->stack,
                                                new_alloc * sizeof(EncoderFrame));
        if (new_stack == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        self->stack = new_stack;
        self->stack_alloc = new_alloc;
    }

    PyEncoderObject *enc = self->encoder;
    EncoderFrame *f = &self->stack[self->stack_depth];
    f->container = Py_NewRef(container);
    f->indent_level = indent_level;
    f->first = true;
    f->need_value = false;
    f->cur_key = NULL;
    f->cur_keystr = NULL;
    f->cur_value = NULL;
    f->ident = NULL;
    f->default_idents = NULL;
    f->default_sources = NULL;

    if (json_marker_enter(enc->markers, container, &f->ident) < 0) {
        Py_DECREF(f->container);
        return -1;
    }

    if (PyList_Check(container) || PyTuple_Check(container)) {
        f->kind = FRAME_LIST;
        f->items = Py_NewRef(container);
        f->length = PySequence_Fast_GET_SIZE(f->items);
        f->index = 0;
    }
    else {
        /* dict */
        f->kind = FRAME_DICT;
        f->index = 0;
        if (PyAnyDict_CheckExact(container) && !enc->sort_keys) {
            /* Iterate lazily with PyDict_Next: iter_dict_next() raises
             * RuntimeError if the dict changes size between yields. */
            f->items = NULL;
            f->length = PyDict_GET_SIZE(container);
        }
        else {
            /* Snapshot into a list we own exclusively: sort_keys must fix
             * the order upfront, and a non-dict mapping controls the list
             * its items() returns (gh-142831). */
            if (PyDict_CheckExact(container)) {
                f->items = PyDict_Items(container);
            }
            else {
                PyObject *items = PyMapping_Items(container);
                if (items != NULL && PyList_CheckExact(items)
                    && Py_REFCNT(items) == 1) {
                    /* A fresh list nothing else can mutate. */
                    f->items = items;
                }
                else {
                    f->items = items != NULL ? PySequence_List(items) : NULL;
                    Py_XDECREF(items);
                }
            }
            if (f->items == NULL) {
                goto fail;
            }
            if (enc->sort_keys && PyList_Sort(f->items) < 0) {
                Py_CLEAR(f->items);
                goto fail;
            }
            f->length = PyList_GET_SIZE(f->items);
        }
    }

    self->stack_depth++;
    return 0;

fail:
    (void)json_marker_leave(enc->markers, f->ident);
    Py_DECREF(f->container);
    return -1;
}

/* Pop the top frame.  Returns 0, or -1 with an exception set when removing a
 * circular-reference marker failed. */
static int
iter_pop_frame(PyEncoderIterObject *self)
{
    assert(self->stack_depth > 0);
    self->stack_depth--;
    EncoderFrame *f = &self->stack[self->stack_depth];
    PyEncoderObject *enc = self->encoder;
    int rv = json_marker_leave(enc->markers, f->ident);
    if (iter_leave_chain(enc, f->default_idents) < 0) {
        rv = -1;
    }
    Py_XDECREF(f->default_idents);
    Py_XDECREF(f->default_sources);
    Py_XDECREF(f->cur_key);
    Py_XDECREF(f->cur_keystr);
    Py_XDECREF(f->cur_value);
    Py_XDECREF(f->items);
    Py_DECREF(f->container);
    return rv;
}

/* Convert a dict key to the unescaped string to be quoted (str kept as-is;
 * int/float/bool/None stringified).  Returns a new ref, or NULL: for an
 * unsupported type *skip is set under skipkeys (no exception), else a
 * TypeError is raised. */
static PyObject *
encoder_key_to_str(PyEncoderObject *s, PyObject *key, int *skip)
{
    *skip = 0;
    if (PyUnicode_Check(key)) {
        return Py_NewRef(key);
    }
    if (PyFloat_Check(key)) {
        return encoder_encode_float(s, key);
    }
    /* Must precede PyLong_Check: True and False are also 1 and 0. */
    if (key == Py_True || key == Py_False || key == Py_None) {
        return _encoded_const(key);
    }
    if (PyLong_Check(key)) {
        return PyLong_Type.tp_repr(key);
    }
    if (s->skipkeys) {
        *skip = 1;
        return NULL;
    }
    PyErr_Format(PyExc_TypeError,
                 "keys must be str, int, float, bool or None, "
                 "not %.100s", Py_TYPE(key)->tp_name);
    return NULL;
}

/* Advance a dict frame to the next pair, setting cur_key (the original key),
 * cur_keystr (its unescaped string form) and cur_value, all owned.  Returns
 * 1 (pair found), 0 (exhausted) or -1 (error); honours skipkeys. */
static int
iter_dict_next(PyEncoderIterObject *self, EncoderFrame *f)
{
    PyEncoderObject *enc = self->encoder;
    Py_CLEAR(f->cur_key);
    Py_CLEAR(f->cur_keystr);
    Py_CLEAR(f->cur_value);
    while (1) {
        PyObject *key = NULL, *value = NULL;
        if (f->items == NULL) {
            /* Lazy iteration over an exact dict.  The pair is claimed under
             * a critical section so a concurrent mutation cannot invalidate
             * the borrowed cursor (matching the locked one-shot path). */
            int status = 1;
            Py_BEGIN_CRITICAL_SECTION(f->container);
            if (PyDict_GET_SIZE(f->container) != f->length) {
                PyErr_SetString(PyExc_RuntimeError,
                                "dictionary changed size during iteration");
                status = -1;
            }
            else if (PyDict_Next(f->container, &f->index, &key, &value)) {
                Py_INCREF(key);
                Py_INCREF(value);
            }
            else {
                status = 0;
            }
            Py_END_CRITICAL_SECTION();
            if (status <= 0) {
                return status;
            }
        }
        else {
            if (f->index >= f->length) {
                return 0;
            }
            PyObject *item = PyList_GET_ITEM(f->items, f->index);
            f->index++;
            /* A non-dict mapping's items() may yield non-2-tuples. */
            if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
                PyErr_SetString(PyExc_ValueError,
                                "items must return 2-tuples");
                return -1;
            }
            key = Py_NewRef(PyTuple_GET_ITEM(item, 0));
            value = Py_NewRef(PyTuple_GET_ITEM(item, 1));
        }
        int skip;
        PyObject *keystr = encoder_key_to_str(enc, key, &skip);
        if (keystr == NULL) {
            Py_DECREF(key);
            Py_DECREF(value);
            if (skip) {
                continue;
            }
            return -1;
        }
        /* The pair must stay valid across yield points. */
        f->cur_key = key;
        f->cur_keystr = keystr;
        f->cur_value = value;
        return 1;
    }
}

static void
encoder_iter_dealloc(PyObject *op)
{
    PyEncoderIterObject *self = PyEncoderIterObject_CAST(op);
    PyTypeObject *tp = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    PyObject *exc = PyErr_GetRaisedException();
    while (self->stack_depth > 0) {
        if (iter_pop_frame(self) < 0) {
            PyErr_Clear();
        }
    }
    PyErr_SetRaisedException(exc);
    PyMem_Free(self->stack);
    Py_XDECREF(self->encoder);
    Py_XDECREF(self->root_obj);
    Py_XDECREF(self->indent_cache);
    PyObject_GC_Del(op);
    Py_DECREF(tp);
}

static int
encoder_iter_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyEncoderIterObject *self = PyEncoderIterObject_CAST(op);
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->encoder);
    Py_VISIT(self->root_obj);
    Py_VISIT(self->indent_cache);
    for (Py_ssize_t i = 0; i < self->stack_depth; i++) {
        EncoderFrame *f = &self->stack[i];
        Py_VISIT(f->container);
        Py_VISIT(f->items);
        Py_VISIT(f->cur_key);
        Py_VISIT(f->cur_keystr);
        Py_VISIT(f->cur_value);
        Py_VISIT(f->ident);
        Py_VISIT(f->default_idents);
        Py_VISIT(f->default_sources);
    }
    return 0;
}

static int
encoder_iter_clear(PyObject *op)
{
    PyEncoderIterObject *self = PyEncoderIterObject_CAST(op);
    PyObject *exc = PyErr_GetRaisedException();
    while (self->stack_depth > 0) {
        if (iter_pop_frame(self) < 0) {
            PyErr_Clear();
        }
    }
    PyErr_SetRaisedException(exc);
    Py_CLEAR(self->encoder);
    Py_CLEAR(self->root_obj);
    Py_CLEAR(self->indent_cache);
    return 0;
}

/* Prepend `sep` to `chunk`, consuming chunk's reference.  Returns chunk
 * unchanged if sep is NULL (no separator) or chunk is NULL (propagating an
 * error).  Returns a new reference, or NULL on error. */
static PyObject *
iter_prepend_sep(PyObject *sep, PyObject *chunk)
{
    if (sep == NULL || chunk == NULL) {
        return chunk;
    }
    PyObject *result = PyUnicode_Concat(sep, chunk);
    Py_DECREF(chunk);
    return result;
}

/* Emit the opener for a container child (at parent level lvl + 1), pushing
 * its frame.  An empty child is returned directly as "[]"/"{}".  Returns the
 * chunk (prepended with sep), or NULL+exception on error. */
static PyObject *
iter_open_child(PyEncoderIterObject *self, PyObject *child, bool child_is_list,
                Py_ssize_t lvl, PyObject *sep)
{
    Py_ssize_t length = child_is_list ? Py_SIZE(child)
                                      : PyDict_GET_SIZE(child);
    if (length == 0) {
        _Py_DECLARE_STR(empty_array, "[]");
        _Py_DECLARE_STR(empty_object, "{}");
        PyObject *empty = child_is_list ? &_Py_STR(empty_array)
                                        : &_Py_STR(empty_object);
        return iter_prepend_sep(sep, Py_NewRef(empty));
    }
    if (iter_push_frame(self, child, lvl + 1) < 0) {
        return NULL;
    }
    return iter_prepend_sep(sep, _Py_LATIN1_CHR(child_is_list ? '[' : '{'));
}

/* Resolve a non-encodable obj via default(), returning the replacement (new
 * ref).  With markers enabled, registers obj and returns its marker key in
 * *ident_out; the caller removes it once the replacement is fully encoded.
 * Returns NULL+exception on error (cleaning up any marker). */
static PyObject *
iter_resolve_default(PyEncoderObject *enc, PyObject *obj, PyObject **ident_out)
{
    PyObject *ident;
    if (json_marker_enter(enc->markers, obj, &ident) < 0) {
        return NULL;
    }
    PyObject *newobj = PyObject_CallOneArg(enc->defaultfn, obj);
    if (newobj == NULL) {
        (void)json_marker_leave(enc->markers, ident);
        return NULL;
    }
    *ident_out = ident;
    return newobj;
}

/* Encode one child value (list element or dict value) at indent level lvl,
 * prepending sep (or NULL).  Scalars return a single chunk; containers are
 * pushed as a new frame and their opener returned.  Unknown objects are
 * resolved through default(), repeatedly if needed, so a container stays
 * streamed no matter how deep the default() chain is.  Returns NULL+exception
 * on error. */
static PyObject *
iter_encode_child(PyEncoderIterObject *self, PyObject *value, Py_ssize_t lvl,
                  PyObject *sep)
{
    PyEncoderObject *enc = self->encoder;

    PyObject *scalar = iter_encode_scalar(enc, value);
    if (scalar != NULL) {
        return iter_prepend_sep(sep, scalar);
    }
    if (PyErr_Occurred()) {
        return NULL;
    }

    bool is_list = PyList_Check(value) || PyTuple_Check(value);
    if (is_list || PyAnyDict_Check(value)) {
        return iter_open_child(self, value, is_list, lvl, sep);
    }

    /* Unknown object: resolve through default().  Every resolved object
     * stays registered in markers until its replacement is fully encoded,
     * so cycles through default() are detected. */
    PyObject *idents = NULL;    /* marker keys of the chain, or NULL */
    PyObject *sources = NULL;   /* objects handed to default(), for notes */
    PyObject *cur = Py_NewRef(value);
    PyObject *chunk = NULL;

    for (;;) {
        if (sources != NULL
            && PyList_GET_SIZE(sources) >= Py_GetRecursionLimit()) {
            PyErr_SetString(PyExc_RecursionError,
                            "maximum recursion depth exceeded "
                            "while encoding a JSON object");
            goto error;
        }
        PyObject *ident = NULL;
        PyObject *newobj = iter_resolve_default(enc, cur, &ident);
        if (newobj == NULL) {
            goto error;
        }
        if (ident != NULL) {
            int r = -1;
            if (idents != NULL || (idents = PyList_New(0)) != NULL) {
                r = PyList_Append(idents, ident);
            }
            if (r < 0) {
                (void)json_marker_leave(enc->markers, ident);
                Py_DECREF(newobj);
                goto error;
            }
            Py_DECREF(ident);
        }
        if (sources == NULL && (sources = PyList_New(0)) == NULL) {
            Py_DECREF(newobj);
            goto error;
        }
        if (PyList_Append(sources, cur) < 0) {
            Py_DECREF(newobj);
            goto error;
        }
        Py_SETREF(cur, newobj);

        scalar = iter_encode_scalar(enc, cur);
        if (scalar != NULL) {
            chunk = iter_prepend_sep(sep, scalar);
            if (chunk == NULL) {
                goto error;
            }
            break;
        }
        if (PyErr_Occurred()) {
            goto error;
        }
        is_list = PyList_Check(cur) || PyTuple_Check(cur);
        if (is_list || PyAnyDict_Check(cur)) {
            Py_ssize_t depth_before = self->stack_depth;
            chunk = iter_open_child(self, cur, is_list, lvl, sep);
            if (chunk == NULL) {
                goto error;
            }
            if (self->stack_depth > depth_before) {
                /* The frame owns the chain: markers are removed and error
                 * notes attached when it is popped. */
                EncoderFrame *f = &self->stack[self->stack_depth - 1];
                f->default_idents = idents;
                f->default_sources = sources;
                Py_DECREF(cur);
                return chunk;
            }
            break;      /* empty container: done in this chunk */
        }
        /* Still not encodable: run default() on the result again. */
    }

    /* Fully encoded in this chunk: drop the chain markers now. */
    if (iter_leave_chain(enc, idents) < 0) {
        Py_CLEAR(chunk);
        goto error;
    }
    Py_XDECREF(idents);
    Py_XDECREF(sources);
    Py_DECREF(cur);
    return chunk;

error:
    /* One note per chain hop whose replacement failed, innermost first,
     * matching the one-shot encoder. */
    if (sources != NULL) {
        for (Py_ssize_t i = PyList_GET_SIZE(sources) - 1; i >= 0; i--) {
            _PyErr_FormatNote("when serializing %T object",
                              PyList_GET_ITEM(sources, i));
        }
    }
    (void)iter_leave_chain(enc, idents);
    Py_XDECREF(idents);
    Py_XDECREF(sources);
    Py_DECREF(cur);
    return NULL;
}

/* Compute the separator/indent prefix to emit before frame f's next item,
 * updating f->first.  On success *prefix is borrowed (from enc or the indent
 * cache) or NULL when nothing precedes the item.  Returns 0, or -1 with an
 * exception set. */
static int
iter_item_prefix(PyEncoderIterObject *self, EncoderFrame *f, PyObject **prefix)
{
    PyEncoderObject *enc = self->encoder;
    *prefix = NULL;
    if (enc->indent == Py_None) {
        if (!f->first) {
            *prefix = enc->item_separator;
        }
        f->first = false;
        return 0;
    }
    if (f->first) {
        f->first = false;
        *prefix = get_newline_indent(enc, f->indent_level + 1,
                                     self->indent_cache);
    }
    else {
        *prefix = get_item_separator(enc, f->indent_level + 1,
                                     self->indent_cache);
    }
    return *prefix == NULL ? -1 : 0;
}

/* Process one step of the top-of-stack list frame. */
static PyObject *
iter_handle_list_frame(PyEncoderIterObject *self, EncoderFrame *f)
{
    PyEncoderObject *enc = self->encoder;
    Py_ssize_t lvl = f->indent_level;

    /* Use the live size (the list may shrink between yields) and claim the
     * item under a critical section: user code run later can mutate the
     * sequence (gh-142831), and another thread can mutate it concurrently. */
    PyObject *item = NULL;
    Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST(f->items);
    if (f->index < PySequence_Fast_GET_SIZE(f->items)) {
        item = Py_NewRef(PySequence_Fast_GET_ITEM(f->items, f->index));
        f->index++;
    }
    Py_END_CRITICAL_SECTION_SEQUENCE_FAST();

    if (item == NULL) {
        bool had_items = !f->first;
        if (iter_pop_frame(self) < 0) {
            return NULL;
        }
        if (enc->indent != Py_None && had_items) {
            PyObject *ni = get_newline_indent(enc, lvl, self->indent_cache);
            if (ni == NULL) {
                return NULL;
            }
            return PyUnicode_Concat(ni, _Py_LATIN1_CHR(']'));
        }
        return _Py_LATIN1_CHR(']');
    }

    PyObject *sep;
    if (iter_item_prefix(self, f, &sep) < 0) {
        Py_DECREF(item);
        return NULL;
    }
    PyObject *chunk = iter_encode_child(self, item, lvl, sep);
    Py_DECREF(item);
    return chunk;
}

/* Process one step of the top-of-stack dict frame (a key+separator chunk,
 * then the value on the next step). */
static PyObject *
iter_handle_dict_frame(PyEncoderIterObject *self, EncoderFrame *f)
{
    PyEncoderObject *enc = self->encoder;
    Py_ssize_t lvl = f->indent_level;

    if (f->need_value) {
        PyObject *value = Py_NewRef(f->cur_value);
        f->need_value = false;
        PyObject *chunk = iter_encode_child(self, value, lvl, NULL);
        Py_DECREF(value);
        return chunk;
    }

    int has_next = iter_dict_next(self, f);
    if (has_next < 0) {
        return NULL;
    }
    if (!has_next) {
        bool had_items = !f->first;
        if (iter_pop_frame(self) < 0) {
            return NULL;
        }
        if (enc->indent != Py_None && had_items) {
            PyObject *ni = get_newline_indent(enc, lvl, self->indent_cache);
            if (ni == NULL) {
                return NULL;
            }
            return PyUnicode_Concat(ni, _Py_LATIN1_CHR('}'));
        }
        return _Py_LATIN1_CHR('}');
    }

    /* Chunk: [prefix +] quoted key + key_separator */
    PyObject *keystr = encoder_encode_string(enc, f->cur_keystr);
    if (keystr == NULL) {
        /* The one-shot encoder attaches no item note for a failing key. */
        Py_CLEAR(f->cur_key);
        Py_CLEAR(f->cur_keystr);
        Py_CLEAR(f->cur_value);
        return NULL;
    }
    PyObject *prefix;
    if (iter_item_prefix(self, f, &prefix) < 0) {
        Py_DECREF(keystr);
        return NULL;
    }
    PyObject *chunk = iter_prepend_sep(prefix, keystr);
    if (chunk == NULL) {
        return NULL;
    }
    PyObject *result = PyUnicode_Concat(chunk, enc->key_separator);
    Py_DECREF(chunk);
    f->need_value = true;
    return result;
}

/* Walk the frame stack innermost-first and attach a contextual error note for
 * each in-progress container, matching the one-shot encoder. */
static void
iter_attach_notes(PyEncoderIterObject *self)
{
    for (Py_ssize_t i = self->stack_depth - 1; i >= 0; i--) {
        EncoderFrame *f = &self->stack[i];
        if (f->kind == FRAME_LIST) {
            /* The in-progress element: index was advanced past it. */
            _PyErr_FormatNote("when serializing %T item %zd",
                              f->container, f->index - 1);
        }
        else if (f->cur_key != NULL) {
            /* cur_key is NULL when advancing the frame itself failed. */
            _PyErr_FormatNote("when serializing %T item %R",
                              f->container, f->cur_key);
        }
        if (f->default_sources != NULL) {
            for (Py_ssize_t j = PyList_GET_SIZE(f->default_sources) - 1;
                 j >= 0; j--) {
                _PyErr_FormatNote("when serializing %T object",
                                  PyList_GET_ITEM(f->default_sources, j));
            }
        }
    }
}

/* Produce the next token chunk, or NULL: with an exception set on error,
 * without one when the document is complete. */
static PyObject *
iter_step(PyEncoderIterObject *self)
{
    if (self->root_obj != NULL) {
        PyObject *root = self->root_obj;
        self->root_obj = NULL;
        /* Dispatch the root as a child at level -1, so a container frame
         * lands at level 0. */
        PyObject *chunk = iter_encode_child(self, root, -1, NULL);
        Py_DECREF(root);
        return chunk;
    }
    if (self->stack_depth > 0) {
        EncoderFrame *f = &self->stack[self->stack_depth - 1];
        if (f->kind == FRAME_LIST) {
            return iter_handle_list_frame(self, f);
        }
        return iter_handle_dict_frame(self, f);
    }
    return NULL;
}

/* Token chunks are buffered up to roughly this many characters per yield:
 * yielding every token would cost an allocation and a Python-level fp.write()
 * call per scalar. */
#define ITER_CHUNK_SIZE 8192

static PyObject *
encoder_iter_iternext(PyObject *op)
{
    PyEncoderIterObject *self = PyEncoderIterObject_CAST(op);

    if (self->running) {
        PyErr_SetString(PyExc_ValueError,
                        "encoder iterator already executing");
        return NULL;
    }
    if (self->root_obj == NULL && self->stack_depth == 0) {
        return NULL;    /* exhausted */
    }
    self->running = true;

    PyUnicodeWriter *writer = NULL;
    Py_ssize_t buffered = 0;
    for (;;) {
        PyObject *chunk = iter_step(self);
        if (chunk == NULL) {
            if (PyErr_Occurred()) {
                goto error;
            }
            break;      /* complete: flush what is buffered */
        }
        if (writer == NULL) {
            if (self->root_obj == NULL && self->stack_depth == 0) {
                /* The whole document fit in this one chunk. */
                self->running = false;
                return chunk;
            }
            writer = PyUnicodeWriter_Create(ITER_CHUNK_SIZE);
            if (writer == NULL) {
                Py_DECREF(chunk);
                goto error;
            }
        }
        buffered += PyUnicode_GET_LENGTH(chunk);
        int w = PyUnicodeWriter_WriteStr(writer, chunk);
        Py_DECREF(chunk);
        if (w < 0) {
            goto error;
        }
        if (buffered >= ITER_CHUNK_SIZE) {
            break;
        }
    }
    self->running = false;
    if (writer == NULL) {
        return NULL;    /* completed with nothing left to yield */
    }
    return PyUnicodeWriter_Finish(writer);

error:
    /* Match generator behavior: an error permanently exhausts the iterator.
     * Attach the contextual notes before unwinding the frames they need. */
    iter_attach_notes(self);
    PyObject *exc = PyErr_GetRaisedException();
    while (self->stack_depth > 0) {
        if (iter_pop_frame(self) < 0) {
            PyErr_Clear();
        }
    }
    Py_CLEAR(self->root_obj);
    PyErr_SetRaisedException(exc);
    if (writer != NULL) {
        PyUnicodeWriter_Discard(writer);
    }
    self->running = false;
    return NULL;
}

PyDoc_STRVAR(encoder_iter_close_doc,
             "close()\n"
             "--\n"
             "\n"
             "Release the resources held by the iterator and mark it exhausted,\n"
             "like generator.close().  Further next() calls raise StopIteration.");

static PyObject *
encoder_iter_close(PyObject *op, PyObject *Py_UNUSED(ignored))
{
    PyEncoderIterObject *self = PyEncoderIterObject_CAST(op);
    if (self->running) {
        PyErr_SetString(PyExc_ValueError,
                        "encoder iterator already executing");
        return NULL;
    }
    PyObject *exc = PyErr_GetRaisedException();
    while (self->stack_depth > 0) {
        if (iter_pop_frame(self) < 0) {
            PyErr_Clear();
        }
    }
    Py_CLEAR(self->root_obj);
    PyErr_SetRaisedException(exc);
    Py_RETURN_NONE;
}

static PyMethodDef encoder_iter_methods[] = {
    {"close", encoder_iter_close, METH_NOARGS, encoder_iter_close_doc},
    {NULL, NULL}
};

static PyType_Slot PyEncoderIterType_slots[] = {
    {Py_tp_dealloc, encoder_iter_dealloc},
    {Py_tp_traverse, encoder_iter_traverse},
    {Py_tp_clear, encoder_iter_clear},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, encoder_iter_iternext},
    {Py_tp_methods, encoder_iter_methods},
    {0, 0}
};

static PyType_Spec PyEncoderIterType_spec = {
    .name = "_json.EncoderIter",
    .basicsize = sizeof(PyEncoderIterObject),
    .itemsize = 0,
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC
              | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION),
    .slots = PyEncoderIterType_slots,
};

static PyObject *
encoder_iter_new(PyEncoderObject *encoder, PyObject *obj, Py_ssize_t indent_level)
{
    PyObject *module = PyType_GetModule(Py_TYPE(encoder));
    if (module == NULL) {
        return NULL;
    }
    PyTypeObject *iter_type = get_json_state(module)->EncoderIterType;
    PyEncoderIterObject *self = PyObject_GC_New(PyEncoderIterObject, iter_type);
    if (self == NULL) {
        return NULL;
    }
    /* Initialize all fields before the first failable call: the
     * deallocator walks them on failure. */
    self->encoder = (PyEncoderObject *)Py_NewRef(encoder);
    self->root_obj = Py_NewRef(obj);
    self->stack = NULL;
    self->stack_depth = 0;
    self->stack_alloc = 0;
    self->running = false;
    self->indent_cache = NULL;
    if (encoder->indent != Py_None) {
        self->indent_cache = create_indent_cache(encoder, indent_level);
        if (self->indent_cache == NULL) {
            Py_DECREF(self);
            return NULL;
        }
    }
    PyObject_GC_Track(self);
    return (PyObject *)self;
}

static PyObject *
encoder_iterencode(PyObject *op, PyObject *const *args, Py_ssize_t nargs)
{
    /* Return a streaming C iterator that yields JSON string chunks. */
    if (nargs != 2) {
        PyErr_Format(PyExc_TypeError,
                     "_iterencode expected 2 arguments, got %zd", nargs);
        return NULL;
    }
    Py_ssize_t indent_level = PyLong_AsSsize_t(args[1]);
    if (indent_level == -1 && PyErr_Occurred()) {
        return NULL;
    }
    return encoder_iter_new(PyEncoderObject_CAST(op), args[0], indent_level);
}

static PyObject *
encoder_call(PyObject *op, PyObject *args, PyObject *kwds)
{
    /* Python callable interface to encoder_listencode_obj */
    static char *kwlist[] = {"obj", "_current_indent_level", NULL};
    PyObject *obj;
    Py_ssize_t indent_level;
    PyEncoderObject *self = PyEncoderObject_CAST(op);

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "On:_iterencode", kwlist,
                                     &obj, &indent_level))
        return NULL;

    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }

    PyObject *indent_cache = NULL;
    if (self->indent != Py_None) {
        indent_cache = create_indent_cache(self, indent_level);
        if (indent_cache == NULL) {
            PyUnicodeWriter_Discard(writer);
            return NULL;
        }
    }
    indent_level = 0;
    if (encoder_listencode_obj(self, writer, obj, indent_level, indent_cache)) {
        PyUnicodeWriter_Discard(writer);
        Py_XDECREF(indent_cache);
        return NULL;
    }
    Py_XDECREF(indent_cache);

    PyObject *str = PyUnicodeWriter_Finish(writer);
    if (str == NULL) {
        return NULL;
    }
    PyObject *result = PyTuple_Pack(1, str);
    Py_DECREF(str);
    return result;
}

static PyObject *
_encoded_const(PyObject *obj)
{
    /* Return the JSON string representation of None, True, False */
    if (obj == Py_None) {
        return &_Py_ID(null);
    }
    else if (obj == Py_True) {
        return &_Py_ID(true);
    }
    else if (obj == Py_False) {
        return &_Py_ID(false);
    }
    else {
        PyErr_SetString(PyExc_ValueError, "not a const");
        return NULL;
    }
}

static PyObject *
encoder_encode_float(PyEncoderObject *s, PyObject *obj)
{
    /* Return the JSON representation of a PyFloat. */
    double i = PyFloat_AS_DOUBLE(obj);
    if (!isfinite(i)) {
        if (!s->allow_nan) {
            PyErr_Format(
                    PyExc_ValueError,
                    "Out of range float values are not JSON compliant: %R",
                    obj
                    );
            return NULL;
        }
        if (i > 0) {
            return PyUnicode_FromString("Infinity");
        }
        else if (i < 0) {
            return PyUnicode_FromString("-Infinity");
        }
        else {
            return PyUnicode_FromString("NaN");
        }
    }
    return PyFloat_Type.tp_repr(obj);
}

static int
encoder_write_string(PyEncoderObject *s, PyUnicodeWriter *writer, PyObject *obj)
{
    /* Return the JSON representation of a string */
    PyObject *encoded;

    if (s->fast_encode) {
        return s->fast_encode(writer, obj);
    }
    encoded = PyObject_CallOneArg(s->encoder, obj);
    if (encoded == NULL) {
        return -1;
    }
    if (!PyUnicode_Check(encoded)) {
        PyErr_Format(PyExc_TypeError,
                     "encoder() must return a string, not %.80s",
                     Py_TYPE(encoded)->tp_name);
        Py_DECREF(encoded);
        return -1;
    }
    return _steal_accumulate(writer, encoded);
}

static int
_steal_accumulate(PyUnicodeWriter *writer, PyObject *stolen)
{
    /* Append stolen and then decrement its reference count */
    int rval = PyUnicodeWriter_WriteStr(writer, stolen);
    Py_DECREF(stolen);
    return rval;
}

static int
encoder_listencode_obj(PyEncoderObject *s, PyUnicodeWriter *writer,
                       PyObject *obj,
                       Py_ssize_t indent_level, PyObject *indent_cache)
{
    /* Encode Python object obj to a JSON term */
    PyObject *newobj;
    int rv;

    if (obj == Py_None) {
      return PyUnicodeWriter_WriteASCII(writer, "null", 4);
    }
    else if (obj == Py_True) {
      return PyUnicodeWriter_WriteASCII(writer, "true", 4);
    }
    else if (obj == Py_False) {
      return PyUnicodeWriter_WriteASCII(writer, "false", 5);
    }
    else if (PyUnicode_Check(obj)) {
        return encoder_write_string(s, writer, obj);
    }
    else if (PyLong_Check(obj)) {
        if (PyLong_CheckExact(obj)) {
            // Fast-path for exact integers
            return PyUnicodeWriter_WriteRepr(writer, obj);
        }
        PyObject *encoded = PyLong_Type.tp_repr(obj);
        if (encoded == NULL)
            return -1;
        return _steal_accumulate(writer, encoded);
    }
    else if (PyFloat_Check(obj)) {
        PyObject *encoded = encoder_encode_float(s, obj);
        if (encoded == NULL)
            return -1;
        return _steal_accumulate(writer, encoded);
    }
    else if (PyList_Check(obj) || PyTuple_Check(obj)) {
        if (_Py_EnterRecursiveCall(" while encoding a JSON object"))
            return -1;
        rv = encoder_listencode_list(s, writer, obj, indent_level, indent_cache);
        _Py_LeaveRecursiveCall();
        return rv;
    }
    else if (PyAnyDict_Check(obj)) {
        if (_Py_EnterRecursiveCall(" while encoding a JSON object"))
            return -1;
        rv = encoder_listencode_dict(s, writer, obj, indent_level, indent_cache);
        _Py_LeaveRecursiveCall();
        return rv;
    }
    else {
        PyObject *ident;
        if (json_marker_enter(s->markers, obj, &ident) < 0) {
            return -1;
        }
        newobj = PyObject_CallOneArg(s->defaultfn, obj);
        if (newobj == NULL) {
            Py_XDECREF(ident);
            return -1;
        }

        if (_Py_EnterRecursiveCall(" while encoding a JSON object")) {
            Py_DECREF(newobj);
            Py_XDECREF(ident);
            return -1;
        }
        rv = encoder_listencode_obj(s, writer, newobj, indent_level, indent_cache);
        _Py_LeaveRecursiveCall();

        Py_DECREF(newobj);
        if (rv) {
            _PyErr_FormatNote("when serializing %T object", obj);
            Py_XDECREF(ident);
            return -1;
        }
        return json_marker_leave(s->markers, ident);
    }
}

static int
encoder_encode_key_value(PyEncoderObject *s, PyUnicodeWriter *writer, bool *first,
                         PyObject *dct, PyObject *key, PyObject *value,
                         Py_ssize_t indent_level, PyObject *indent_cache,
                         PyObject *item_separator)
{
    int rv, skip;

    PyObject *keystr = encoder_key_to_str(s, key, &skip);
    if (keystr == NULL) {
        return skip ? 0 : -1;
    }

    if (*first) {
        *first = false;
        if (s->indent != Py_None) {
            if (write_newline_indent(writer, indent_level, indent_cache) < 0) {
                Py_DECREF(keystr);
                return -1;
            }
        }
    }
    else {
        if (PyUnicodeWriter_WriteStr(writer, item_separator) < 0) {
            Py_DECREF(keystr);
            return -1;
        }
    }

    rv = encoder_write_string(s, writer, keystr);
    Py_DECREF(keystr);

    if (rv < 0) {
        return -1;
    }
    if (PyUnicodeWriter_WriteStr(writer, s->key_separator) < 0) {
        return -1;
    }
    if (encoder_listencode_obj(s, writer, value, indent_level, indent_cache) < 0) {
        _PyErr_FormatNote("when serializing %T item %R", dct, key);
        return -1;
    }
    return 0;
}

static inline int
_encoder_iterate_mapping_lock_held(PyEncoderObject *s, PyUnicodeWriter *writer,
                            bool *first, PyObject *dct, PyObject *items,
                            Py_ssize_t indent_level, PyObject *indent_cache,
                            PyObject *separator)
{
    _Py_CRITICAL_SECTION_ASSERT_OBJECT_LOCKED(items);
    PyObject *key, *value;
    for (Py_ssize_t  i = 0; i < PyList_GET_SIZE(items); i++) {
        PyObject *item = PyList_GET_ITEM(items, i);
        // gh-142831: encoder_encode_key_value() can invoke user code
        // that mutates the items list, invalidating this borrowed ref.
        Py_INCREF(item);
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            PyErr_SetString(PyExc_ValueError, "items must return 2-tuples");
            Py_DECREF(item);
            return -1;
        }

        key = PyTuple_GET_ITEM(item, 0);
        value = PyTuple_GET_ITEM(item, 1);
        if (encoder_encode_key_value(s, writer, first, dct, key, value,
                                     indent_level, indent_cache,
                                     separator) < 0) {
            Py_DECREF(item);
            return -1;
        }
        Py_DECREF(item);
    }

    return 0;
}

static inline int
_encoder_iterate_dict_lock_held(PyEncoderObject *s, PyUnicodeWriter *writer,
                         bool *first, PyObject *dct, Py_ssize_t indent_level,
                         PyObject *indent_cache, PyObject *separator)
{
    _Py_CRITICAL_SECTION_ASSERT_OBJECT_LOCKED(dct);
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dct, &pos, &key, &value)) {
        // gh-145244: encoder_encode_key_value() can invoke user code
        // that mutates the dict, invalidating these borrowed refs.
        Py_INCREF(key);
        Py_INCREF(value);
        if (encoder_encode_key_value(s, writer, first, dct, key, value,
                                    indent_level, indent_cache,
                                    separator) < 0) {
            Py_DECREF(key);
            Py_DECREF(value);
            return -1;
        }
        Py_DECREF(key);
        Py_DECREF(value);
    }
    return 0;
}

static int
encoder_listencode_dict(PyEncoderObject *s, PyUnicodeWriter *writer,
                       PyObject *dct,
                       Py_ssize_t indent_level, PyObject *indent_cache)
{
    /* Encode Python dict dct a JSON term */
    PyObject *ident = NULL;
    bool first = true;

    if (PyDict_GET_SIZE(dct) == 0) {
        /* Fast path */
        return PyUnicodeWriter_WriteASCII(writer, "{}", 2);
    }

    if (json_marker_enter(s->markers, dct, &ident) < 0) {
        goto bail;
    }

    if (PyUnicodeWriter_WriteChar(writer, '{')) {
        goto bail;
    }

    PyObject *separator = s->item_separator; // borrowed reference
    if (s->indent != Py_None) {
        indent_level++;
        separator = get_item_separator(s, indent_level, indent_cache);
        if (separator == NULL)
            goto bail;
    }

    if (s->sort_keys || !PyAnyDict_CheckExact(dct)) {
        PyObject *items = PyMapping_Items(dct);
        if (items == NULL || (s->sort_keys && PyList_Sort(items) < 0)) {
            Py_XDECREF(items);
            goto bail;
        }

        int result;
        Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST(items);
        result = _encoder_iterate_mapping_lock_held(s, writer, &first, dct,
                    items, indent_level, indent_cache, separator);
        Py_END_CRITICAL_SECTION_SEQUENCE_FAST();
        Py_DECREF(items);
        if (result < 0) {
            goto bail;
        }

    } else {
        int result;
        Py_BEGIN_CRITICAL_SECTION(dct);
        result = _encoder_iterate_dict_lock_held(s, writer, &first, dct,
                            indent_level, indent_cache, separator);
        Py_END_CRITICAL_SECTION();
        if (result < 0) {
            goto bail;
        }
    }

    {
        int leave_rv = json_marker_leave(s->markers, ident);
        ident = NULL;
        if (leave_rv < 0) {
            goto bail;
        }
    }
    if (s->indent != Py_None && !first) {
        indent_level--;
        if (write_newline_indent(writer, indent_level, indent_cache) < 0) {
            goto bail;
        }
    }

    if (PyUnicodeWriter_WriteChar(writer, '}')) {
        goto bail;
    }
    return 0;

bail:
    Py_XDECREF(ident);
    return -1;
}

static inline int
_encoder_iterate_fast_seq_lock_held(PyEncoderObject *s, PyUnicodeWriter *writer,
    PyObject *seq, PyObject *s_fast,
    Py_ssize_t indent_level, PyObject *indent_cache, PyObject *separator)
{
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(s_fast); i++) {
        PyObject *obj = PySequence_Fast_GET_ITEM(s_fast, i);
        // gh-142831: encoder_listencode_obj() can invoke user code
        // that mutates the sequence, invalidating this borrowed ref.
        Py_INCREF(obj);
        if (i) {
            if (PyUnicodeWriter_WriteStr(writer, separator) < 0) {
                Py_DECREF(obj);
                return -1;
            }
        }
        if (encoder_listencode_obj(s, writer, obj, indent_level, indent_cache)) {
            _PyErr_FormatNote("when serializing %T item %zd", seq, i);
            Py_DECREF(obj);
            return -1;
        }
        Py_DECREF(obj);
    }
    return 0;
}

static int
encoder_listencode_list(PyEncoderObject *s, PyUnicodeWriter *writer,
                        PyObject *seq,
                        Py_ssize_t indent_level, PyObject *indent_cache)
{
    PyObject *ident = NULL;
    PyObject *s_fast = NULL;

    s_fast = PySequence_Fast(seq, "encoder_listencode_list needs a sequence");
    if (s_fast == NULL)
        return -1;
    if (PySequence_Fast_GET_SIZE(s_fast) == 0) {
        Py_DECREF(s_fast);
        return PyUnicodeWriter_WriteASCII(writer, "[]", 2);
    }

    if (json_marker_enter(s->markers, seq, &ident) < 0) {
        goto bail;
    }

    if (PyUnicodeWriter_WriteChar(writer, '[')) {
        goto bail;
    }

    PyObject *separator = s->item_separator; // borrowed reference
    if (s->indent != Py_None) {
        indent_level++;
        separator = get_item_separator(s, indent_level, indent_cache);
        if (separator == NULL ||
            write_newline_indent(writer, indent_level, indent_cache) < 0)
        {
            goto bail;
        }
    }
    int result;
    Py_BEGIN_CRITICAL_SECTION_SEQUENCE_FAST(seq);
    result = _encoder_iterate_fast_seq_lock_held(s, writer, seq, s_fast,
                     indent_level, indent_cache, separator);
    Py_END_CRITICAL_SECTION_SEQUENCE_FAST();
    if (result < 0) {
        goto bail;
    }
    {
        int leave_rv = json_marker_leave(s->markers, ident);
        ident = NULL;
        if (leave_rv < 0) {
            goto bail;
        }
    }

    if (s->indent != Py_None) {
        indent_level--;
        if (write_newline_indent(writer, indent_level, indent_cache) < 0) {
            goto bail;
        }
    }

    if (PyUnicodeWriter_WriteChar(writer, ']')) {
        goto bail;
    }
    Py_DECREF(s_fast);
    return 0;

bail:
    Py_XDECREF(ident);
    Py_DECREF(s_fast);
    return -1;
}

static void
encoder_dealloc(PyObject *self)
{
    PyTypeObject *tp = Py_TYPE(self);
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyObject_GC_UnTrack(self);
    (void)encoder_clear(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static int
encoder_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyEncoderObject *self = PyEncoderObject_CAST(op);
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->markers);
    Py_VISIT(self->defaultfn);
    Py_VISIT(self->encoder);
    Py_VISIT(self->indent);
    Py_VISIT(self->key_separator);
    Py_VISIT(self->item_separator);
    return 0;
}

static int
encoder_clear(PyObject *op)
{
    PyEncoderObject *self = PyEncoderObject_CAST(op);
    /* Deallocate Encoder */
    Py_CLEAR(self->markers);
    Py_CLEAR(self->defaultfn);
    Py_CLEAR(self->encoder);
    Py_CLEAR(self->indent);
    Py_CLEAR(self->key_separator);
    Py_CLEAR(self->item_separator);
    return 0;
}

PyDoc_STRVAR(encoder_doc, "Encoder(markers, default, encoder, indent, key_separator, item_separator, sort_keys, skipkeys, allow_nan)");

static PyMethodDef encoder_methods[] = {
    {"_iterencode", _PyCFunction_CAST(encoder_iterencode), METH_FASTCALL,
     "_iterencode(obj, _current_indent_level)\n--\n\n"
     "Return an iterator yielding the JSON encoding of obj as string chunks."},
    {NULL, NULL}
};

static PyType_Slot PyEncoderType_slots[] = {
    {Py_tp_doc, (void *)encoder_doc},
    {Py_tp_dealloc, encoder_dealloc},
    {Py_tp_call, encoder_call},
    {Py_tp_traverse, encoder_traverse},
    {Py_tp_clear, encoder_clear},
    {Py_tp_members, encoder_members},
    {Py_tp_methods, encoder_methods},
    {Py_tp_new, encoder_new},
    {0, 0}
};

static PyType_Spec PyEncoderType_spec = {
    .name = "_json.Encoder",
    .basicsize = sizeof(PyEncoderObject),
    .itemsize = 0,
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .slots = PyEncoderType_slots
};

static PyMethodDef speedups_methods[] = {
    PY_ENCODE_BASESTRING_ASCII_METHODDEF
    PY_ENCODE_BASESTRING_METHODDEF
    PY_SCANSTRING_METHODDEF
    {NULL, NULL, 0, NULL}
};

PyDoc_STRVAR(module_doc,
"json speedups\n");

static int
_json_exec(PyObject *module)
{
    PyObject *PyScannerType = PyType_FromSpec(&PyScannerType_spec);
    if (PyModule_Add(module, "make_scanner", PyScannerType) < 0) {
        return -1;
    }

    /* Created with the module so encoder_iter_new() can recover it via
     * PyType_GetModuleByDef(). */
    PyObject *PyEncoderType = PyType_FromModuleAndSpec(module,
                                                       &PyEncoderType_spec, NULL);
    if (PyModule_Add(module, "make_encoder", PyEncoderType) < 0) {
        return -1;
    }

    PyObject *PyEncoderIterType = PyType_FromModuleAndSpec(module,
                                                           &PyEncoderIterType_spec,
                                                           NULL);
    if (PyEncoderIterType == NULL) {
        return -1;
    }
    get_json_state(module)->EncoderIterType =
        (PyTypeObject *)Py_NewRef(PyEncoderIterType);
    if (PyModule_Add(module, "_EncoderIter", PyEncoderIterType) < 0) {
        return -1;
    }

    return 0;
}

static int
_json_traverse(PyObject *module, visitproc visit, void *arg)
{
    Py_VISIT(get_json_state(module)->EncoderIterType);
    return 0;
}

static int
_json_clear(PyObject *module)
{
    Py_CLEAR(get_json_state(module)->EncoderIterType);
    return 0;
}

static void
_json_free(void *module)
{
    (void)_json_clear((PyObject *)module);
}

static PyModuleDef_Slot _json_slots[] = {
    _Py_ABI_SLOT,
    {Py_mod_exec, _json_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef jsonmodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_json",
    .m_doc = module_doc,
    .m_size = sizeof(_jsonstate),
    .m_methods = speedups_methods,
    .m_slots = _json_slots,
    .m_traverse = _json_traverse,
    .m_clear = _json_clear,
    .m_free = _json_free,
};

PyMODINIT_FUNC
PyInit__json(void)
{
    return PyModuleDef_Init(&jsonmodule);
}
