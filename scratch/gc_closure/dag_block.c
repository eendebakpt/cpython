/*
 * Deferred-verdict variant (PYTHON_GC_DAG=1).
 *
 * Forward closures are kept exactly as in closure_pass(), but a closure is
 * not judged on its own.  Every candidate carries the index of its closure
 * record in the gc_refs bits of _gc_prev (0 = not discovered yet).  Edges
 * from the closure being built into members of earlier records are not
 * counted in E (as before: those members are retired) but recorded per
 * target record.  Because a closure absorbs everything it reaches that is
 * not retired yet, such edges always point from a later record to an
 * earlier one, so the records form a DAG ordered by creation.  A backward
 * sweep then judges each record: it is garbage iff its external references
 * (R - E) are exactly the edges from records already judged garbage.  By
 * induction every reference into a garbage record comes from garbage, so
 * the verdict is sound; and a garbage structure whose sink was visited
 * before its source is found whole in one collection (the plain pass only
 * finds the source and leaves the sink for the next collection).  Unlike
 * union-find merging, a record never absorbs live candidates it points at.
 * Still one traversal per object; the record index write per object and a
 * final walk that restores the list links (as move_unreachable() does) are
 * the price, plus O(records + inter-record edges) bookkeeping.
 */

typedef struct {
    Py_ssize_t rc_sum;      /* R */
    Py_ssize_t edges;       /* E (edges inside the record) */
    Py_ssize_t garbage_in;  /* edges into this record from garbage records */
    Py_ssize_t out_start;   /* this record's out-edges in the edge arena */
    Py_ssize_t out_count;
    Py_ssize_t stamp;       /* == current record while tmp is valid */
    Py_ssize_t tmp;         /* edges from the current record into this one */
    Py_ssize_t size;        /* members (statistics) */
    int garbage;
} dag_record;

typedef struct {
    dag_record *rec;
    Py_ssize_t count, cap;  /* records 1..count-1; 0 means undiscovered */
    Py_ssize_t cur;
    Py_ssize_t *touched;    /* records with edges from cur, for the arena */
    Py_ssize_t ntouched, touched_cap;
    Py_ssize_t *etarget;    /* edge arena: (target record, edge count) */
    Py_ssize_t *ecount;
    Py_ssize_t elen, ecap;
    PyObject **queue;
    Py_ssize_t len, qcap;
    int oom;
} dag_state;

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
    void *np = PyMem_Realloc(*p, ncap * elem);
    if (np == NULL) {
        return -1;
    }
    *p = np;
    *cap = ncap;
    return 0;
}

static int
dag_new_record(dag_state *ds)
{
    if (dag_grow((void **)&ds->rec, &ds->cap, ds->count + 1, sizeof(dag_record)) < 0) {
        ds->oom = 1;
        return -1;
    }
    Py_ssize_t i = ds->count++;
    memset(&ds->rec[i], 0, sizeof(dag_record));
    ds->rec[i].stamp = -1;
    return (int)i;
}

static int
dag_discover(dag_state *ds, PyObject *op)
{
    if (dag_grow((void **)&ds->queue, &ds->qcap, ds->len + 1, sizeof(PyObject *)) < 0) {
        ds->oom = 1;
        return -1;
    }
    gc_set_refs(AS_GC(op), ds->cur);
    _PyObject_ASSERT(op, Py_REFCNT(op) > 0);
    ds->rec[ds->cur].rc_sum += Py_REFCNT(op);
    ds->queue[ds->len++] = op;
    return 0;
}

/* A traversal callback for closure_pass_dag. */
static int
visit_closure_dag(PyObject *op, void *arg)
{
    dag_state *ds = arg;
    OBJECT_STAT_INC(object_visits);
    if (!_PyObject_IS_GC(op)) {
        return 0;
    }
    PyGC_Head *gc = AS_GC(op);
    if (!gc_is_collecting(gc)) {
        return 0;   /* outside S: neither counted nor followed */
    }
    Py_ssize_t idx = gc_get_refs(gc);
    if (idx == 0) {
        ds->rec[ds->cur].edges++;
        return dag_discover(ds, op);
    }
    if (idx == ds->cur) {
        ds->rec[ds->cur].edges++;
        return 0;
    }
    /* an edge into an earlier record: not internal, remembered for the
     * backward sweep */
    dag_record *r = &ds->rec[idx];
    if (r->stamp != ds->cur) {
        if (dag_grow((void **)&ds->touched, &ds->touched_cap, ds->ntouched + 1,
                     sizeof(Py_ssize_t)) < 0) {
            ds->oom = 1;
            return -1;
        }
        r->stamp = ds->cur;
        r->tmp = 0;
        ds->touched[ds->ntouched++] = idx;
    }
    r->tmp++;
    return 0;
}

