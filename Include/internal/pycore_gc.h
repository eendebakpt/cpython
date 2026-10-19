#ifndef Py_INTERNAL_GC_H
#define Py_INTERNAL_GC_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_interp_structs.h" // PyGC_Head
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_typedefs.h"      // _PyInterpreterFrame


/* Get an object's GC head */
static inline PyGC_Head* _Py_AS_GC(PyObject *op) {
    char *gc = ((char*)op) - sizeof(PyGC_Head);
    return (PyGC_Head*)gc;
}

/* Get the object given the GC head */
static inline PyObject* _Py_FROM_GC(PyGC_Head *gc) {
    char *op = ((char *)gc) + sizeof(PyGC_Head);
    return (PyObject *)op;
}


/* Bit flags for ob_gc_bits (in Py_GIL_DISABLED builds)
 *
 * Setting the bits requires a relaxed store. The per-object lock must also be
 * held, except when the object is only visible to a single thread (e.g. during
 * object initialization or destruction).
 *
 * Reading the bits requires using a relaxed load, but does not require holding
 * the per-object lock.
 */
#ifdef Py_GIL_DISABLED
#  define _PyGC_BITS_TRACKED        (1<<0)     // Tracked by the GC
#  define _PyGC_BITS_FINALIZED      (1<<1)     // tp_finalize was called
#  define _PyGC_BITS_UNREACHABLE    (1<<2)
#  define _PyGC_BITS_FROZEN         (1<<3)
#  define _PyGC_BITS_SHARED         (1<<4)
#  define _PyGC_BITS_ALIVE          (1<<5)    // Reachable from a known root.
#  define _PyGC_BITS_DEFERRED       (1<<6)    // Use deferred reference counting
#endif

#ifdef Py_GIL_DISABLED

static inline void
_PyObject_SET_GC_BITS(PyObject *op, uint8_t new_bits)
{
    uint8_t bits = _Py_atomic_load_uint8_relaxed(&op->ob_gc_bits);
    _Py_atomic_store_uint8_relaxed(&op->ob_gc_bits, bits | new_bits);
}

static inline int
_PyObject_HAS_GC_BITS(PyObject *op, uint8_t bits)
{
    return (_Py_atomic_load_uint8_relaxed(&op->ob_gc_bits) & bits) != 0;
}

static inline void
_PyObject_CLEAR_GC_BITS(PyObject *op, uint8_t bits_to_clear)
{
    uint8_t bits = _Py_atomic_load_uint8_relaxed(&op->ob_gc_bits);
    _Py_atomic_store_uint8_relaxed(&op->ob_gc_bits, bits & ~bits_to_clear);
}

#endif

/* True if the object is currently tracked by the GC. */
static inline int _PyObject_GC_IS_TRACKED(PyObject *op) {
#ifdef Py_GIL_DISABLED
    return _PyObject_HAS_GC_BITS(op, _PyGC_BITS_TRACKED);
#else
    PyGC_Head *gc = _Py_AS_GC(op);
    return (gc->_gc_next != 0);
#endif
}
#define _PyObject_GC_IS_TRACKED(op) _PyObject_GC_IS_TRACKED(_Py_CAST(PyObject*, op))

/* True if the object may be tracked by the GC in the future, or already is.
   This can be useful to implement some optimizations. */
static inline int _PyObject_GC_MAY_BE_TRACKED(PyObject *obj) {
    if (!PyObject_IS_GC(obj)) {
        return 0;
    }
    if (PyTuple_CheckExact(obj)) {
        return _PyObject_GC_IS_TRACKED(obj);
    }
    return 1;
}

#ifdef Py_GIL_DISABLED

/* True if memory the object references is shared between
 * multiple threads and needs special purpose when freeing
 * those references due to the possibility of in-flight
 * lock-free reads occurring.  The object is responsible
 * for calling _PyMem_FreeDelayed on the referenced
 * memory. */
