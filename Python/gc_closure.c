/* Closure-sum cycle detection: the parts shared by the default-build
 * collector (gc.c) and the free-threaded collector (gc_free_threading.c):
 * configuration, statistics and the gc._closure_* helpers.  The detectors
 * themselves are closure_pass() in the two files; see the comment there.
 *
 * PYTHON_GC_CLOSURE selects the detector (enum _PyGC_ClosureMode) and
 * PYTHON_GC_CLOSURE_STATS names a file that receives one JSON line of
 * statistics per process at exit.  Both are read with getenv() on purpose:
 * they must reach subprocesses too, and -E/-I would hide them.
 */

#include "Python.h"
#include "pycore_gc.h"
#include "pycore_interp.h"        // PyInterpreterState.gc
#include "pycore_pymem.h"         // _PyMem_RawStrdup()

#include <stdlib.h>               // getenv(), atoi()
#include <string.h>               // strncpy()

void
_PyGC_ClosureInit(struct _gc_runtime_state *gcstate)
{
    gcstate->closure_mode = _PyGC_CLOSURE_OFF;
    gcstate->closure_counter = 0;
    gcstate->closure_stats = NULL;
    gcstate->closure_stats_path = NULL;

    const char *mode = getenv("PYTHON_GC_CLOSURE");
    if (mode != NULL && *mode != '\0') {
        int value = atoi(mode);
        if (value >= _PyGC_CLOSURE_OFF && value <= _PyGC_CLOSURE_OLD) {
            gcstate->closure_mode = value;
        }
    }
    const char *mark = getenv("PYTHON_GC_MARK");
    gcstate->closure_mark_alive = (mark != NULL && atoi(mark) != 0);
    const char *path = getenv("PYTHON_GC_CLOSURE_STATS");
    int want_path = (path != NULL && *path != '\0');
    if (want_path || gcstate->closure_mode == _PyGC_CLOSURE_TRUTH) {
        gcstate->closure_stats = PyMem_RawCalloc(1, sizeof(struct _gc_closure_stats));
        if (gcstate->closure_stats == NULL) {
            gcstate->closure_mode = _PyGC_CLOSURE_OFF;
            return;
        }
        if (want_path) {
            gcstate->closure_stats_path = _PyMem_RawStrdup(path);
        }
    }
}

int
_PyGC_ClosureWanted(struct _gc_runtime_state *gcstate, int generation)
{
    switch (gcstate->closure_mode) {
    case _PyGC_CLOSURE_ALL:
        return 1;
    case _PyGC_CLOSURE_YOUNG:
        return generation < NUM_GENERATIONS - 1;
    case _PyGC_CLOSURE_OLD:
        return generation == NUM_GENERATIONS - 1;
    case _PyGC_CLOSURE_ALTERNATE:
        return (gcstate->closure_counter++ & 1) != 0;
    default:
        return 0;
    }
}

struct _gc_closure_gen_stats *
_PyGC_ClosureGenStats(struct _gc_runtime_state *gcstate, int generation)
{
    if (gcstate->closure_stats == NULL) {
        return NULL;
    }
    assert(generation >= 0 && generation < NUM_GENERATIONS);
    struct _gc_closure_gen_stats *st = &gcstate->closure_stats->gens[generation];
    st->collections++;
    return st;
}

static inline int
closure_size_bucket(Py_ssize_t n)
{
    int b = 0;
    while (n > 1 && b < _PyGC_CLOSURE_HIST - 1) {
        n >>= 1;
        b++;
    }
    return b;
}

void
_PyGC_ClosureAccount(struct _gc_closure_gen_stats *st, Py_ssize_t size,
                     Py_ssize_t rc_sum, Py_ssize_t edges, int pure)
{
    st->closures++;
    st->edges += edges;
    st->size_hist[closure_size_bucket(size)]++;
    if (pure) {
        st->pure_closures++;
        st->pure_objects += size;
        st->pure_size_hist[closure_size_bucket(size)]++;
    }
    if (rc_sum < edges) {
        st->phantom_closures++;
    }
    if (size > (Py_ssize_t)st->queue_high_water) {
        st->queue_high_water = size;
    }
}

