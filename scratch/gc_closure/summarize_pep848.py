"""Summarize the PEP 848 A/B: wall time from pyperf suites, GC time per
generation from the gc.get_stats() dumps of the workers.

    python summarize_pep848.py [results_dir] [prefix]

Columns: classic wall (C0a), closure/classic wall (A, Ab), control/classic
wall (C0b); young-GC time closure/classic (sum over workers of
gc.get_stats()[0]['duration']); old (increments) and full likewise; and the
share of garbage found by young collections.
"""
import glob
import json
import os
import statistics
import sys

res = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "results_pep848")
prefix = sys.argv[2] if len(sys.argv) > 2 else "pep"


def wall(tag):
    p = os.path.join(res, f"{tag}.json")
    if not os.path.exists(p):
        return None
    with open(p) as f:
        data = json.load(f)
    vals = [v for bm in data["benchmarks"] for run in bm.get("runs", []) for v in run.get("values", [])]
    return statistics.mean(vals) if vals else None


def gcstats(tag):
    p = os.path.join(res, f"{tag}.stats.jsonl")
    if not os.path.exists(p):
        return None
    tot = [dict(duration=0.0, collected=0, collections=0, candidates=0, max_pause=0.0) for _ in range(3)]
    n = 0
    with open(p) as f:
        for line in f:
            rec = json.loads(line)
            if not rec.get("worker"):
                continue
            n += 1
            for g, st in enumerate(rec["stats"]):
                for k in ("duration", "collected", "collections", "candidates"):
                    tot[g][k] += st.get(k, 0)
                tot[g]["max_pause"] = max(tot[g]["max_pause"], st.get("max_pause", 0.0))
    return tot if n else None


def ratio(a, b):
    if a is None or b is None or not b:
        return "n/a"
    return f"{a / b:.2f}x"


benches = sorted({os.path.basename(p)[len(prefix) + 1:].split("_", 1)[1][:-5]
                  for p in glob.glob(os.path.join(res, f"{prefix}_C0a_*.json"))})
print("| benchmark | classic wall | closure/classic | control/classic | closure2/classic | young GC | old GC | full GC | young share of garbage: classic -> closure |")
print("|---|---|---|---|---|---|---|---|---|")
for b in benches:
    w = {c: wall(f"{prefix}_{c}_{b}") for c in ("C0a", "A", "C0b", "Ab")}
    s = {c: gcstats(f"{prefix}_{c}_{b}") for c in ("C0a", "A", "C0b", "Ab")}
    base = w["C0a"]
    row = [b, f"{base * 1e3:.1f} ms" if base else "n/a",
           ratio(w["A"], base), ratio(w["C0b"], base), ratio(w["Ab"], base)]
    if s["C0a"] and s["A"]:
        for g in range(3):
            row.append(ratio(s["A"][g]["duration"], s["C0a"][g]["duration"]))
        def share(t):
            tot = sum(t[g]["collected"] for g in range(3))
            return f"{t[0]['collected'] / tot:.0%}" if tot else "-"
        row.append(f"{share(s['C0a'])} -> {share(s['A'])}")
    else:
        row += ["n/a"] * 4
    print("| " + " | ".join(row) + " |")

# control consistency on GC time
print()
for b in benches:
    a, c = gcstats(f"{prefix}_C0a_{b}"), gcstats(f"{prefix}_C0b_{b}")
    if a and c:
        print(f"{b}: young GC control/classic {ratio(c[0]['duration'], a[0]['duration'])}, "
              f"young collections {a[0]['collections']}, old {a[1]['collections']}, full {a[2]['collections']}")
