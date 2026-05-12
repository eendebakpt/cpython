"""Tight benchmark of instance allocation, focusing on patterns where the
memset cost matters most. Each pattern is timed for many trials, then
reduced via robust min-of-many across trial groups."""
import statistics
import sys
import time


def make_class(nattrs):
    body = "    def __init__(self, x):\n"
    for i in range(nattrs):
        body += f"        self.a{i} = x\n"
    if nattrs == 0:
        body = "    pass\n"
    src = "class C:\n" + body
    ns = {}
    exec(src, ns)
    return ns["C"]


class C1:
    def __init__(self, x): self.a = x


class C5:
    def __init__(self, x):
        self.a = x; self.b = x; self.c = x; self.d = x; self.e = x


class C10:
    def __init__(self, x):
        self.a=x; self.b=x; self.c=x; self.d=x; self.e=x
        self.f=x; self.g=x; self.h=x; self.i=x; self.j=x


class C20:
    def __init__(self, x):
        for n in "abcdefghijklmnopqrst":
            object.__setattr__(self, n, x)


N = 1_000_000
TRIALS = 9


def bench_one(cls, args):
    # Warm
    for _ in range(50000):
        cls(*args)
    samples = []
    for _ in range(TRIALS):
        t0 = time.perf_counter()
        for _ in range(N):
            cls(*args)
        samples.append((time.perf_counter() - t0) / N * 1e9)
    samples.sort()
    return samples[0], statistics.median(samples)


print(f"Python: {sys.version.split()[0]}")
print(f"N={N:,} per trial; {TRIALS} trials; reporting min and median ns/instance")
print(f"{'class':20s} {'min ns':>8s} {'median ns':>10s}")
print("-" * 42)
for name, cls in [("C1 (1 attr)", C1), ("C5 (5 attrs)", C5),
                  ("C10 (10 attrs)", C10), ("C20 (20 attrs, slow path)", C20)]:
    mn, md = bench_one(cls, (42,))
    print(f"{name:20s} {mn:>8.1f} {md:>10.1f}")
