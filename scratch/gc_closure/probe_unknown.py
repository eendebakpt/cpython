import gc, weakref, sys
class A:
    pass
keep = []
def make():
    a = A()
    a.me = a
    live = []
    keep.append(live)
    a.live = live
    print("a refcnt", sys.getrefcount(a) - 1, "live refcnt", sys.getrefcount(live) - 1,
          "a tracked", gc.is_tracked(a), "live tracked", gc.is_tracked(live))
    print("gen0 contains a:", any(o is a for o in gc.get_objects(0)),
          "live:", any(o is live for o in gc.get_objects(0)))
    return weakref.ref(a)
gc.disable()
r = make()
print("mode", gc._closure_mode())
if gc._closure_mode() == 4:
    gc._closure_reset_stats()
n = gc.collect(0)
print("collected", n, "r()", r())
if gc._closure_mode() == 4:
    st = gc._closure_stats()[0]
    print({k: v for k, v in st.items() if k in ("closures", "pure_closures", "pure_objects", "true_garbage", "missed_garbage", "blockers", "missed_types")})