static inline int _PyObject_GC_IS_SHARED(PyObject *op) {
    return _PyObject_HAS_GC_BITS(op, _PyGC_BITS_SHARED);
}
#define _PyObject_GC_IS_SHARED(op) _PyObject_GC_IS_SHARED(_Py_CAST(PyObject*, op))

static inline void _PyObject_GC_SET_SHARED(PyObject *op) {
    _PyObject_SET_GC_BITS(op, _PyGC_BITS_SHARED);
}
#define _PyObject_GC_SET_SHARED(op) _PyObject_GC_SET_SHARED(_Py_CAST(PyObject*, op))

#endif

/* Bit flags for _gc_prev */
/* Bit 0 is set when tp_finalize is called */
#define _PyGC_PREV_MASK_FINALIZED  ((uintptr_t)1)
/* Bit 1 is set when the object is in generation which is GCed currently. */
#define _PyGC_PREV_MASK_COLLECTING ((uintptr_t)2)

#define _PyGC_PREV_SHIFT           2
#define _PyGC_PREV_MASK            (((uintptr_t) -1) << _PyGC_PREV_SHIFT)

/* set for debugging information */
#define _PyGC_DEBUG_STATS             (1<<0) /* print collection statistics */
#define _PyGC_DEBUG_COLLECTABLE       (1<<1) /* print collectable objects */
#define _PyGC_DEBUG_UNCOLLECTABLE     (1<<2) /* print uncollectable objects */
#define _PyGC_DEBUG_SAVEALL           (1<<5) /* save all garbage in gc.garbage */
#define _PyGC_DEBUG_LEAK              _PyGC_DEBUG_COLLECTABLE | \
                                      _PyGC_DEBUG_UNCOLLECTABLE | \
                                      _PyGC_DEBUG_SAVEALL

typedef enum {
    // GC was triggered by heap allocation
    _Py_GC_REASON_HEAP,

    // GC was called during shutdown
    _Py_GC_REASON_SHUTDOWN,

    // GC was called by gc.collect() or PyGC_Collect()
    _Py_GC_REASON_MANUAL
} _PyGC_Reason;

// Lowest bit of _gc_next is used for flags only in GC.
// But it is always 0 for normal code.
static inline PyGC_Head* _PyGCHead_NEXT(PyGC_Head *gc) {
    uintptr_t next = gc->_gc_next;
    return (PyGC_Head*)next;
}
static inline void _PyGCHead_SET_NEXT(PyGC_Head *gc, PyGC_Head *next) {
    gc->_gc_next = (uintptr_t)next;
}

// Lowest two bits of _gc_prev is used for _PyGC_PREV_MASK_* flags.
static inline PyGC_Head* _PyGCHead_PREV(PyGC_Head *gc) {
    uintptr_t prev = (gc->_gc_prev & _PyGC_PREV_MASK);
    return (PyGC_Head*)prev;
}

static inline void _PyGCHead_SET_PREV(PyGC_Head *gc, PyGC_Head *prev) {
    uintptr_t uprev = (uintptr_t)prev;
    assert((uprev & ~_PyGC_PREV_MASK) == 0);
    gc->_gc_prev = ((gc->_gc_prev & ~_PyGC_PREV_MASK) | uprev);
}

static inline int _PyGC_FINALIZED(PyObject *op) {
#ifdef Py_GIL_DISABLED
    return _PyObject_HAS_GC_BITS(op, _PyGC_BITS_FINALIZED);
#else
    PyGC_Head *gc = _Py_AS_GC(op);
    return ((gc->_gc_prev & _PyGC_PREV_MASK_FINALIZED) != 0);
#endif
}
static inline void _PyGC_SET_FINALIZED(PyObject *op) {
#ifdef Py_GIL_DISABLED
    _PyObject_SET_GC_BITS(op, _PyGC_BITS_FINALIZED);
#else
    PyGC_Head *gc = _Py_AS_GC(op);
    gc->_gc_prev |= _PyGC_PREV_MASK_FINALIZED;
#endif
}
static inline void _PyGC_CLEAR_FINALIZED(PyObject *op) {
#ifdef Py_GIL_DISABLED
    _PyObject_CLEAR_GC_BITS(op, _PyGC_BITS_FINALIZED);
#else
    PyGC_Head *gc = _Py_AS_GC(op);
    gc->_gc_prev &= ~_PyGC_PREV_MASK_FINALIZED;
#endif
}

