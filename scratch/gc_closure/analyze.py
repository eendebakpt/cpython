"""Aggregate PYTHON_GC_CLOSURE_STATS dumps (one JSON object per line/process).

    python analyze.py stats_a.jsonl [stats_b.jsonl ...]
"""
import json
import sys
from collections import Counter

HIST = 24


def load(path):
    gens = [Counter() for _ in range(3)]
    hists = [[0] * HIST for _ in range(3)]
    phists = [[0] * HIST for _ in range(3)]
    blockers = [Counter() for _ in range(3)]
    missed = [Counter() for _ in range(3)]
    modes = Counter()
    nproc = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            nproc += 1
            modes[rec["mode"]] += 1
            for g, st in enumerate(rec["gens"]):
                for k, v in st.items():
                    if isinstance(v, (int, float)):
                        gens[g][k] += v
                for i, v in enumerate(st["size_hist"]):
                    hists[g][i] += v
                for i, v in enumerate(st["pure_size_hist"]):
                    phists[g][i] += v
                blockers[g].update(st["blockers"])
                missed[g].update(st["missed_types"])
    return nproc, modes, gens, hists, phists, blockers, missed


def pct(a, b):
    return f"{a / b * 100:6.1f}%" if b else "   n/a "


def hist_summary(h):
    total = sum(h)
    if not total:
        return "-"
    acc = 0
    p50 = p99 = None
    for i, v in enumerate(h):
        acc += v
        if p50 is None and acc >= total * 0.5:
            p50 = i
        if p99 is None and acc >= total * 0.99:
            p99 = i
    mx = max(i for i, v in enumerate(h) if v)
    return f"p50<2^{p50 + 1} p99<2^{p99 + 1} max<2^{mx + 1}"


def report(path):
    nproc, modes, gens, hists, phists, blockers, missed = load(path)
    print(f"== {path}: {nproc} processes, modes {dict(modes)}")
    print(f"{'gen':>3} {'coll':>6} {'new':>6} {'old':>6} {'truth':>6} {'cand':>10} {'closures':>9} "
          f"{'pure':>8} {'pure_obj':>9} {'truth_g':>9} {'recall':>7} {'missed':>8} {'imp_g':>7} "
          f"{'edges':>10} {'trav_old':>10} {'t_new ms':>9} {'t_old ms':>9} {'hw':>8} {'fb':>3}")
    for g in range(3):
        s = gens[g]
        if not s["collections"]:
            continue
        print(f"{g:>3} {s['collections']:>6} {s['closure_collections']:>6} "
              f"{s['collections'] - s['closure_collections'] + s['truth_collections']:>6} "
              f"{s['truth_collections']:>6} {s['candidates']:>10} {s['closures']:>9} "
              f"{s['pure_closures']:>8} {s['pure_objects']:>9} {s['true_garbage']:>9} "
              f"{pct(s['pure_objects'], s['true_garbage'])} {s['missed_garbage']:>8} "
              f"{s['unknown_with_garbage']:>7} {s['edges']:>10} {s['traversals_old']:>10} "
              f"{s['closure_time_ns'] / 1e6:>9.2f} {s['deduce_time_ns'] / 1e6:>9.2f} "
              f"{s['queue_high_water']:>8} {s['fallbacks']:>3}")
        if s.get("marked") or s.get("mark_time_ns"):
            print(f"     mark-alive: marked={s.get('marked', 0)} "
                  f"t_mark={s.get('mark_time_ns', 0) / 1e6:.2f} ms")
        if s["phantom_closures"] or s["pure_but_live"] or s["log_oom"]:
            print(f"     !! phantom={s['phantom_closures']} pure_but_live={s['pure_but_live']} "
                  f"log_oom={s['log_oom']}")
        print(f"     closure sizes: {hist_summary(hists[g])}; pure: {hist_summary(phists[g])}")
        if blockers[g]:
            print(f"     blockers (live members of closures that hide garbage): "
                  f"{blockers[g].most_common(8)}")
        if missed[g]:
            print(f"     missed garbage types: {missed[g].most_common(8)}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(p)
