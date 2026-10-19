"""Concrete examples of live objects that hide garbage from the closure pass.

Run with PYTHON_GC_CLOSURE=4 PYTHON_GC_MARK=1 PYTHON_GC_DAG=1:

    python blockers.py xml_etree | async_tree

Runs a slice of the benchmark, does a full collection, then prints the
sampled blockers (live members of unknown records that contain garbage)
with their referrers, so the missing roots can be identified.
"""
import gc
import importlib.util
import os
import sys
import types

BM = r"C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\Lib\site-packages\pyperformance\data-files\benchmarks"


def load(name):
    path = os.path.join(BM, "bm_" + name, "run_benchmark.py")
    spec = importlib.util.spec_from_file_location("bm_" + name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def run_xml_etree():
    mod = load("xml_etree")
    import xml.etree.ElementTree as etree
    for fn in (mod.bench_parse, mod.bench_iterparse, mod.bench_process, mod.bench_generate):
        mod.bench_etree(2, etree, fn)


def run_async_tree():
    mod = load("async_tree")
    import asyncio
    tree = mod.MemoizationAsyncTree()
    for _ in range(2):
        asyncio.run(tree.run())


def describe(obj):
    t = type(obj)
    if isinstance(obj, type):
        return f"type {obj.__module__}.{obj.__qualname__}"
    if isinstance(obj, types.ModuleType):
        return f"module {obj.__name__}"
    if isinstance(obj, types.FunctionType):
        return f"function {obj.__module__}.{obj.__qualname__}"
    if isinstance(obj, dict):
        keys = list(obj)[:4]
        return f"dict({len(obj)}) keys={keys!r:.80}"
    if isinstance(obj, (list, tuple, set)):
        return f"{t.__name__}({len(obj)})"
    r = repr(obj)
    return f"{t.__module__}.{t.__qualname__} {r[:60]}"


def owner_of_dict(d, referrers):
    for r in referrers:
        try:
            if getattr(r, "__dict__", None) is d:
                return r
        except Exception:
            pass
    return None


def main():
    which = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    run = {"xml_etree": run_xml_etree, "async_tree": run_async_tree}[which]
    for _ in range(rounds):
        run()
        gc.collect()             # full collection in truth mode
    sample = gc._closure_blockers()
    print(f"{which}: {len(sample)} sampled blockers "
          f"(mode {gc._closure_mode()})")
    seen = set()
    for blocker, gtype, size, garbage in sample:
        key = (type(blocker).__name__, describe(blocker)[:50])
        if key in seen:
            continue
        seen.add(key)
        refs = [r for r in gc.get_referrers(blocker) if r is not sample]
        print(f"\n== blocker: {describe(blocker)}")
        print(f"   record size {size}, garbage in it {garbage}, e.g. a {gtype}")
        if isinstance(blocker, dict):
            owner = owner_of_dict(blocker, refs)
            if owner is not None:
                print(f"   __dict__ of: {describe(owner)}")
                refs = [r for r in gc.get_referrers(owner)]
                print(f"   owner referrers: {[describe(r)[:70] for r in refs[:5]]}")
                continue
        print(f"   referrers ({len(refs)}): {[describe(r)[:70] for r in refs[:6]]}")


if __name__ == "__main__":
    main()
