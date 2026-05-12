"""Test which __init__-reassignment patterns actually break specialization."""
import dis
import time

N = 200_000
WARMUP = 50_000

def call_opname(fn):
    for ins in dis.get_instructions(fn, adaptive=True):
        if ins.opname.startswith("CALL") and ins.opname != "CALL_INTRINSIC_1":
            return ins.opname
    return "<no CALL>"

def time_fn(fn):
    for _ in range(WARMUP):
        fn()
    t0 = time.perf_counter()
    for _ in range(N):
        fn()
    return (time.perf_counter() - t0) / N * 1e9


# A. __init__ defined inside class body (baseline)
class A:
    def __init__(self, x): self.x = x

def a(): return A(42)


# B. Module-level function assigned in class body
def _ext_init(self, x): self.x = x
class B:
    __init__ = _ext_init

def b(): return B(42)


# C. Function defined inside class body, assigned to __init__
class C:
    def _real_init(self, x): self.x = x
    __init__ = _real_init

def c(): return C(42)


# D. __init__ patched after class body
class D:
    pass
def _d_init(self, x): self.x = x
D.__init__ = _d_init

def d(): return D(42)


# E. __init__ patched AFTER first call to D() — invalidates cache?
class E:
    pass
def _e_init(self, x): self.x = x
E.__init__ = _e_init  # patch immediately after class
for _ in range(WARMUP):  # warm up
    E(42)
# now re-patch with a different function
def _e_init2(self, x): self.x = x * 2
E.__init__ = _e_init2

def e(): return E(42)


# F. Decorator that wraps __init__
def wrap_init(cls):
    orig = cls.__init__
    def new_init(self, *a, **k):
        return orig(self, *a, **k)
    cls.__init__ = new_init
    return cls

@wrap_init
class F:
    def __init__(self, x): self.x = x

def f(): return F(42)


cases = [
    ("A. __init__ inside class body", a),
    ("B. external func, assigned in body", b),
    ("C. inner func, assigned in body", c),
    ("D. __init__ patched after body", d),
    ("E. __init__ patched then re-patched", e),
    ("F. decorator wraps __init__", f),
]

print(f"{'case':45s} {'CALL opname':35s} {'ns/instance':>11s}")
print("-" * 95)
for name, fn in cases:
    ns = time_fn(fn)
    op = call_opname(fn)
    print(f"{name:45s} {op:35s} {ns:>10.1f}")