extern void _Py_ScheduleGC(PyThreadState *tstate);


/* Tell the GC to track this object.
 *
 * The object must not be tracked by the GC.
 *
 * NB: While the object is tracked by the collector, it must be safe to call the
 * ob_traverse method.
 *
 * Internal note: interp->gc.generation0->_gc_prev doesn't have any bit flags
 * because it's not an object header. So we don't use _PyGCHead_PREV() and
 * _PyGCHead_SET_PREV() for it to avoid unnecessary bitwise operations.
 *
 * See also the public PyObject_GC_Track() function.
 */
static inline void _PyObject_GC_TRACK(
// The preprocessor removes _PyObject_ASSERT_FROM() calls if NDEBUG is defined
#ifndef NDEBUG
    const char *filename, int lineno,
#endif
    PyObject *op)
{
    _PyObject_ASSERT_FROM(op, !_PyObject_GC_IS_TRACKED(op),
                          "object already tracked by the garbage collector",
                          filename, lineno, __func__);
#ifdef Py_GIL_DISABLED
    _PyObject_SET_GC_BITS(op, _PyGC_BITS_TRACKED);
#else
    PyGC_Head *gc = _Py_AS_GC(op);
    _PyObject_ASSERT_FROM(op,
                          (gc->_gc_prev & _PyGC_PREV_MASK_COLLECTING) == 0,
                          "object is in generation which is garbage collected",
                          filename, lineno, __func__);

    struct _gc_runtime_state *gcstate = &_PyInterpreterState_GET()->gc;
    PyGC_Head *generation0 = gcstate->generation0;
    PyGC_Head *last = (PyGC_Head*)(generation0->_gc_prev);
    _PyGCHead_SET_NEXT(last, gc);
    _PyGCHead_SET_PREV(gc, last);
    _PyGCHead_SET_NEXT(gc, generation0);
    generation0->_gc_prev = (uintptr_t)gc;
    gcstate->heap_size++;
#endif
}

/* Tell the GC to stop tracking this object.
 *
 * Internal note: This may be called while GC. So _PyGC_PREV_MASK_COLLECTING
 * must be cleared. But _PyGC_PREV_MASK_FINALIZED bit is kept.
 *
 * The object must be tracked by the GC.
 *
 * See also the public PyObject_GC_UnTrack() which accept an object which is
 * not tracked.
 */
static inline void _PyObject_GC_UNTRACK(
// The preprocessor removes _PyObject_ASSERT_FROM() calls if NDEBUG is defined
#ifndef NDEBUG
    const char *filename, int lineno,
#endif
    PyObject *op)
{
    _PyObject_ASSERT_FROM(op, _PyObject_GC_IS_TRACKED(op),
                          "object not tracked by the garbage collector",
                          filename, lineno, __func__);

#ifdef Py_GIL_DISABLED
    _PyObject_CLEAR_GC_BITS(op, _PyGC_BITS_TRACKED);
#else
    PyGC_Head *gc = _Py_AS_GC(op);
    PyGC_Head *prev = _PyGCHead_PREV(gc);
    PyGC_Head *next = _PyGCHead_NEXT(gc);
    _PyGCHead_SET_NEXT(prev, next);
    _PyGCHead_SET_PREV(next, prev);
    gc->_gc_next = 0;
    gc->_gc_prev &= _PyGC_PREV_MASK_FINALIZED;
    struct _gc_runtime_state *gcstate = &_PyInterpreterState_GET()->gc;
    gcstate->heap_size--;
#endif
}