/* Move the current record's out-edges from the sparse accumulators into
 * the arena. */
static int
dag_flush_edges(dag_state *ds)
{
    dag_record *cur = &ds->rec[ds->cur];
    cur->out_start = ds->elen;
    cur->out_count = ds->ntouched;
    if (dag_grow((void **)&ds->etarget, &ds->ecap, ds->elen + ds->ntouched,
                 sizeof(Py_ssize_t)) < 0) {
        ds->oom = 1;
        return -1;
    }
    Py_ssize_t ecap2 = ds->ecap;    /* keep both arrays the same size */
    if (dag_grow((void **)&ds->ecount, &ecap2, ds->ecap, sizeof(Py_ssize_t)) < 0) {
        ds->oom = 1;
        return -1;
    }
    for (Py_ssize_t i = 0; i < ds->ntouched; i++) {
        Py_ssize_t k = ds->touched[i];
        ds->etarget[ds->elen] = k;
        ds->ecount[ds->elen] = ds->rec[k].tmp;
        ds->elen++;
    }
    ds->ntouched = 0;
    return 0;
}

static void
dag_free(dag_state *ds)
{
    PyMem_Free(ds->rec);
    PyMem_Free(ds->touched);
    PyMem_Free(ds->etarget);
    PyMem_Free(ds->ecount);
    PyMem_Free(ds->queue);
}

/* Truth mode: record the records (members grouped by record) before the
 * final walk destroys the record indices. */
static void
dag_log_records(dag_state *ds, PyGC_Head *young, struct _gc_closure_log *log)
{
    Py_ssize_t n = 0;
    for (PyGC_Head *gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        ds->rec[gc_get_refs(gc)].size++;
        n++;
    }
    Py_ssize_t *offset = PyMem_Malloc(ds->count * sizeof(Py_ssize_t));
    PyObject **arena = PyMem_Malloc((n ? n : 1) * sizeof(PyObject *));
    if (offset == NULL || arena == NULL) {
        PyMem_Free(offset);
        PyMem_Free(arena);
        log->oom = 1;
        return;
    }
    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 1; i < ds->count; i++) {
        offset[i] = pos;
        pos += ds->rec[i].size;
    }
    for (PyGC_Head *gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        arena[offset[gc_get_refs(gc)]++] = FROM_GC(gc);
    }
    for (Py_ssize_t i = 1; i < ds->count; i++) {
        if (ds->rec[i].size == 0) {
            continue;
        }
        if (_PyGC_ClosureLogAdd(log, arena + offset[i] - ds->rec[i].size,
                                ds->rec[i].size, ds->rec[i].garbage) < 0) {
            break;
        }
    }
    PyMem_Free(offset);
    PyMem_Free(arena);
    for (Py_ssize_t i = 1; i < ds->count; i++) {
        ds->rec[i].size = 0;    /* recounted by the final walk */
    }
}

/* Same contract as closure_pass(). */
static Py_ssize_t
closure_pass_dag(PyGC_Head *young, PyGC_Head *unreachable, Py_ssize_t *reachable,
                 struct _gc_closure_gen_stats *st, struct _gc_closure_log *log)
{
    validate_list(young, collecting_clear_unreachable_clear);
    gc_list_init(unreachable);

    /* Like update_refs() with gc_refs = 0: record index "undiscovered". */
    Py_ssize_t candidates = 0;
    for (PyGC_Head *gc = GC_NEXT(young), *next; gc != young; gc = next) {
        next = GC_NEXT(gc);
        PyObject *op = FROM_GC(gc);
        if (_Py_IsImmortal(op)) {
            assert(!_Py_IsStaticImmortal(op));
            _PyObject_GC_UNTRACK(op);
            continue;
        }
        gc_reset_refs(gc, 0);
        candidates++;
    }

