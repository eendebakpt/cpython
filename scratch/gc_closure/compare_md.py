"""Markdown table from pyperf suites: baseline, candidate, baseline control.

    python compare_md.py base.json cand.json control.json [cand2.json]

Prints per benchmark: baseline mean, candidate/baseline, control/baseline
(noise check), and flags rows whose control deviates by more than 5%.
"""
import json
import statistics
import sys


def load(path):
    """A pyperf suite file, or a glob of per-benchmark suite files."""
    import glob
    out = {}
    for p in (glob.glob(path) if "*" in path else [path]):
        with open(p) as f:
            data = json.load(f)
        for bm in data["benchmarks"]:
            name = bm.get("metadata", {}).get("name") or data["metadata"].get("name")
            vals = [v for run in bm.get("runs", []) for v in run.get("values", [])]
            if vals:
                out[name] = statistics.mean(vals)
    return out


def fmt(x):
    if x < 1e-3:
        return f"{x * 1e6:.1f} us"
    if x < 1:
        return f"{x * 1e3:.2f} ms"
    return f"{x:.3f} s"


def main():
    base = load(sys.argv[1])
    cand = load(sys.argv[2])
    ctrl = load(sys.argv[3]) if len(sys.argv) > 3 else {}
    cand2 = load(sys.argv[4]) if len(sys.argv) > 4 else {}
    print("| benchmark | classic | closure / classic | control / classic |"
          + (" closure 2 / classic |" if cand2 else ""))
    print("|---|---|---|---|" + ("---|" if cand2 else ""))
    ratios = []
    for name in sorted(base):
        if name not in cand:
            continue
        r = cand[name] / base[name]
        c = ctrl[name] / base[name] if name in ctrl else float("nan")
        flag = " (noisy)" if abs(c - 1) > 0.05 else ""
        row = f"| {name} | {fmt(base[name])} | {r:.2f}x | {c:.2f}x{flag} |"
        if cand2 and name in cand2:
            row += f" {cand2[name] / base[name]:.2f}x |"
        print(row)
        if abs(c - 1) <= 0.05:
            ratios.append(r)
    if ratios:
        gm = statistics.geometric_mean(ratios)
        print(f"\nGeometric mean over {len(ratios)} benchmarks with a clean control: {gm:.3f}x")


if __name__ == "__main__":
    main()