/*
   NOTE: about untracking of mutable objects.

   Certain types of container cannot participate in a reference cycle, and
   so do not need to be tracked by the garbage collector. Untracking these
   objects reduces the cost of garbage collections. However, determining
   which objects may be untracked is not free, and the costs must be
   weighed against the benefits for garbage collection.

   There are two possible strategies for when to untrack a container:

   i) When the container is created.
   ii) When the container is examined by the garbage collector.

   Tuples containing only immutable objects (integers, strings etc, and
   recursively, tuples of immutable objects) do not need to be tracked.
   The interpreter creates a large number of tuples, many of which will
   not survive until garbage collection. It is therefore not worthwhile
   to untrack eligible tuples at creation time.

   Instead, all tuples except the empty tuple are tracked when created.
   During garbage collection it is determined whether any surviving tuples
   can be untracked. A tuple can be untracked if all of its contents are
   already not tracked. Tuples are examined for untracking in all garbage
   collection cycles. It may take more than one cycle to untrack a tuple.

   Dictionaries containing only immutable objects also do not need to be
   tracked. Dictionaries are untracked when created. If a tracked item is
   inserted into a dictionary (either as a key or value), the dictionary
   becomes tracked. During a full garbage collection (all generations),
   the collector will untrack any dictionaries whose contents are not
   tracked.

   The module provides the python function is_tracked(obj), which returns
   the CURRENT tracking status of the object. Subsequent garbage
   collections may change the tracking status of the object.

   Untracking of certain containers was introduced in issue #4688, and
   the algorithm was refined in response to issue #14775.
*/

extern void _PyGC_InitState(struct _gc_runtime_state *);

/* Closure-sum cycle detection (experimental, PYTHON_GC_CLOSURE).  The
 * detectors are closure_pass() in gc.c and gc_free_threading.c; the shared
 * configuration and statistics code is in gc_closure.c. */
enum _PyGC_ClosureMode {
    _PyGC_CLOSURE_OFF = 0,        /* classic algorithm only (default) */
    _PyGC_CLOSURE_ALL = 1,        /* closure pass for every collection */
    _PyGC_CLOSURE_YOUNG = 2,      /* closure pass unless the oldest generation */
    _PyGC_CLOSURE_ALTERNATE = 3,  /* alternate between the two */
    _PyGC_CLOSURE_TRUTH = 4,      /* closure pass for statistics, then the
                                     classic algorithm for the result */
    _PyGC_CLOSURE_OLD = 5,        /* closure pass for the oldest generation only */
    _PyGC_CLOSURE_ADAPTIVE = 6,   /* per generation, the detector that was
                                     cheaper per candidate last time; the
                                     other one is re-probed periodically */
};

