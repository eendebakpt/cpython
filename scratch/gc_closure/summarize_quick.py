"""Summarise a quick campaign: per benchmark and config, wall time (pyperf
mean) and GC-phase time from the stats dump (mark + closure + deduce, summed
over all collections of all worker processes).

    python summarize_quick.py <results_dir> <prefix> config1 config2 ...
"""
import json
import os
import sys


def gc_phase_ms(path):
    tot = {"mark": 0, "closure": 0, "deduce": 0, "cand": 0, "coll": 0,
           "pure": 0, "truth": 0, "missed": 0, "marked": 0,
           "young": 0, "full": 0, "full_coll": 0}
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            rec = json.loads(line)
            for g, st in enumerate(rec["gens"]):
                t = st.get("mark_time_ns", 0) + st["closure_time_ns"] + st["deduce_time_ns"]
                if g == len(rec["gens"]) - 1:
                    tot["full"] += t
                    tot["full_coll"] += st["collections"]
                else:
                    tot["young"] += t
                tot["mark"] += st.get("mark_time_ns", 0)
                tot["closure"] += st["closure_time_ns"]
                tot["deduce"] += st["deduce_time_ns"]
                tot["cand"] += st["candidates"]
                tot["coll"] += st["collections"]
                tot["pure"] += st["pure_objects"]
                tot["truth"] += st["true_garbage"]
                tot["missed"] += st["missed_garbage"]
                tot["marked"] += st.get("marked", 0)
    return tot


def wall_ms(path):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        data = json.load(f)
    vals = []
    for bm in data["benchmarks"]:
        for run in bm.get("runs", []):
            vals.extend(run.get("values", []))
    if not vals:
        return None
    vals.sort()
    return 1e3 * vals[len(vals) // 2]


def main():
    rdir, prefix, *configs = sys.argv[1:]
    benches = sorted({f.split("_", 2)[2].rsplit(".", 1)[0]
                      for f in os.listdir(rdir)
                      if f.startswith(prefix + "_") and f.endswith(".json")
                      and not f.startswith("stats_")})
    print(f"{'benchmark':24} {'config':6} {'wall ms':>10} {'gc ms':>9} {'young':>8} {'full':>8} {'/fullc':>7} {'fcoll':>6} {'recall':>7}")
    for b in benches:
        base = None
        for c in configs:
            tag = f"{prefix}_{c}_{b}"
            w = wall_ms(os.path.join(rdir, tag + ".json"))
            g = gc_phase_ms(os.path.join(rdir, "stats_" + tag + ".jsonl"))
            if w is None or g is None:
                print(f"{b:24} {c:6} {'-':>10}")
                continue
            gc_total = (g["mark"] + g["closure"] + g["deduce"]) / 1e6
            young = g["young"] / 1e6
            full = g["full"] / 1e6
            per_full = full / g["full_coll"] if g["full_coll"] else 0.0
            rel = ""
            if base is None:
                base = (w, gc_total, young, per_full)
            else:
                ry = f"{young / base[2]:.2f}x" if base[2] else "-"
                rf = f"{per_full / base[3]:.2f}x" if base[3] else "-"
                rel = f" (wall {w / base[0]:.2f}x, young {ry}, per-full {rf})"
            recall = f"{g['pure'] / g['truth'] * 100:6.1f}%" if g["truth"] else "   n/a "
            print(f"{b:24} {c:6} {w:10.2f} {gc_total:9.2f} {young:8.1f} {full:8.1f} {per_full:7.3f} "
                  f"{g['full_coll']:6} {recall}{rel}")


if __name__ == "__main__":
    main()
