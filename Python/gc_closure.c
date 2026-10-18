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

struct _gc_closure_config _PyGC_closure;

void
_PyGC_ClosureInit(void)
{
    _PyGC_closure.mode = _PyGC_CLOSURE_OFF;
    _PyGC_closure.counter = 0;
    _PyGC_closure.stats = NULL;
    _PyGC_closure.stats_path = NULL;

    const char *mode = getenv("PYTHON_GC_CLOSURE");
    if (mode != NULL && *mode != '\0') {
        int value = atoi(mode);
        if (value >= _PyGC_CLOSURE_OFF && value <= _PyGC_CLOSURE_ADAPTIVE) {
            _PyGC_closure.mode = value;
        }
    }
    const char *mark = getenv("PYTHON_GC_MARK");
    _PyGC_closure.mark_alive = (mark != NULL && atoi(mark) != 0);
    const char *dag = getenv("PYTHON_GC_DAG");
    _PyGC_closure.dag = (dag != NULL) ? atoi(dag) : 0;
    if (_PyGC_closure.dag < 0 || _PyGC_closure.dag > 2) {
        _PyGC_closure.dag = 0;
    }
    memset(_PyGC_closure.adapt, 0, sizeof(_PyGC_closure.adapt));
    const char *every = getenv("PYTHON_GC_CLASSIC_EVERY");
    _PyGC_closure.classic_every = (every != NULL) ? atoi(every) : 0;
    _PyGC_closure.full_counter = 0;
    const char *path = getenv("PYTHON_GC_CLOSURE_STATS");
    int want_path = (path != NULL && *path != '\0');
    if (want_path || _PyGC_closure.mode == _PyGC_CLOSURE_TRUTH) {
        _PyGC_closure.stats = PyMem_RawCalloc(1, sizeof(struct _gc_closure_stats));
        if (_PyGC_closure.stats == NULL) {
            _PyGC_closure.mode = _PyGC_CLOSURE_OFF;
            return;
        }
        if (want_path) {
            _PyGC_closure.stats_path = _PyMem_RawStrdup(path);
        }
    }
}

int
_PyGC_ClosureWanted(int generation)
{
    if (generation == NUM_GENERATIONS - 1 && _PyGC_closure.classic_every > 0
        && (++_PyGC_closure.full_counter % _PyGC_closure.classic_every) == 0) {
        return 0;   /* periodic classic full collection */
    }
    switch (_PyGC_closure.mode) {
    case _PyGC_CLOSURE_ADAPTIVE: {
        /* Use the detector that was cheaper per candidate the last time it
         * ran on this generation; every 16th collection run the other one
         * so that both samples stay current (and the classic algorithm
         * bounds the delay of garbage the closure pass cannot decide). */
        assert(generation >= 0 && generation < 3);
        struct _gc_closure_adapt *a = &_PyGC_closure.adapt[generation];
        a->n++;
        int choice;
        if (a->closure_cost == 0.0) {
            choice = 1;
        }
        else if (a->classic_cost == 0.0) {
            choice = 0;
        }
        else if ((a->n & 15) == 0) {
            choice = !a->last_closure;
        }
        else {
            choice = a->closure_cost < a->classic_cost;
        }
        a->last_closure = choice;
        return choice;
    }
    case _PyGC_CLOSURE_ALL:
        return 1;
    case _PyGC_CLOSURE_YOUNG:
        return generation < NUM_GENERATIONS - 1;
    case _PyGC_CLOSURE_OLD:
        return generation == NUM_GENERATIONS - 1;
    case _PyGC_CLOSURE_ALTERNATE:
        return (_PyGC_closure.counter++ & 1) != 0;
    default:
        return 0;
    }
}

void
_PyGC_ClosureRecordCost(int generation, int used_closure, PyTime_t ns,
                        Py_ssize_t candidates)
{
    if (_PyGC_closure.mode != _PyGC_CLOSURE_ADAPTIVE || candidates <= 0) {
        return;
    }
    double cost = (double)ns / (double)candidates;
    if (used_closure) {
        _PyGC_closure.adapt[generation].closure_cost = cost;
    }
    else {
        _PyGC_closure.adapt[generation].classic_cost = cost;
    }
}