#define _PyGC_CLOSURE_HIST 24
#define _PyGC_CLOSURE_TYPES 24
struct _gc_closure_type_count {
    char name[48];
    uint64_t count;
};
struct _gc_closure_gen_stats {
    uint64_t collections;          /* collections of this generation */
    uint64_t closure_collections;  /* ... that used the closure pass */
    uint64_t truth_collections;    /* ... that also ran deduce_unreachable() */
    uint64_t candidates;
    uint64_t closures;
    uint64_t pure_closures;
    uint64_t pure_objects;         /* garbage found by the closure pass */
    uint64_t true_garbage;         /* garbage found by deduce_unreachable() */
    uint64_t missed_garbage;       /* true garbage inside unknown closures */
    uint64_t unknown_with_garbage; /* unknown closures hiding garbage */
    uint64_t phantom_closures;     /* closures with E > R (broken traverse) */
    uint64_t pure_but_live;        /* pure closures not fully garbage: must be 0 */
    uint64_t edges;                /* E over all closures */
    uint64_t traversals_old;       /* tp_traverse calls by deduce_unreachable() */
    uint64_t queue_high_water;
    uint64_t fallbacks;            /* closure pass gave up (no memory) */
    uint64_t log_oom;
    PyTime_t closure_time;         /* spent in closure_pass() */
    PyTime_t deduce_time;          /* spent in deduce_unreachable() */
    PyTime_t mark_time;            /* spent marking from the roots */
    uint64_t marked;               /* objects taken out by marking */
    uint64_t size_hist[_PyGC_CLOSURE_HIST];       /* log2 closure sizes */
    uint64_t pure_size_hist[_PyGC_CLOSURE_HIST];
    struct _gc_closure_type_count blockers[_PyGC_CLOSURE_TYPES];
    struct _gc_closure_type_count missed_types[_PyGC_CLOSURE_TYPES];
};
struct _gc_closure_stats {
    struct _gc_closure_gen_stats gens[3];   /* NUM_GENERATIONS */
};

/* Record of the closures of one collection (truth mode). */
struct _gc_closure_record {
    Py_ssize_t start;       /* offset into arena */
    Py_ssize_t size;
    int pure;
};
struct _gc_closure_log {
    PyObject **arena;
    Py_ssize_t arena_len, arena_cap;
    struct _gc_closure_record *recs;
    Py_ssize_t recs_len, recs_cap;
    int oom;
};

/* Process-wide configuration and statistics of the experiment (deliberately
 * not per interpreter: fields in PyInterpreterState would make every
 * extension module ABI-dependent on the experiment). */
struct _gc_closure_config {
    int mode;                   /* enum _PyGC_ClosureMode, PYTHON_GC_CLOSURE */
    int mark_alive;             /* PYTHON_GC_MARK: mark from the roots before a
                                   full collection (default build) */
    int dag;                    /* PYTHON_GC_DAG: judge closures in a backward
                                   sweep over the record DAG; 1: every
                                   collection, 2: full collections only */
    unsigned int counter;       /* for _PyGC_CLOSURE_ALTERNATE */
    int classic_every;          /* PYTHON_GC_CLASSIC_EVERY=k: every k-th full
                                   collection uses the classic algorithm, which
                                   bounds the delay of garbage the closure pass
                                   cannot decide (0: never) */
    unsigned int full_counter;
    const char *stats_path;     /* PYTHON_GC_CLOSURE_STATS */
    struct _gc_closure_stats *stats;
    /* _PyGC_CLOSURE_ADAPTIVE: last measured cost per candidate (ns) of each
     * detector, per generation; 0 = no sample yet */
    struct _gc_closure_adapt {
        double closure_cost;
        double classic_cost;
        unsigned int n;
        int last_closure;
    } adapt[3];
};
extern struct _gc_closure_config _PyGC_closure;
/* Record the cost of the detector just used (adaptive mode). */
extern void _PyGC_ClosureRecordCost(int generation, int used_closure,
                                    PyTime_t ns, Py_ssize_t candidates);

/* Record DAG of the deferred-verdict closure pass (PYTHON_GC_DAG), shared
 * by both collectors; see closure_pass_dag() in gc.c.  Records are created
 * in visit order; an edge from the record being built (cur) into an earlier
 * record is recorded with _PyGC_DagEdge(); _PyGC_DagSweep() judges every
 * record garbage iff its external references (rc_sum - edges) are exactly
 * the edges from records already judged garbage. */
