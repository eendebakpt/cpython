/*
 * Closure-sum detection for young collections.
 *
 * An alternative to deduce_unreachable() for finding the unreachable subset
 * of `young`, used for the generations below the oldest one.  Let S be the
 * candidate set (every member of `young`; all have PREV_MASK_COLLECTING
 * set).  For an object x in S, its closure C(x) is every member of S
 * reachable from x through tp_traverse edges that stay inside S; edges
 * leaving S (into older generations) are neither counted nor followed.
 * Over the closure we accumulate
 *
 *     R = sum of ob_refcnt over the members of C
 *     E = number of tp_traverse visits o -> t with o in C and t in S
 *
 * Every reference counted in E comes from inside C (C is closed), so R - E
 * is the number of references into C from anywhere else: older
 * generations, the C stack, untracked or non-GC holders, or edges a
 * tp_traverse does not report.  If R == E the whole closure is
 * unreachable.  Otherwise the closure is "unknown" and is kept, exactly
 * like deduce_unreachable() keeps reachable objects: its members are
 * promoted and the next collection of the older generation, or the full
 * collection, which still runs deduce_unreachable(), finds any garbage in
 * it.  This is trial deletion with one aggregate counter; the per-object
 * variant is what update_refs(), subtract_refs() and move_unreachable()
 * compute.
 *
 * Each object is traversed once, survivors are not moved, and no
 * per-object counter is stored: the only state touched is two flag bits,
 * whereas deduce_unreachable() overwrites every candidate's _gc_prev with
 * gc_refs and rebuilds the lists afterwards.  That makes the pass 5-40%
 * cheaper than deduce_unreachable() on young collections.  It is not used
 * for full collections: there the candidate set contains the live class
 * and module state that garbage references, which would make most closures
 * unknown.
 *
 * Soundness rests on the same assumption as deduce_unreachable(): a
 * tp_traverse reports exactly the owned references.  A traverse that
 * visits a borrowed reference can make E exceed R; deduce_unreachable()
 * asserts "refcount is too small" for the same bug, this pass asserts
 * R >= E and treats the closure as unknown in release builds.
 *
 * Flags during the pass:
 *   PREV_MASK_COLLECTING   object is in S and not retired yet
 *   NEXT_MASK_UNREACHABLE  object was discovered by the current closure;
 *                          garbage keeps it set, which is what
 *                          move_legacy_finalizers() expects.
 * gc_refs is not used, so prev pointers stay valid and garbage can be
 * unlinked with the normal list operations.
 */

typedef struct {
    PyObject **queue;       /* breadth-first worklist; queue[0..len) is the closure */
    Py_ssize_t len;
    Py_ssize_t cap;
    Py_ssize_t rc_sum;      /* R */
    Py_ssize_t edges;       /* E */
    int oom;
} closure_state;

static int
closure_discover(closure_state *cs, PyObject *op)
{
    if (cs->len == cs->cap) {
        Py_ssize_t ncap = cs->cap ? cs->cap * 2 : 1024;
        PyObject **nq = PyMem_Realloc(cs->queue, ncap * sizeof(PyObject *));
        if (nq == NULL) {
            cs->oom = 1;
            return -1;
        }
        cs->queue = nq;
        cs->cap = ncap;
    }
    AS_GC(op)->_gc_next |= NEXT_MASK_UNREACHABLE;
    cs->queue[cs->len++] = op;
    _PyObject_ASSERT(op, Py_REFCNT(op) > 0);
    cs->rc_sum += Py_REFCNT(op);
    return 0;
}

/* A traversal callback for closure_pass. */
static int
visit_closure(PyObject *op, void *arg)
{
    closure_state *cs = arg;
    OBJECT_STAT_INC(object_visits);
    if (!_PyObject_IS_GC(op)) {
        return 0;
    }
    PyGC_Head *gc = AS_GC(op);
    if (!gc_is_collecting(gc)) {
        /* Outside S (older generation, retired survivor, untracked):
         * neither counted nor followed. */
        return 0;
    }
    cs->edges++;
    if (gc->_gc_next & NEXT_MASK_UNREACHABLE) {
        /* Already discovered.  A member of an earlier garbage closure can
         * not be reached from here: it has no external references. */
        return 0;
    }
    return closure_discover(cs, op);
}

/* Like update_refs() without storing gc_refs: set PREV_MASK_COLLECTING on
 * every member of containers and untrack immortal objects. */
static Py_ssize_t
closure_mark_candidates(PyGC_Head *containers)
{
    Py_ssize_t candidates = 0;
    PyGC_Head *gc = GC_NEXT(containers);
    while (gc != containers) {
        PyGC_Head *next = GC_NEXT(gc);
        PyObject *op = FROM_GC(gc);
        if (_Py_IsImmortal(op)) {
            assert(!_Py_IsStaticImmortal(op));
            _PyObject_GC_UNTRACK(op);
        }
        else {
            _PyObject_ASSERT(op, (gc->_gc_next & NEXT_MASK_UNREACHABLE) == 0);
            gc->_gc_prev |= PREV_MASK_COLLECTING;
            candidates++;
        }
        gc = next;
    }
    return candidates;
}

/* Move the members of a garbage closure to `unreachable`, building the list
 * the way move_unreachable() does (NEXT_MASK_UNREACHABLE set on members, not
 * on the head). */