struct _gc_closure_gen_stats *
_PyGC_ClosureGenStats(int generation)
{
    if (_PyGC_closure.stats == NULL) {
        return NULL;
    }
    assert(generation >= 0 && generation < NUM_GENERATIONS);
    struct _gc_closure_gen_stats *st = &_PyGC_closure.stats->gens[generation];
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
closure_stats_dump(void)
{
    FILE *f = fopen(_PyGC_closure.stats_path, "a");
    if (f == NULL) {
        return;
    }
    fprintf(f, "{\"mode\": %d, \"gens\": [", _PyGC_closure.mode);
    for (int g = 0; g < NUM_GENERATIONS; g++) {
        struct _gc_closure_gen_stats *st = &_PyGC_closure.stats->gens[g];
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
_PyGC_ClosureFini(void)
{
    if (_PyGC_closure.stats != NULL && _PyGC_closure.stats_path != NULL) {
        closure_stats_dump();
    }
    PyMem_RawFree(_PyGC_closure.stats);
    _PyGC_closure.stats = NULL;
    PyMem_RawFree((void *)_PyGC_closure.stats_path);
    _PyGC_closure.stats_path = NULL;
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
_PyGC_ClosureStats(void)
{
    if (_PyGC_closure.stats == NULL) {
        Py_RETURN_NONE;
    }
    PyObject *result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }
    for (int g = 0; g < NUM_GENERATIONS; g++) {
        struct _gc_closure_gen_stats *st = &_PyGC_closure.stats->gens[g];
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
_PyGC_ClosureResetStats(void)
{
    if (_PyGC_closure.stats != NULL) {
        memset(_PyGC_closure.stats, 0, sizeof(struct _gc_closure_stats));
    }
}

/* ---- record DAG of the deferred-verdict pass ---------------------------- */

static int
dag_grow(void **p, Py_ssize_t *cap, Py_ssize_t needed, size_t elem)
{
    if (needed <= *cap) {
        return 0;
    }
    Py_ssize_t ncap = *cap ? *cap : 1024;
    while (ncap < needed) {
        ncap *= 2;
    }
    /* raw allocator: the free-threaded collector calls this while walking
     * the mimalloc heaps */
    void *np = PyMem_RawRealloc(*p, ncap * elem);
    if (np == NULL) {
        return -1;
    }
    *p = np;
    *cap = ncap;
    return 0;
}

int
_PyGC_DagNewRecord(struct _gc_dag *dag)
{
    if (dag_grow((void **)&dag->rec, &dag->cap, dag->count + 1,
                 sizeof(struct _gc_dag_record)) < 0) {
        return -1;
    }
    Py_ssize_t i = dag->count++;
    memset(&dag->rec[i], 0, sizeof(struct _gc_dag_record));
    dag->rec[i].stamp = -1;
    dag->cur = i;
    return (int)i;
}

int
_PyGC_DagEdge(struct _gc_dag *dag, Py_ssize_t target)
{
    struct _gc_dag_record *r = &dag->rec[target];
    if (r->stamp != dag->cur) {
        if (dag_grow((void **)&dag->touched, &dag->touched_cap,
                     dag->ntouched + 1, sizeof(Py_ssize_t)) < 0) {
            return -1;
        }
        r->stamp = dag->cur;
        r->tmp = 0;
        dag->touched[dag->ntouched++] = target;
    }
    r->tmp++;
    return 0;
}

int
_PyGC_DagFlush(struct _gc_dag *dag)
{
    struct _gc_dag_record *cur = &dag->rec[dag->cur];
    cur->out_start = dag->elen;
    cur->out_count = dag->ntouched;
    Py_ssize_t needed = dag->elen + dag->ntouched;
    if (dag_grow((void **)&dag->etarget, &dag->ecap, needed, sizeof(Py_ssize_t)) < 0
        || dag_grow((void **)&dag->ecount, &dag->ecount_cap, needed,
                    sizeof(Py_ssize_t)) < 0) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < dag->ntouched; i++) {
        Py_ssize_t k = dag->touched[i];
        dag->etarget[dag->elen] = k;
        dag->ecount[dag->elen] = dag->rec[k].tmp;
        dag->elen++;
    }
    dag->ntouched = 0;
    return 0;
}

/* Backward sweep: records are created in visit order and their out-edges
 * only point at earlier records.  A record with E > R (a tp_traverse that
 * visits a reference it does not own) is never garbage. */
void
_PyGC_DagSweep(struct _gc_dag *dag)
{
    for (Py_ssize_t k = dag->count - 1; k >= 1; k--) {
        struct _gc_dag_record *r = &dag->rec[k];
        r->garbage = (!r->tainted && r->rc_sum >= r->edges
                      && r->rc_sum - r->edges == r->garbage_in);
        if (r->garbage) {
            for (Py_ssize_t e = r->out_start; e < r->out_start + r->out_count; e++) {
                dag->rec[dag->etarget[e]].garbage_in += dag->ecount[e];
            }
        }
    }
}

void
_PyGC_DagFree(struct _gc_dag *dag)
{
    PyMem_RawFree(dag->rec);
    PyMem_RawFree(dag->touched);
    PyMem_RawFree(dag->etarget);
    PyMem_RawFree(dag->ecount);
    memset(dag, 0, sizeof(*dag));
}