struct _gc_dag_record {
    Py_ssize_t rc_sum;      /* R */
    Py_ssize_t edges;       /* E, edges inside the record */
    Py_ssize_t garbage_in;  /* edges into this record from garbage records */
    Py_ssize_t out_start;   /* this record's out-edges in the edge arena */
    Py_ssize_t out_count;
    Py_ssize_t stamp;       /* == cur while tmp is valid */
    Py_ssize_t tmp;         /* edges from cur into this record */
    Py_ssize_t size;        /* members (statistics) */
    int tainted;            /* never garbage (deferred references unseen) */
    int garbage;
};
struct _gc_dag {
    struct _gc_dag_record *rec;
    Py_ssize_t count, cap;  /* records 1..count-1; 0 means undiscovered */
    Py_ssize_t cur;         /* record being built */
    Py_ssize_t *touched;    /* records with edges from cur, for the arena */
    Py_ssize_t ntouched, touched_cap;
    Py_ssize_t *etarget;    /* edge arena: (target record, edge count) */
    Py_ssize_t *ecount;
    Py_ssize_t elen, ecap, ecount_cap;
};
extern int _PyGC_DagNewRecord(struct _gc_dag *dag);    /* sets cur; -1 on OOM */
extern int _PyGC_DagEdge(struct _gc_dag *dag, Py_ssize_t target);
extern int _PyGC_DagFlush(struct _gc_dag *dag);        /* end of record cur */
extern void _PyGC_DagSweep(struct _gc_dag *dag);
extern void _PyGC_DagFree(struct _gc_dag *dag);

extern void _PyGC_ClosureInit(void);
extern void _PyGC_ClosureFini(void);
extern int _PyGC_ClosureWanted(int generation);
/* Statistics slot of a generation (NULL if statistics are off); counts the
 * collection. */
extern struct _gc_closure_gen_stats *_PyGC_ClosureGenStats(int generation);
extern void _PyGC_ClosureAccount(struct _gc_closure_gen_stats *st,
                                 Py_ssize_t size, Py_ssize_t rc_sum,
                                 Py_ssize_t edges, int pure);
extern void _PyGC_ClosureCountType(struct _gc_closure_type_count *table,
                                   PyTypeObject *tp);
extern int _PyGC_ClosureLogAdd(struct _gc_closure_log *log,
                               PyObject **members, Py_ssize_t n, int pure);
extern void _PyGC_ClosureLogFree(struct _gc_closure_log *log);
extern PyObject *_PyGC_ClosureStats(void);
extern void _PyGC_ClosureResetStats(void);
#ifndef Py_GIL_DISABLED
extern PyObject *_PyGC_ClosureBlockers(void);  /* truth-mode diagnostic */
#endif

extern Py_ssize_t _PyGC_Collect(PyThreadState *tstate, int generation, _PyGC_Reason reason);
extern void _PyGC_CollectNoFail(PyThreadState *tstate);

/* Freeze objects tracked by the GC and ignore them in future collections. */
extern void _PyGC_Freeze(PyInterpreterState *interp);
/* Unfreezes objects placing them in the oldest generation */
extern void _PyGC_Unfreeze(PyInterpreterState *interp);
/* Number of frozen objects */
extern Py_ssize_t _PyGC_GetFreezeCount(PyInterpreterState *interp);

extern PyObject *_PyGC_GetObjects(PyInterpreterState *interp, int generation);
extern PyObject *_PyGC_GetReferrers(PyInterpreterState *interp, PyObject *objs);

// Functions to clear types free lists
extern void _PyGC_ClearAllFreeLists(PyInterpreterState *interp);
extern void _Py_RunGC(PyThreadState *tstate);

union _PyStackRef;

// GC visit callback for tracked interpreter frames
// GH-150766: exported for greenlet
PyAPI_FUNC(int) _PyGC_VisitFrameStack(_PyInterpreterFrame *frame, visitproc visit, void *arg);
PyAPI_FUNC(int) _PyGC_VisitStackRef(union _PyStackRef *ref, visitproc visit, void *arg);

#ifdef Py_GIL_DISABLED
extern void _PyGC_VisitObjectsWorldStopped(PyInterpreterState *interp,
                                           gcvisitobjects_t callback, void *arg);
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_GC_H */
