"""Measure CALL_ALLOC_AND_ENTER_INIT hit rate across the patterns one
actually finds in real Python code.

For each pattern we (a) instantiate enough times to force specialization,
(b) walk the test function's bytecode to see what specialized form the
CALL adopted, and (c) measure timing for context.
"""
import collections
import dataclasses
import dis
import enum
import sys
import time
import typing

N = 200_000
WARMUP = 50_000


def call_opname(fn, target_lineno=None):
    """Return the (specialized) opname of the CALL inside fn."""
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


# --- Patterns ---

# 1. Bare class, no __init__
class P1_Empty:
    pass

def p1(): return P1_Empty()


# 2. __init__ with no body
class P2_Init:
    def __init__(self, x): pass

def p2(): return P2_Init(42)


# 3. __init__ with one attribute
class P3_OneAttr:
    def __init__(self, x): self.x = x

def p3(): return P3_OneAttr(42)


# 4. Multi-attribute
class P4_FiveAttrs:
    def __init__(self, a, b, c, d, e):
        self.a = a; self.b = b; self.c = c; self.d = d; self.e = e

def p4(): return P4_FiveAttrs(1, 2, 3, 4, 5)


# 5. __slots__
class P5_Slots:
    __slots__ = ("x",)
    def __init__(self, x): self.x = x

def p5(): return P5_Slots(42)


# 6. Subclass with super().__init__
class P6_Base:
    def __init__(self, x): self.x = x

class P6_Sub(P6_Base):
    def __init__(self, x, y):
        super().__init__(x)
        self.y = y

def p6(): return P6_Sub(1, 2)


# 7. Dataclass
@dataclasses.dataclass
class P7_DC:
    x: int
    y: int

def p7(): return P7_DC(1, 2)


# 8. NamedTuple
class P8_NT(typing.NamedTuple):
    x: int
    y: int

def p8(): return P8_NT(1, 2)


# 9. Class with custom __new__
class P9_NewOnly:
    def __new__(cls, x):
        obj = super().__new__(cls)
        obj.x = x
        return obj

def p9(): return P9_NewOnly(42)


# 10. Class with __init__ patched after class body
def _patched_init(self, x): self.x = x
class P10_Patched:
    pass
P10_Patched.__init__ = _patched_init

def p10(): return P10_Patched(42)


# 11. Class with __init__ that takes *args
class P11_VarArgs:
    def __init__(self, *args): self.args = args

def p11(): return P11_VarArgs(1, 2, 3)


# 12. Class with __init__ that takes **kwargs (called positionally)
class P12_KwArgs:
    def __init__(self, **kw): self.kw = kw

def p12(): return P12_KwArgs()


# 13. Enum
class P13_Enum(enum.Enum):
    A = 1
    B = 2

def p13(): return P13_Enum.A  # not really an instantiation, but a CALL pattern


# 14. Property descriptors / type with __set_name__ etc. (subset)
class P14_Prop:
    @property
    def x(self): return self._x
    @x.setter
    def x(self, v): self._x = v
    def __init__(self, x):
        self.x = x

def p14(): return P14_Prop(42)


# 15. Subclass of dict
class P15_DictSub(dict):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)

def p15(): return P15_DictSub({"a": 1})


patterns = [
    ("01 Empty class, no __init__", p1),
    ("02 __init__ with pass", p2),
    ("03 __init__ one attr", p3),
    ("04 __init__ five attrs", p4),
    ("05 __slots__", p5),
    ("06 Subclass + super().__init__", p6),
    ("07 @dataclass", p7),
    ("08 NamedTuple", p8),
    ("09 Custom __new__", p9),
    ("10 __init__ patched after class body", p10),
    ("11 __init__(*args)", p11),
    ("12 __init__(**kw)", p12),
    ("13 Enum member access", p13),
    ("14 Class with @property", p14),
    ("15 dict subclass", p15),
]


print(f"{'pattern':45s} {'CALL opname':35s} {'ns/instance':>11s}")
print("-" * 95)

for name, fn in patterns:
    ns = time_fn(fn)        # this warms up the call site
    op = call_opname(fn)    # now read the specialized opname
    print(f"{name:45s} {op:35s} {ns:>10.1f}")
