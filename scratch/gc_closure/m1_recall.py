"""Recall per generation, with and without mark-alive, from the m1stats files."""
import json
import os
import sys

rdir = sys.argv[1]
benches = sys.argv[2:]


def load(path):
    gens = [dict(pure=0, truth=0, marked=0, cand=0, coll=0, ct=0, dt=0, mt=0) for _ in range(3)]
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            if line.strip():
                for g, st in enumerate(json.loads(line)["gens"]):
                    gens[g]["pure"] += st["pure_objects"]
                    gens[g]["truth"] += st["true_garbage"]
                    gens[g]["marked"] += st.get("marked", 0)
                    gens[g]["cand"] += st["candidates"]
                    gens[g]["coll"] += st["collections"]
                    gens[g]["ct"] += st["closure_time_ns"]
                    gens[g]["dt"] += st["deduce_time_ns"]
                    gens[g]["mt"] += st.get("mark_time_ns", 0)
    return gens


def pct(a, b):
    return f"{a / b * 100:5.1f}%" if b else "  n/a "


print(f"{'benchmark':24} gen {'recall nomark':>14} {'recall mark':>12} {'marked/cand':>12} {'t_mark':>8} {'t_clos':>8} {'t_ded':>8}  (ms, mark run)")
for b in benches:
    nm = load(os.path.join(rdir, f"stats_m1stats_nomark_{b}.jsonl"))
    mk = load(os.path.join(rdir, f"stats_m1stats_mark_{b}.jsonl"))
    if nm is None or mk is None:
        print(f"{b:24} missing")
        continue
    for g in range(3):
        if not nm[g]["coll"] and not mk[g]["coll"]:
            continue
        print(f"{b:24} {g:3} {pct(nm[g]['pure'], nm[g]['truth']):>14} {pct(mk[g]['pure'], mk[g]['truth']):>12} "
              f"{pct(mk[g]['marked'], mk[g]['cand'] + mk[g]['marked']):>12} {mk[g]['mt'] / 1e6:8.1f} {mk[g]['ct'] / 1e6:8.1f} {mk[g]['dt'] / 1e6:8.1f}")
