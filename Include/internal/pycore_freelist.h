#ifndef Py_INTERNAL_FREELIST_H
#define Py_INTERNAL_FREELIST_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_freelist_state.h"      // struct _Py_freelists
#include "pycore_interp_structs.h"      // PyInterpreterState
#include "pycore_pyatomic_ft_wrappers.h" // FT_ATOMIC_STORE_PTR_RELAXED()
#include "pycore_pystate.h"             // _PyThreadState_GET
#include "pycore_stats.h"               // OBJECT_STAT_INC

static inline struct _Py_freelists *
_Py_freelists_GET(void)
{
#ifdef Py_DEBUG
    _Py_AssertHoldsTstate();
#endif

#ifdef Py_GIL_DISABLED
    PyThreadState *tstate = _PyThreadState_GET();
    return &((_PyThreadStateImpl*)tstate)->freelists;
#else
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->object_state.freelists;
#endif
}

// Pushes `op` to the freelist, calls `freefunc` if the freelist is full
#define _Py_FREELIST_FREE(NAME, op, freefunc) \
    _PyFreeList_Free(&_Py_freelists_GET()->NAME, _PyObject_CAST(op), \
                     Py_ ## NAME ## _MAXFREELIST, freefunc)
// Pushes `op` to the freelist, returns 1 if successful, 0 if the freelist is full
#define _Py_FREELIST_PUSH(NAME, op, limit) \
    _PyFreeList_Push(&_Py_freelists_GET()->NAME, _PyObject_CAST(op), limit)

// Pops a PyObject from the freelist, returns NULL if the freelist is empty.
#define _Py_FREELIST_POP(TYPE, NAME) \
    _Py_CAST(TYPE*, _PyFreeList_Pop(&_Py_freelists_GET()->NAME))

// Pops a non-PyObject data structure from the freelist, returns NULL if the
// freelist is empty.
#define _Py_FREELIST_POP_MEM(NAME) \
    _PyFreeList_PopMem(&_Py_freelists_GET()->NAME)

#define _Py_FREELIST_SIZE(NAME) (int)((_Py_freelists_GET()->NAME).size)

static inline int
_PyFreeList_Push(struct _Py_freelist *fl, void *obj, Py_ssize_t maxsize)
{
    if (fl->size < maxsize && fl->size >= 0) {
        FT_ATOMIC_STORE_PTR_RELAXED(*(void **)obj, fl->freelist);
        fl->freelist = obj;
        fl->size++;
        OBJECT_STAT_INC(to_freelist);
        return 1;
    }
    return 0;
}

static inline void
_PyFreeList_Free(struct _Py_freelist *fl, void *obj, Py_ssize_t maxsize,
                 freefunc dofree)
{
    if (!_PyFreeList_Push(fl, obj, maxsize)) {
        dofree(obj);
    }
}

static inline void *
_PyFreeList_PopNoStats(struct _Py_freelist *fl)
{
    void *obj = fl->freelist;
    if (obj != NULL) {
        assert(fl->size > 0);
        fl->freelist = *(void **)obj;
        fl->size--;
    }
    return obj;
}

static inline PyObject *
_PyFreeList_Pop(struct _Py_freelist *fl)
{
    PyObject *op = _PyFreeList_PopNoStats(fl);
    if (op != NULL) {
        OBJECT_STAT_INC(from_freelist);
        _Py_NewReference(op);
    }
    return op;
}

static inline void *
_PyFreeList_PopMem(struct _Py_freelist *fl)
{
    void *op = _PyFreeList_PopNoStats(fl);
    if (op != NULL) {
        OBJECT_STAT_INC(from_freelist);
    }
    return op;
}

/* Generic size-classed freelist for fixed-size Py_TPFLAGS_TRIVIAL_DEALLOC
 * types (float, complex, range_iterator, ...). A block of a given size class
 * is interchangeable across types of that size; the allocating type re-inits
 * the object header via _PyObject_Init() on pop. */

// Map a fixed tp_basicsize to its size-class index, or -1 if it is too small,
// too large, or not pointer-aligned (and so not eligible for the freelist).
// Macro form is an integer constant expression (usable in static_assert) when
// `basicsize` is constant; the inline form is for runtime tp_basicsize.
#define _Py_SIZECLASS_INDEX(basicsize)                                      \
    (((basicsize) >= _Py_SIZECLASS_MIN                                      \
      && ((basicsize) & (sizeof(void *) - 1)) == 0                          \
      && ((basicsize) - _Py_SIZECLASS_MIN) / sizeof(void *)                 \
             < (size_t)_Py_SIZECLASS_COUNT)                                 \
     ? (int)(((basicsize) - _Py_SIZECLASS_MIN) / sizeof(void *))            \
     : -1)

static inline int
_PyObject_SizeClassIndex(size_t basicsize)
{
    return _Py_SIZECLASS_INDEX(basicsize);
}

// Push `op` onto the freelist for size-class `index` (caller guarantees
// index >= 0). Returns 1 on success, 0 if the freelist is full.
static inline int
_PyObject_SizeClassFreePush(int index, void *op)
{
    return _PyFreeList_Push(&_Py_freelists_GET()->sizeclasses[index], op,
                            Py_sizeclasses_MAXFREELIST);
}

// Pop a (still uninitialized) block from size-class `index`, or NULL if empty.
static inline void *
_PyObject_SizeClassAllocMem(int index)
{
    return _PyFreeList_PopMem(&_Py_freelists_GET()->sizeclasses[index]);
}

// Current number of cached blocks for the size class of `basicsize` (stats).
static inline Py_ssize_t
_PyObject_SizeClassFreeListSize(size_t basicsize)
{
    int index = _PyObject_SizeClassIndex(basicsize);
    return index < 0 ? 0 : _Py_freelists_GET()->sizeclasses[index].size;
}

extern void _PyObject_ClearFreeLists(struct _Py_freelists *freelists, int is_finalization);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_FREELIST_H */
