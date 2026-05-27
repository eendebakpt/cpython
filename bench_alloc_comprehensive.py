"""Comprehensive instance-allocation benchmark covering all four paths through
_PyType_AllocNoTrack:

  1. Plain class with __init__ (INLINE_VALUES, no __slots__) -- the dominant
     fast-path case. The previous benchmark covered this only.
  2. Class with __slots__ (no INLINE_VALUES, tp_itemsize == 0). General path,
     tp_basicsize > sizeof(PyObject), so the slow-path memset fires.
  3. Variable-size subclass (tp_itemsize > 0). General path, ob_size set via
     _PyObject_InitVar. Subclassing tuple is the canonical case.
  4. @dataclass instance. Goes through the same _PyType_AllocNoTrack path as
     case 1 -- dataclasses do not override tp_alloc.

For each case we report min and median ns/instance across many trials.
"""
import dataclasses
import statistics
import sys
import time


# 1. Plain inline-values class with a Python __init__
class PlainInit:
    def __init__(self, x):
        self.a = x
        self.b = x
        self.c = x
        self.d = x
        self.e = x


# 1b. Plain inline-values class with no __init__ at all (the bare allocation,
#     without the Python __init__ frame -- exercises the alloc path without
#     the surrounding setup).
class PlainEmpty:
    pass


# 2. Class with __slots__: no INLINE_VALUES, tp_basicsize > sizeof(PyObject)
class SlotsCls:
    __slots__ = ("a", "b", "c", "d", "e")
    def __init__(self, x):
        self.a = x
        self.b = x
        self.c = x
        self.d = x
        self.e = x


# 3. Variable-size subclass: tp_itemsize > 0
class TupleSub(tuple):
    pass


# 4. Dataclass -- verify it goes through _PyType_AllocNoTrack
@dataclasses.dataclass
class DC:
    a: int
    b: int
    c: int
    d: int
    e: int


N = 1_000_000
TRIALS = 11


def bench(name, callable_, args=()):
    # Warm
    for _ in range(50000):
        callable_(*args)
    samples = []
    for _ in range(TRIALS):
        t0 = time.perf_counter()
        for _ in range(N):
            callable_(*args)
        samples.append((time.perf_counter() - t0) / N * 1e9)
    samples.sort()
    return name, samples[0], statistics.median(samples)


def main():
    print(f"Python: {sys.version.split()[0]}")
    print(f"N={N:,} per trial, {TRIALS} trials, ns/instance")
    print()
    cases = [
        ("PlainEmpty()      (inline-values, no __init__)", PlainEmpty, ()),
        ("PlainInit(1)      (inline-values, 5-attr init)", PlainInit, (1,)),
        ("DC(1,2,3,4,5)     (@dataclass, 5 attrs)", DC, (1, 2, 3, 4, 5)),
        ("SlotsCls(1)       (__slots__, 5-attr init)", SlotsCls, (1,)),
        ("TupleSub((1,2,3)) (tp_itemsize>0)", TupleSub, ((1, 2, 3),)),
    ]
    print(f"  {'case':52s}  {'min ns':>8s}  {'median ns':>10s}")
    print("  " + "-" * 76)
    for name, fn, args in cases:
        _, mn, md = bench(name, fn, args)
        print(f"  {name:52s} {mn:>9.1f}  {md:>10.1f}")


if __name__ == "__main__":
    main()