    dag_state ds = { 0 };
    if (dag_new_record(&ds) < 0) {      /* record 0: unused */
        goto fail;
    }
    for (PyGC_Head *gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        if (gc_get_refs(gc) != 0) {
            continue;   /* already part of a record */
        }
        int r = dag_new_record(&ds);
        if (r < 0) {
            goto fail;
        }
        ds.cur = r;
        ds.len = 0;
        if (dag_discover(&ds, FROM_GC(gc)) < 0) {
            goto fail;
        }
        for (Py_ssize_t head = 0; head < ds.len && !ds.oom; head++) {
            PyObject *op = ds.queue[head];
            (void) Py_TYPE(op)->tp_traverse(op, visit_closure_dag, &ds);
        }
        if (ds.oom || dag_flush_edges(&ds) < 0) {
            goto fail;
        }
    }

    /* Backward sweep: records are created in visit order and their
     * out-edges only point at earlier records. */
    for (Py_ssize_t k = ds.count - 1; k >= 1; k--) {
        dag_record *r = &ds.rec[k];
        /* E > R means a tp_traverse visited a reference it does not own;
         * such a record is never garbage. */
        r->garbage = (r->rc_sum >= r->edges
                      && r->rc_sum - r->edges == r->garbage_in);
        if (r->garbage) {
            for (Py_ssize_t e = r->out_start; e < r->out_start + r->out_count; e++) {
                ds.rec[ds.etarget[e]].garbage_in += ds.ecount[e];
            }
        }
    }

    if (log != NULL) {
        dag_log_records(&ds, young, log);
    }

    /* Final walk, modelled on move_unreachable(): restore the prev links
     * of the survivors, move the garbage to unreachable. */
    Py_ssize_t nreachable = 0;
    PyGC_Head *prev = young;
    PyGC_Head *gc = GC_NEXT(young);
    while (gc != young) {
        dag_record *r = &ds.rec[gc_get_refs(gc)];
        r->size++;
        PyObject *op = FROM_GC(gc);
        _PyObject_ASSERT_WITH_MSG(op, r->rc_sum >= r->edges,
                                  "closure edges exceed refcounts");
        if (r->garbage) {
            prev->_gc_next = gc->_gc_next;
            PyGC_Head *last = GC_PREV(unreachable);
            last->_gc_next = (NEXT_MASK_UNREACHABLE | (uintptr_t)gc);
            _PyGCHead_SET_PREV(gc, last);
            gc->_gc_next = (NEXT_MASK_UNREACHABLE | (uintptr_t)unreachable);
            unreachable->_gc_prev = (uintptr_t)gc;
        }
        else {
            _PyGCHead_SET_PREV(gc, prev);
            gc_clear_collecting(gc);
            if (PyTuple_CheckExact(op)) {
                /* untracking overwrites the next object's _gc_prev */
                PyGC_Head *next = GC_NEXT(gc);
                uintptr_t next_prev = next->_gc_prev;
                _PyTuple_MaybeUntrack(op);
                next->_gc_prev = next_prev;
                if (gc->_gc_next != 0) {
                    prev = gc;
                    nreachable++;
                }
            }
            else {
                prev = gc;
                nreachable++;
            }
        }
        gc = (PyGC_Head *)prev->_gc_next;
    }
    young->_gc_prev = (uintptr_t)prev;
    unreachable->_gc_next &= ~NEXT_MASK_UNREACHABLE;

    if (st != NULL) {
        for (Py_ssize_t i = 1; i < ds.count; i++) {
            if (ds.rec[i].size > 0) {
                _PyGC_ClosureAccount(st, ds.rec[i].size, ds.rec[i].rc_sum,
                                     ds.rec[i].edges, ds.rec[i].garbage);
            }
        }
    }
    dag_free(&ds);
    *reachable = nreachable;
    validate_list(young, collecting_clear_unreachable_clear);
    validate_list(unreachable, collecting_set_unreachable_set);
    return candidates;

fail:
    /* Nothing has been moved yet: restore the links by walking the still
     * intact _gc_next chain. */
    prev = young;
    for (gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        _PyGCHead_SET_PREV(gc, prev);
        gc_clear_collecting(gc);
        prev = gc;
    }
    young->_gc_prev = (uintptr_t)prev;
    dag_free(&ds);
    return -1;
}