void
_PyGC_ClosureCountType(struct _gc_closure_type_count *table, PyTypeObject *tp)
{
    const char *name = tp->tp_name;
    int i;
    for (i = 0; i < _PyGC_CLOSURE_TYPES - 1; i++) {
        if (table[i].count == 0) {
            strncpy(table[i].name, name, sizeof(table[i].name) - 1);
            table[i].name[sizeof(table[i].name) - 1] = '\0';
            break;
        }
        if (strncmp(table[i].name, name, sizeof(table[i].name) - 1) == 0) {
            break;
        }
    }
    if (i == _PyGC_CLOSURE_TYPES - 1 && table[i].count == 0) {
        strcpy(table[i].name, "<other>");
    }
    table[i].count++;
}

/* Raw allocation: the free-threaded collector records closures while
 * walking the mimalloc heaps, where the object allocator must not run. */
int
_PyGC_ClosureLogAdd(struct _gc_closure_log *log, PyObject **members,
                    Py_ssize_t n, int pure)
{
    if (log->arena_len + n > log->arena_cap) {
        Py_ssize_t ncap = log->arena_cap ? log->arena_cap : 4096;
        while (ncap < log->arena_len + n) {
            ncap *= 2;
        }
        PyObject **na = PyMem_RawRealloc(log->arena, ncap * sizeof(PyObject *));
        if (na == NULL) {
            log->oom = 1;
            return -1;
        }
        log->arena = na;
        log->arena_cap = ncap;
    }
    if (log->recs_len == log->recs_cap) {
        Py_ssize_t ncap = log->recs_cap ? log->recs_cap * 2 : 256;
        struct _gc_closure_record *nr = PyMem_RawRealloc(
            log->recs, ncap * sizeof(struct _gc_closure_record));
        if (nr == NULL) {
            log->oom = 1;
            return -1;
        }
        log->recs = nr;
        log->recs_cap = ncap;
    }
    struct _gc_closure_record *rec = &log->recs[log->recs_len++];
    rec->start = log->arena_len;
    rec->size = n;
    rec->pure = pure;
    memcpy(log->arena + log->arena_len, members, n * sizeof(PyObject *));
    log->arena_len += n;
    return 0;
}

void
_PyGC_ClosureLogFree(struct _gc_closure_log *log)
{
    PyMem_RawFree(log->arena);
    PyMem_RawFree(log->recs);
    log->arena = NULL;
    log->recs = NULL;
}

#define CLOSURE_GEN_COUNTERS(X) \
    X(collections) X(closure_collections) X(truth_collections) \
    X(candidates) X(closures) X(pure_closures) X(pure_objects) \
    X(true_garbage) X(missed_garbage) X(unknown_with_garbage) \
    X(phantom_closures) X(pure_but_live) X(edges) X(traversals_old) \
    X(queue_high_water) X(fallbacks) X(log_oom) X(marked)

static void
closure_stats_write_types(FILE *f, const struct _gc_closure_type_count *table)
{
    fputc('{', f);
    const char *sep = "";
    for (int i = 0; i < _PyGC_CLOSURE_TYPES; i++) {
        if (table[i].count == 0) {
            continue;
        }
        fprintf(f, "%s\"%s\": %llu", sep, table[i].name,
                (unsigned long long)table[i].count);
        sep = ", ";
    }
    fputc('}', f);
}

static void
closure_stats_write_hist(FILE *f, const uint64_t *hist)
{
    fputc('[', f);
    for (int i = 0; i < _PyGC_CLOSURE_HIST; i++) {
        fprintf(f, "%s%llu", i ? ", " : "", (unsigned long long)hist[i]);
    }
    fputc(']', f);
}

/* Append one JSON line.  Only integers are printed: %f is locale dependent
 * and benchmark harnesses do call setlocale(). */
