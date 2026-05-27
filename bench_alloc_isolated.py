"""Per-case isolated allocation benchmark.

Runs each case in its own subprocess to eliminate cross-case GC and
specialization state pollution. Disables GC during measurement.

Use this when in-process measurement is noisy. On the original Windows
desktop, the in-process benchmark showed >2x variance for DC / SlotsCls
across runs of the same binary; subprocess isolation removed that.
"""
import json
import statistics
import subprocess
import sys


CASE_SETUP = {
    # 1. Plain class with no __init__ -- inline-values fast path,
    # no Python __init__ frame to enter.
    "PlainEmpty": """
class C: pass
fn = C
args = ()
""",

    # 2. Plain class with a 5-attribute Python __init__. Specializes to
    # CALL_ALLOC_AND_ENTER_INIT.
    "PlainInit": """
class C:
    def __init__(self, x):
        self.a = x; self.b = x; self.c = x; self.d = x; self.e = x
fn = C
args = (1,)
""",

    # 3. @dataclass with 5 fields. Goes through the same path as PlainInit
    # (dataclass generates a Python __init__; does NOT override tp_alloc).
    "DC": """
import dataclasses
@dataclasses.dataclass
class C:
    a: int
    b: int
    c: int
    d: int
    e: int
fn = C
args = (1, 2, 3, 4, 5)
""",

    # 4. __slots__ class -- no INLINE_VALUES, so the general (slow) path
    # of _PyType_AllocNoTrack fires. tp_itemsize == 0, tp_basicsize larger
    # than sizeof(PyObject) because of slot members.
    "SlotsCls": """
class C:
    __slots__ = ('a','b','c','d','e')
    def __init__(self, x):
        self.a = x; self.b = x; self.c = x; self.d = x; self.e = x
fn = C
args = (1,)
""",

    # 5. Subclass of tuple -- tp_itemsize > 0. Exercises _PyObject_InitVar
    # path of _PyType_AllocNoTrack.
    "TupleSub": """
class C(tuple):
    pass
fn = C
args = ((1, 2, 3),)
""",
}


CHILD_SCRIPT = """
import gc, json, statistics, time
{setup}
N = {N}
TRIALS = {TRIALS}

# warmup -- triggers tier-1 specialization and any tier-2 tracing
for _ in range(50000):
    fn(*args)

gc.collect()
gc.disable()
samples = []
for _ in range(TRIALS):
    t0 = time.perf_counter()
    for _ in range(N):
        fn(*args)
    samples.append((time.perf_counter() - t0) / N * 1e9)
gc.enable()
samples.sort()
print(json.dumps({{
    "min": samples[0],
    "median": statistics.median(samples),
    "mean": statistics.mean(samples),
    "samples": samples,
}}))
"""


def run_case(name, N=1_000_000, TRIALS=11):
    setup = CASE_SETUP[name]
    script = CHILD_SCRIPT.format(setup=setup, N=N, TRIALS=TRIALS)
    out = subprocess.run([sys.executable, "-c", script],
                         capture_output=True, text=True, check=True)
    return json.loads(out.stdout.strip().splitlines()[-1])


def main():
    print(f"Python: {sys.version.split()[0]}")
    print(f"Per-case subprocess isolation, GC disabled during measurement.")
    print()
    cases = ["PlainEmpty", "PlainInit", "DC", "SlotsCls", "TupleSub"]
    results = {}
    print(f"  {'case':14s} {'min':>9s} {'median':>9s} {'mean':>9s}")
    print("  " + "-" * 48)
    for name in cases:
        r = run_case(name)
        results[name] = r
        print(f"  {name:14s} {r['min']:>8.1f}  {r['median']:>8.1f}  {r['mean']:>8.1f}")
    return results


if __name__ == "__main__":
    main()
