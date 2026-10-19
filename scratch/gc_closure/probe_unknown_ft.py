import gc, weakref
class A:
    pass
keep = []
gc.freeze()
def make():
    a = A()
    a.me = a
    live = ([],) * 50
    keep.append(live)
    a.live = live
    return weakref.ref(a)
gc.disable()
r = make()
print("mode", gc._closure_mode(), "frozen", gc.get_freeze_count())
if gc._closure_mode() == 4:
    gc._closure_reset_stats()
n = gc.collect(0)
print("collect(0) ->", n, "r() alive:", r() is not None)
if gc._closure_mode() == 4:
    st = gc._closure_stats()[0]
    print({k: v for k, v in st.items() if k in ("candidates", "closures", "pure_closures", "pure_objects",
          "true_garbage", "missed_garbage", "unknown_with_garbage", "blockers", "missed_types", "pure_but_live")})
n = gc.collect()
print("collect() ->", n, "r() alive:", r() is not None)
