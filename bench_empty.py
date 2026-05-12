"""Tight benchmark of Empty() (no __init__) before/after A1."""
import statistics
import sys
import time


class Empty:
    pass


class WithInit:
    def __init__(self):
        pass


N = 5_000_000
TRIALS = 11


def bench(cls):
    for _ in range(100000):
        cls()
    samples = []
    for _ in range(TRIALS):
        t0 = time.perf_counter()
        for _ in range(N):
            cls()
        samples.append((time.perf_counter() - t0) / N * 1e9)
    samples.sort()
    return samples[0], statistics.median(samples)


print(f"Python: {sys.version.split()[0]}")
print(f"N={N:,}; {TRIALS} trials; reporting min and median ns/instance")
print()
for name, cls in [("Empty()       ", Empty), ("WithInit()    ", WithInit)]:
    mn, md = bench(cls)
    print(f"  {name} min={mn:5.1f} ns  median={md:5.1f} ns")
