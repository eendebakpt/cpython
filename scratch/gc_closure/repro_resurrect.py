import gc
import sys

N = 100


class A:
    def __init__(self):
        self.me = self


class Z(A):
    def __del__(self):
        zs.append(self)


zs = []


def getstats():
    d = gc.get_stats()[-1]
    return d['collected'], d['uncollectable']


print("mode", gc._closure_mode())
gc.collect()
gc.disable()
for i in range(N):
    A()
print("A only:", gc.collect())
Z()
print("Z only:", gc.collect(), "zs", len(zs))
for i in range(N):
    A()
Z()
print("A + Z:", gc.collect(), "zs", len(zs))
for z in zs:
    print("  Z refcnt", sys.getrefcount(z) - 1, "tracked", gc.is_tracked(z),
          "finalized", gc.is_finalized(z),
          "referrers", [type(r).__name__ for r in gc.get_referrers(z)])
zs_copy = list(zs)
zs.clear()
for z in zs_copy:
    print("  after clear: Z refcnt", sys.getrefcount(z) - 1,
          "referrers", [type(r).__name__ for r in gc.get_referrers(z) if r is not zs_copy])
del z
zs_copy.clear()
print("final:", gc.collect())
print("garbage:", gc.garbage)
