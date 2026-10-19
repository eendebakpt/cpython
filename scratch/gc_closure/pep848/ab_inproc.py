"""In-process A/B of the closure pass on the PEP 848 branch.

The detector is switched per collection through the gc.set_debug() bit
DEBUG_CLASSIC (1<<7): consecutive young collections alternate between the
classic algorithm and the closure pass, so both see the same workload
phases and the same machine load.  Time per collection comes from
gc.callbacks (perf_counter around each collection), candidates from
gc.get_stats() deltas.  Old-generation increments are classic in both
modes and act as the noise control.

    python ab_inproc.py <benchmark> [seconds] [out.json]

Run with the pyperformance venv python of the branch (it has the benchmark
dependencies).
"""
import asyncio
import builtins
import gc
import importlib.util
import json
import os
import random
import sys
import time

BM = r"C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\Lib\site-packages\pyperformance\data-files\benchmarks"
CLASSIC = 1 << 7
TOGGLE_GENS = (0,) if hasattr(gc, "get_implementation") else (0, 1)


def load(name):
    path = os.path.join(BM, "bm_" + name, "run_benchmark.py")
    spec = importlib.util.spec_from_file_location("bm_" + name, path)
    mod = importlib.util.module_from_spec(spec)
    mod.__builtins__ = builtins        # bm_sphinx assigns __builtins__.open
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def wl_deltablue():
    mod = load("deltablue")
    return lambda: mod.delta_blue(20000)


def wl_richards():
    mod = load("richards")
    r = mod.Richards()
    return lambda: r.run(2)


def wl_raytrace():
    mod = load("raytrace")
    return lambda: mod.bench_raytrace(1, mod.DEFAULT_WIDTH, mod.DEFAULT_HEIGHT, None)


def wl_mdp():
    mod = load("mdp")
    return lambda: mod.bench_mdp(1)


def wl_networkx():
    mod = load("networkx")
    def run():
        mod.bench_shortest_path()
        mod.bench_connected_components()
        mod.bench_k_core()
    return run


def wl_sympy():
    mod = load("sympy")
    def run():
        for name in mod.BENCHMARKS:
            mod.clear_cache()
            getattr(mod, "bench_" + name)()
    return run


def wl_docutils():
    mod = load("docutils")
    return lambda: mod.bench_docutils(1, mod.DOC_ROOT)


def wl_xml_etree():
    mod = load("xml_etree")
    import xml.etree.ElementTree as etree
    def run():
        for fn in (mod.bench_parse, mod.bench_iterparse, mod.bench_process, mod.bench_generate):
            mod.bench_etree(2, etree, fn)
    return run


def wl_async_tree():
    mod = load("async_tree")
    tree = mod.MemoizationAsyncTree()
    return lambda: asyncio.run(tree.run())


def wl_sphinx():
    mod = load("sphinx")
    return lambda: mod.bench_sphinx(1, mod.DOC_ROOT)


WORKLOADS = {
    "deltablue": wl_deltablue, "richards": wl_richards, "raytrace": wl_raytrace,
    "mdp": wl_mdp, "networkx": wl_networkx, "sympy": wl_sympy,
    "docutils": wl_docutils, "xml_etree": wl_xml_etree,
    "async_tree_memoization": wl_async_tree, "sphinx": wl_sphinx,
}


class Meter:
    def __init__(self):
        self.active = False
        self.rng = random.Random(12345)
        self.pending = []      # balanced, non-periodic mode sequence
        self.t0 = 0.0
        self.cand0 = 0
        # [mode][generation] -> dict
        self.acc = {m: [dict(time=0.0, n=0, collected=0, candidates=0) for _ in range(3)]
                    for m in ("classic", "closure")}

    def mode(self):
        return "classic" if gc.get_debug() & CLASSIC else "closure"

    def __call__(self, phase, info):
        gen = info["generation"]
        if phase == "start":
            self.cand0 = gc.get_stats()[gen]["candidates"]
            self.t0 = time.perf_counter()
            return
        dt = time.perf_counter() - self.t0
        if self.active:
            a = self.acc[self.mode()][gen]
            a["time"] += dt
            a["n"] += 1
            a["collected"] += info["collected"]
            a["candidates"] += gc.get_stats()[gen]["candidates"] - self.cand0
        # Alternate the detector after every young collection.  Under PEP 848
        # automatic collections strictly alternate young/old, so toggling
        # after every collection would pin each kind to one mode; there only
        # generation 0 is young.  On main generations 0 and 1 are young.
        if gen in TOGGLE_GENS:
            if not self.pending:
                self.pending = [0, CLASSIC]
                self.rng.shuffle(self.pending)
            gc.set_debug((gc.get_debug() & ~CLASSIC) | self.pending.pop())


def main():
    name = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    out = sys.argv[3] if len(sys.argv) > 3 else None
    run = WORKLOADS[name]()
    gc.enable()
    gc.set_debug(gc.get_debug() & ~CLASSIC)
    meter = Meter()
    gc.callbacks.append(meter)
    run()                      # warm-up, not accounted
    meter.active = True
    t0 = time.perf_counter()
    rounds = 0
    while rounds < 4 or time.perf_counter() - t0 < seconds:
        run()
        rounds += 1
    meter.active = False
    gc.callbacks.remove(meter)
    impl = gc.get_implementation() if hasattr(gc, "get_implementation") else "main"
    rec = {"benchmark": name, "rounds": rounds, "impl": impl,
           "wall": time.perf_counter() - t0, "acc": meter.acc}
    print(json.dumps(rec))
    if out:
        with open(out, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")
    cl, cs = meter.acc["classic"], meter.acc["closure"]
    labels = ("young", "old-inc", "full") if impl != "main" else ("gen0", "gen1", "full")
    for g, label in enumerate(labels):
        if cl[g]["n"] and cs[g]["n"]:
            print(f"{name} {label}: closure/classic time {cs[g]['time'] / cl[g]['time']:.3f}x "
                  f"(per candidate {(cs[g]['time'] / max(cs[g]['candidates'], 1)) / (cl[g]['time'] / max(cl[g]['candidates'], 1)):.3f}x); "
                  f"collections {cl[g]['n']}/{cs[g]['n']}, collected {cl[g]['collected']}/{cs[g]['collected']}, "
                  f"candidates {cl[g]['candidates']}/{cs[g]['candidates']}")


if __name__ == "__main__":
    main()