static void
closure_stats_dump(struct _gc_runtime_state *gcstate)
{
    FILE *f = fopen(gcstate->closure_stats_path, "a");
    if (f == NULL) {
        return;
    }
    fprintf(f, "{\"mode\": %d, \"gens\": [", gcstate->closure_mode);
    for (int g = 0; g < NUM_GENERATIONS; g++) {
        struct _gc_closure_gen_stats *st = &gcstate->closure_stats->gens[g];
        fprintf(f, "%s{", g ? ", " : "");
#define WRITE_COUNTER(name) \
        fprintf(f, "\"" #name "\": %llu, ", (unsigned long long)st->name);
        CLOSURE_GEN_COUNTERS(WRITE_COUNTER)
#undef WRITE_COUNTER
        fprintf(f, "\"closure_time_ns\": %lld, \"deduce_time_ns\": %lld, "
                "\"mark_time_ns\": %lld, ",
                (long long)st->closure_time, (long long)st->deduce_time,
                (long long)st->mark_time);
        fprintf(f, "\"size_hist\": ");
        closure_stats_write_hist(f, st->size_hist);
        fprintf(f, ", \"pure_size_hist\": ");
        closure_stats_write_hist(f, st->pure_size_hist);
        fprintf(f, ", \"blockers\": ");
        closure_stats_write_types(f, st->blockers);
        fprintf(f, ", \"missed_types\": ");
        closure_stats_write_types(f, st->missed_types);
        fputc('}', f);
    }
    fprintf(f, "]}\n");
    fclose(f);
}

void
_PyGC_ClosureFini(struct _gc_runtime_state *gcstate)
{
    if (gcstate->closure_stats != NULL && gcstate->closure_stats_path != NULL) {
        closure_stats_dump(gcstate);
    }
    PyMem_RawFree(gcstate->closure_stats);
    gcstate->closure_stats = NULL;
    PyMem_RawFree((void *)gcstate->closure_stats_path);
    gcstate->closure_stats_path = NULL;
}

static PyObject *
closure_types_dict(const struct _gc_closure_type_count *table)
{
    PyObject *d = PyDict_New();
    if (d == NULL) {
        return NULL;
    }
    for (int i = 0; i < _PyGC_CLOSURE_TYPES; i++) {
        if (table[i].count == 0) {
            continue;
        }
        PyObject *v = PyLong_FromUnsignedLongLong(table[i].count);
        if (v == NULL || PyDict_SetItemString(d, table[i].name, v) < 0) {
            Py_XDECREF(v);
            Py_DECREF(d);
            return NULL;
        }
        Py_DECREF(v);
    }
    return d;
}

static PyObject *
closure_hist_list(const uint64_t *hist)
{
    PyObject *l = PyList_New(_PyGC_CLOSURE_HIST);
    if (l == NULL) {
        return NULL;
    }
    for (int i = 0; i < _PyGC_CLOSURE_HIST; i++) {
        PyObject *v = PyLong_FromUnsignedLongLong(hist[i]);
        if (v == NULL) {
            Py_DECREF(l);
            return NULL;
        }
        PyList_SET_ITEM(l, i, v);
    }
    return l;
}

static int
closure_dict_set(PyObject *d, const char *key, PyObject *value)
{
    if (value == NULL) {
        return -1;
    }
    int rc = PyDict_SetItemString(d, key, value);
    Py_DECREF(value);
    return rc;
}

/* gc._closure_stats(): list with one dict per generation, or None. */
PyObject *
_PyGC_ClosureStats(PyInterpreterState *interp)
{
    struct _gc_runtime_state *gcstate = &interp->gc;
    if (gcstate->closure_stats == NULL) {
        Py_RETURN_NONE;
    }
    PyObject *result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }
    for (int g = 0; g < NUM_GENERATIONS; g++) {
        struct _gc_closure_gen_stats *st = &gcstate->closure_stats->gens[g];
        PyObject *d = PyDict_New();
        if (d == NULL) {
            goto error;
        }
#define SET_COUNTER(name) \
        if (closure_dict_set(d, #name, \
                PyLong_FromUnsignedLongLong(st->name)) < 0) { \
            Py_DECREF(d); \
            goto error; \
        }
        CLOSURE_GEN_COUNTERS(SET_COUNTER)
#undef SET_COUNTER
        if (closure_dict_set(d, "closure_time_ns",
                             PyLong_FromLongLong(st->closure_time)) < 0
            || closure_dict_set(d, "deduce_time_ns",
                                PyLong_FromLongLong(st->deduce_time)) < 0
            || closure_dict_set(d, "mark_time_ns",
                                PyLong_FromLongLong(st->mark_time)) < 0
            || closure_dict_set(d, "size_hist",
                                closure_hist_list(st->size_hist)) < 0
            || closure_dict_set(d, "pure_size_hist",
                                closure_hist_list(st->pure_size_hist)) < 0
            || closure_dict_set(d, "blockers",
                                closure_types_dict(st->blockers)) < 0
            || closure_dict_set(d, "missed_types",
                                closure_types_dict(st->missed_types)) < 0
            || PyList_Append(result, d) < 0) {
            Py_DECREF(d);
            goto error;
        }
        Py_DECREF(d);
    }
    return result;
error:
    Py_DECREF(result);
    return NULL;
}

void
_PyGC_ClosureResetStats(PyInterpreterState *interp)
{
    struct _gc_runtime_state *gcstate = &interp->gc;
    if (gcstate->closure_stats != NULL) {
        memset(gcstate->closure_stats, 0, sizeof(struct _gc_closure_stats));
    }
}
