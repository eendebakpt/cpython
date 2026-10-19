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
 * sweep (_PyGC_DagSweep, shared with the free-threaded collector) then
 * judges each record: it is garbage iff its external references (R - E)
 * are exactly the edges from records already judged garbage.  By induction
 * every reference into a garbage record comes from garbage, so the verdict
 * is sound; and a garbage structure whose sink was visited before its
 * source is found whole in one collection (the plain pass only finds the
 * source and leaves the sink for the next collection).  Unlike union-find
 * merging, a record never absorbs live candidates it points at.  Still one
 * traversal per object; the record index write per object and a final
 * walk that restores the list links (as move_unreachable() does) are the
 * price, plus O(records + inter-record edges) bookkeeping.
 */

typedef struct {
    struct _gc_dag dag;
    PyObject **queue;
    Py_ssize_t len, qcap;
    int oom;
} dag_state;

static int
dag_discover(dag_state *ds, PyObject *op)
{
    if (ds->len == ds->qcap) {
        Py_ssize_t ncap = ds->qcap ? ds->qcap * 2 : 1024;
        PyObject **nq = PyMem_Realloc(ds->queue, ncap * sizeof(PyObject *));
        if (nq == NULL) {
            ds->oom = 1;
            return -1;
        }
        ds->queue = nq;
        ds->qcap = ncap;
    }
    Py_ssize_t cur = ds->dag.cur;
    gc_set_refs(AS_GC(op), cur);
    _PyObject_ASSERT(op, Py_REFCNT(op) > 0);
    ds->dag.rec[cur].rc_sum += Py_REFCNT(op);
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
    Py_ssize_t cur = ds->dag.cur;
    if (idx == 0) {
        ds->dag.rec[cur].edges++;
        return dag_discover(ds, op);
    }
    if (idx == cur) {
        ds->dag.rec[cur].edges++;
        return 0;
    }
    if (_PyGC_DagEdge(&ds->dag, idx) < 0) {
        ds->oom = 1;
        return -1;
    }
    return 0;
}

/* Truth mode: record the records (members grouped by record) before the
 * final walk destroys the record indices. */
static void
dag_log_records(dag_state *ds, PyGC_Head *young, struct _gc_closure_log *log)
{
    struct _gc_dag_record *rec = ds->dag.rec;
    Py_ssize_t count = ds->dag.count;
    Py_ssize_t n = 0;
    for (PyGC_Head *gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        rec[gc_get_refs(gc)].size++;
        n++;
    }
    Py_ssize_t *offset = PyMem_Malloc(count * sizeof(Py_ssize_t));
    PyObject **arena = PyMem_Malloc((n ? n : 1) * sizeof(PyObject *));
    if (offset == NULL || arena == NULL) {
        PyMem_Free(offset);
        PyMem_Free(arena);
        log->oom = 1;
        return;
    }
    Py_ssize_t pos = 0;
    for (Py_ssize_t i = 1; i < count; i++) {
        offset[i] = pos;
        pos += rec[i].size;
    }
    for (PyGC_Head *gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        arena[offset[gc_get_refs(gc)]++] = FROM_GC(gc);
    }
    for (Py_ssize_t i = 1; i < count; i++) {
        if (rec[i].size == 0) {
            continue;
        }
        if (_PyGC_ClosureLogAdd(log, arena + offset[i] - rec[i].size,
                                rec[i].size, rec[i].garbage) < 0) {
            break;
        }
    }
    PyMem_Free(offset);
    PyMem_Free(arena);
    for (Py_ssize_t i = 1; i < count; i++) {
        rec[i].size = 0;    /* recounted by the final walk */
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
    if (_PyGC_DagNewRecord(&ds.dag) < 0) {      /* record 0: unused */
        goto fail;
    }
    for (PyGC_Head *gc = GC_NEXT(young); gc != young; gc = GC_NEXT(gc)) {
        if (gc_get_refs(gc) != 0) {
            continue;   /* already part of a record */
        }
        if (_PyGC_DagNewRecord(&ds.dag) < 0) {
            goto fail;
        }
        ds.len = 0;
        if (dag_discover(&ds, FROM_GC(gc)) < 0) {
            goto fail;
        }
        for (Py_ssize_t head = 0; head < ds.len && !ds.oom; head++) {
            PyObject *op = ds.queue[head];
            (void) Py_TYPE(op)->tp_traverse(op, visit_closure_dag, &ds);
        }
        if (ds.oom || _PyGC_DagFlush(&ds.dag) < 0) {
            goto fail;
        }
    }
    _PyGC_DagSweep(&ds.dag);

    if (log != NULL) {
        dag_log_records(&ds, young, log);
    }

    /* Final walk, modelled on move_unreachable(): restore the prev links
     * of the survivors, move the garbage to unreachable. */
    Py_ssize_t nreachable = 0;
    PyGC_Head *prev = young;
    PyGC_Head *gc = GC_NEXT(young);
    while (gc != young) {
        struct _gc_dag_record *r = &ds.dag.rec[gc_get_refs(gc)];
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
        for (Py_ssize_t i = 1; i < ds.dag.count; i++) {
            struct _gc_dag_record *r = &ds.dag.rec[i];
            if (r->size > 0) {
                _PyGC_ClosureAccount(st, r->size, r->rc_sum, r->edges, r->garbage);
            }
        }
    }
    _PyGC_DagFree(&ds.dag);
    PyMem_Free(ds.queue);
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
    _PyGC_DagFree(&ds.dag);
    PyMem_Free(ds.queue);
    return -1;
}

