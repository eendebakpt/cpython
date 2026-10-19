"""Markdown table from the in-process A/B records (ab_inproc.py).

    python summarize_inproc.py results_pep848/inproc*.jsonl
"""
import glob
import json
import sys

recs = []
for pat in sys.argv[1:]:
    for p in glob.glob(pat):
        with open(p, encoding="utf-8") as f:
            recs += [json.loads(line) for line in f if line.strip()]


def r(a, b):
    return f"{a / b:.2f}x" if b else "n/a"


print("| benchmark | young collections (classic/closure) | candidates per young collection | young GC time closure/classic | per candidate | control (old increments / full collections) closure/classic | young share of GC time (classic) |")
print("|---|---|---|---|---|---|---|")
for rec in sorted(recs, key=lambda x: x["benchmark"]):
    cl, cs = rec["acc"]["classic"], rec["acc"]["closure"]
    main = rec.get("impl") == "main"
    if main:
        # main-based branch: generations 0 and 1 are young, full is the control
        def merge(a, b):
            return {k: a[k] + b[k] for k in a}
        y_cl, y_cs = merge(cl[0], cl[1]), merge(cs[0], cs[1])
        o_cl, o_cs = cl[2], cs[2]
    else:
        y_cl, y_cs = cl[0], cs[0]
        o_cl, o_cs = cl[1], cs[1]
    if not y_cl["n"] or not y_cs["n"]:
        print(f"| {rec['benchmark']} | {y_cl['n']}/{y_cs['n']} | - | no young collections | - | - | - |")
        continue
    cand = (y_cl["candidates"] + y_cs["candidates"]) / (y_cl["n"] + y_cs["n"])
    per_cand = (y_cs["time"] / max(y_cs["candidates"], 1)) / (y_cl["time"] / max(y_cl["candidates"], 1))
    tot_cl = sum(g["time"] for g in cl)
    share = y_cl["time"] / tot_cl if tot_cl else 0
    ctrl = r(o_cs["time"], o_cl["time"]) if (o_cl["n"] and o_cs["n"]) else "-"
    print(f"| {rec['benchmark']} | {y_cl['n']}/{y_cs['n']} | {cand:,.0f} | {r(y_cs['time'], y_cl['time'])} | {per_cand:.2f}x | {ctrl} | {share:.0%} |")