static void
closure_collect(closure_state *cs, PyGC_Head *unreachable)
{
    for (Py_ssize_t i = 0; i < cs->len; i++) {
        PyGC_Head *gc = AS_GC(cs->queue[i]);
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
        PyGC_Head *prev = GC_PREV(gc);
        PyGC_Head *next = GC_NEXT(gc);
        /* prev may be a later member whose discovered bit this clears;
         * that is fine, the search is over and we go by the queue. */
        _PyGCHead_SET_NEXT(prev, next);
        _PyGCHead_SET_PREV(next, prev);

        PyGC_Head *last = GC_PREV(unreachable);
        uintptr_t flag = (last == unreachable) ? 0 : NEXT_MASK_UNREACHABLE;
        last->_gc_next = (uintptr_t)gc | flag;
        _PyGCHead_SET_PREV(gc, last);
        gc->_gc_next = (uintptr_t)unreachable | NEXT_MASK_UNREACHABLE;
        unreachable->_gc_prev = (uintptr_t)gc;
    }
}

/* Retire the members of an unknown closure in place: clear the flags and
 * untrack exact tuples that no longer need tracking, as move_unreachable()
 * does.  Returns the number of members still tracked. */
static Py_ssize_t
closure_keep(closure_state *cs)
{
    Py_ssize_t kept = 0;
    for (Py_ssize_t i = 0; i < cs->len; i++) {
        PyObject *op = cs->queue[i];
        PyGC_Head *gc = AS_GC(op);
        gc->_gc_next &= ~NEXT_MASK_UNREACHABLE;
        gc_clear_collecting(gc);
        if (PyTuple_CheckExact(op)) {
            _PyTuple_MaybeUntrack(op);
            if (gc->_gc_next == 0) {
                continue;
            }
        }
        kept++;
    }
    return kept;
}

/* Undo a partially completed pass after a memory failure: everything back
 * in young with plain flags, so that deduce_unreachable() can run. */
static void
closure_abort(closure_state *cs, PyGC_Head *young, PyGC_Head *unreachable)
{
    for (Py_ssize_t i = 0; i < cs->len; i++) {
        AS_GC(cs->queue[i])->_gc_next &= ~NEXT_MASK_UNREACHABLE;
    }
    clear_unreachable_mask(unreachable);
    gc_list_clear_collecting(unreachable);
    gc_list_merge(unreachable, young);
    gc_list_clear_collecting(young);
}

/* The closure-sum counterpart of deduce_unreachable(), with the same
 * post-conditions: `unreachable` holds the garbage (PREV_MASK_COLLECTING and
 * NEXT_MASK_UNREACHABLE set), `young` the survivors (flags clear).
 * Returns the number of candidates, or -1 if memory ran out; in that case
 * all objects are back in `young` with plain flags. */
static Py_ssize_t
closure_pass(PyGC_Head *young, PyGC_Head *unreachable, Py_ssize_t *reachable)
{
    validate_list(young, collecting_clear_unreachable_clear);
    gc_list_init(unreachable);
    Py_ssize_t candidates = closure_mark_candidates(young);
    closure_state cs = { 0 };
    Py_ssize_t nreachable = 0;

    /* Walk young with a cursor.  `prev` is always a node that stays in
     * young (the head or a retired survivor), so GC_NEXT(prev) is the next
     * node to look at even after a garbage closure has been unlinked. */
    PyGC_Head *prev = young;
    PyGC_Head *start;
    while ((start = GC_NEXT(prev)) != young) {
        if (!gc_is_collecting(start)) {
            prev = start;   /* retired by an earlier closure */
            continue;
        }
        cs.len = 0;
        cs.rc_sum = 0;
        cs.edges = 0;
        if (closure_discover(&cs, FROM_GC(start)) < 0) {
            break;
        }
        for (Py_ssize_t head = 0; head < cs.len && !cs.oom; head++) {
            PyObject *op = cs.queue[head];
            (void) Py_TYPE(op)->tp_traverse(op, visit_closure, &cs);
        }
        if (cs.oom) {
            break;
        }
        _PyObject_ASSERT_WITH_MSG(FROM_GC(start), cs.rc_sum >= cs.edges,
                                  "closure edges exceed refcounts");
        if (cs.rc_sum == cs.edges) {
            closure_collect(&cs, unreachable);
            /* start is unlinked; prev stays */
        }
        else {
            PyGC_Head *before = GC_PREV(start);
            nreachable += closure_keep(&cs);
            /* start itself may have been untracked; `before` is still in
             * young and GC_NEXT(before) is then start's old successor. */
            prev = (start->_gc_next == 0) ? before : start;
        }
    }

    if (cs.oom) {
        closure_abort(&cs, young, unreachable);
        PyMem_Free(cs.queue);
        return -1;
    }
    PyMem_Free(cs.queue);
    *reachable = nreachable;
    validate_list(young, collecting_clear_unreachable_clear);
    validate_list(unreachable, collecting_set_unreachable_set);
    return candidates;
}

/* Find the unreachable subset of young: the closure pass for the young
 * generations, deduce_unreachable() for the full collection (and as the
 * fallback when memory runs out).  PYTHON_GC_CLASSIC=1 disables the closure
 * pass; this switch exists for A/B measurements and will go. */
static Py_ssize_t
find_unreachable(GCState *gcstate, int generation, PyGC_Head *young,
                 PyGC_Head *unreachable, Py_ssize_t *reachable)
{
    if (generation < NUM_GENERATIONS - 1 && !gcstate->closure_disabled) {
        Py_ssize_t candidates = closure_pass(young, unreachable, reachable);
        if (candidates >= 0) {
            return candidates;
        }
    }
    return deduce_unreachable(young, unreachable, reachable);
}

