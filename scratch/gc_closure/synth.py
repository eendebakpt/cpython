"""Synthetic workloads for the closure-sum detector.

Run with PYTHON_GC_CLOSURE=4 (closure pass + old algorithm as truth) so that
gc._closure_stats() reports recall; any other mode only reports timings.

    PYTHON_GC_CLOSURE=4 python synth.py [case ...]
"""
import gc
import sys
import time

N = 20000


class A:
    def __init__(self):
        self.other = None
        self.extra = None


class B(A):
    pass


def case_self_list():
    """l.append(l): one-object cycles."""
    for _ in range(N):
        l = []
        l.append(l)


def case_pair_dict():
    """two-object dict cycles."""
    for _ in range(N):
        a = {}
        b = {"a": a}
        a["b"] = b


def case_instances():
    """A <-> B instance cycles (classes are old)."""
    for _ in range(N):
        a = A()
        b = B()
        a.other = b
        b.other = a


LIVE = []


def case_instances_live_ref():
    """A <-> B cycles that also reference a young object kept alive elsewhere
    (the expected failure mode: impure closure)."""
    LIVE.clear()
    for _ in range(N):
        shared = [1, 2, 3]
        LIVE.append(shared)
        a = A()
        b = B()
        a.other = b
        b.other = a
        a.extra = shared


def _raise_and_catch():
    try:
        1 / 0
    except Exception as e:
        err = e        # e -> traceback -> frame -> err: cycle
        return err


def case_exceptions():
    """exception / traceback / frame cycles."""
    for _ in range(N):
        _raise_and_catch()


def _make_closure():
    def inner():
        return inner
    return inner


def case_closures():
    """function <-> cell cycles."""
    for _ in range(N):
        _make_closure()


def case_live_tree():
    """a large live young structure: no garbage, measures cost."""
    root = {}
    nodes = [root]
    for i in range(100000):
        parent = nodes[i // 4]
        child = {}
        parent[i] = child
        nodes.append(child)
    LIVE.append(nodes)


def case_mixed():
    """half the cycles are pure, half reference a live young object."""
    LIVE.clear()
    for i in range(N):
        a = A()
        b = B()
        a.other = b
        b.other = a
        if i % 2:
            shared = {}
            LIVE.append(shared)
            a.extra = shared


def case_long_chain():
    """one long garbage chain closed into a cycle (deep closure)."""
    for _ in range(10):
        first = prev = []
        for _ in range(N):
            cur = [prev]
            prev = cur
        first.append(prev)


CASES = [case_self_list, case_pair_dict, case_instances, case_instances_live_ref,
         case_exceptions, case_closures, case_live_tree, case_mixed, case_long_chain]


def fmt_gen(g, st):
    tg = st["true_garbage"]
    recall = (st["pure_objects"] / tg * 100) if tg else float("nan")
    blockers = sorted(st["blockers"].items(), key=lambda kv: -kv[1])[:4]
    missed = sorted(st["missed_types"].items(), key=lambda kv: -kv[1])[:4]
    return (f"  gen{g}: coll={st['collections']} cand={st['candidates']} closures={st['closures']} "
            f"pure={st['pure_closures']} pure_obj={st['pure_objects']} truth={tg} "
            f"recall={recall:.1f}% missed={st['missed_garbage']} "
            f"trav_old={st['traversals_old']} edges={st['edges']} "
            f"t_new={st['closure_time_ns']/1e6:.2f}ms t_old={st['deduce_time_ns']/1e6:.2f}ms "
            f"hw={st['queue_high_water']} phantom={st['phantom_closures']} pbl={st['pure_but_live']}\n"
            f"        blockers={blockers} missed_types={missed}")


def run(case, explicit):
    gc.collect()            # make classes/module state old
    gc._closure_reset_stats()
    LIVE.clear()
    t0 = time.perf_counter()
    case()
    if explicit:
        gc.collect(0)
    else:
        gc.collect()
    dt = time.perf_counter() - t0
    stats = gc._closure_stats()
    print(f"{case.__name__} ({'gen0' if explicit else 'full'}, {dt*1e3:.1f} ms): {case.__doc__}")
    if stats is None:
        print("  (no stats: set PYTHON_GC_CLOSURE_STATS or mode 4)")
        return
    for g, st in enumerate(stats):
        if st["collections"]:
            print(fmt_gen(g, st))
    LIVE.clear()
    gc.collect()


def main():
    mode = gc._closure_mode()
    print(f"closure mode {mode}; {sys.version}")
    names = sys.argv[1:]
    cases = [c for c in CASES if not names or c.__name__ in names or c.__name__[5:] in names]
    for case in cases:
        run(case, explicit=True)
        run(case, explicit=False)
    # automatic collections: many cycles with default thresholds
    gc._closure_reset_stats()
    t0 = time.perf_counter()
    for _ in range(5):
        case_instances()
        case_exceptions()
        case_mixed()
    dt = time.perf_counter() - t0
    print(f"automatic collections ({dt*1e3:.1f} ms)")
    stats = gc._closure_stats()
    if stats:
        for g, st in enumerate(stats):
            if st["collections"]:
                print(fmt_gen(g, st))


if __name__ == "__main__":
    main()
