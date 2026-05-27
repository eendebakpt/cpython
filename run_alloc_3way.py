"""Driver for an N-way comparison of allocator-init changes.

Configurations to measure (set via CONFIGS list below):

  1. baseline    -- branch `main`. Unmodified.

  2. memset_init -- branch `feature/alloc-init-memset`. Single-line change in
                    `Objects/dictobject.c`: replace the for-loop that zeros
                    `values->values[0..size-1]` with `memset(values->values,
                    0, size * sizeof(PyObject *))`. Same logical work; the
                    compiler can lower memset to vector stores. Minimal diff
                    (one line, no header changes); does NOT touch typeobject.c.

                    Note: A related "just drop the loop entirely" variant was
                    tried and turned out to be INCORRECT -- `_PyObject_GC_New`
                    in Python/gc.c is a second alloc path that does NOT memset
                    the body, so the values must be explicitly zeroed by
                    `_PyObject_InitInlineValues`.

  3. helper      -- branch `feature/alloc-fast-path-helper`.
                    A4 (skip-memset) + static-inline helper
                    `alloc_inline_values_instance()`. Fast path has no
                    Py_TPFLAGS_INLINE_VALUES re-checks at all.

  4. reduced     -- branch `feature/alloc-fast-path`.
                    A4 + single function with one consolidated
                    `if (INLINE_VALUES)` at the top. Smaller diff than helper.

  5. split_two   -- branch `feature/alloc-split-two-fns`.
                    Both code paths factored into named static helpers:
                    `alloc_inline_values_instance(type)` and
                    `alloc_non_inline_values_instance(type, nitems)`. The
                    top-level `_PyType_AllocNoTrack` is a 4-line dispatcher.
                    Symmetric structure; same asserts as `helper` /
                    `reduced` on the fast path; each helper has just one
                    caller so the compiler should inline both back.

`memset_init` touches a different file than the typeobject.c variants and
can combine with any of them. A "combined" config could be added later.

The runner:
  1. Records starting branch + HEAD so it can restore at the end.
  2. For each config: git checkout the branch, rebuild with msbuild, run
     bench_alloc_isolated.py three times (subprocess isolation already
     applied per case), pick the min across the three full-bench repeats.
  3. Prints a side-by-side comparison table.

Run from the repo root:
    python run_alloc_3way.py

Stop and resume cleanly: if interrupted between configs, the working tree
may have a stale typeobject.c -- run `git checkout <branch> --` to restore.

Notes for the operator:
  - Run on a system that is otherwise idle. Background activity has shown
    2x+ variance on this benchmark.
  - The 3rd repeat per config is a tiebreaker; if numbers are stable across
    repeats, the fewest-noise repeat is the min-of-3 reported.
  - If `helper` measures faster than `reduced` by a wide margin (more than
    a few %), that suggests the compiler isn't inlining the helper back
    into _PyType_AllocNoTrack on this toolchain, and we should keep the
    helper version. Otherwise prefer `reduced` for the smaller diff.
"""

import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent

# --- Configurations -----------------------------------------------------------

# (label, branch_name, short_description)
CONFIGS = [
    ("baseline", "main",
     "main / unmodified"),
    ("memset_init", "feature/alloc-init-memset",
     "1-line change: for-loop -> memset in _PyObject_InitInlineValues"),
    ("helper", "feature/alloc-fast-path-helper",
     "A4 + static-inline helper; no INLINE_VALUES re-checks"),
    ("reduced", "feature/alloc-fast-path",
     "A4 + single function; one INLINE_VALUES branch at top"),
    ("split_two", "feature/alloc-split-two-fns",
     "two named static helpers (inline-values + non-inline-values); thin dispatcher"),
]

REPEATS_PER_CONFIG = 3
CASES = ["PlainEmpty", "PlainInit", "DC", "SlotsCls", "TupleSub"]

# --- Platform-specific build --------------------------------------------------

PYTHON_EXE = REPO / "PCbuild" / "amd64" / "python.exe"

MSBUILD_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\msbuild.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
]


def find_msbuild():
    for p in MSBUILD_CANDIDATES:
        if os.path.exists(p):
            return p
    # Fall back to PATH
    return "msbuild.exe"


def run(cmd, **kw):
    """Run a subprocess, raising on failure."""
    print(f"  $ {' '.join(map(str, cmd)) if isinstance(cmd, (list, tuple)) else cmd}")
    return subprocess.run(cmd, check=True, **kw)


def current_branch():
    out = subprocess.run(["git", "branch", "--show-current"],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


def have_clean_tree():
    out = subprocess.run(["git", "status", "--porcelain"],
                         capture_output=True, text=True, check=True)
    # Allow untracked files (??); reject modified tracked files (M, A, D, R, C, U).
    for line in out.stdout.splitlines():
        if line[:2].strip() and not line.startswith("??"):
            return False, line
    return True, None


def checkout(branch):
    print(f"\n=== Checking out {branch} ===")
    run(["git", "checkout", branch])


def build():
    print(f"  Rebuilding...")
    mb = find_msbuild()
    run([mb, "PCbuild\\pcbuild.proj",
         "/t:Build",
         "/p:Configuration=Release",
         "/p:Platform=x64",
         f"/p:PythonForBuild={PYTHON_EXE}",
         "/p:UseTIER2=4",
         "/v:m", "/nologo", "/m"],
        stdout=subprocess.DEVNULL)


def bench_one():
    """Run bench_alloc_isolated.py and parse its table back to a dict."""
    # bench_alloc_isolated.py prints the table to stdout
    out = subprocess.run(
        [str(PYTHON_EXE), "bench_alloc_isolated.py"],
        capture_output=True, text=True, check=True,
    )
    results = {}
    for line in out.stdout.splitlines():
        # Parse lines of the form "  CaseName     mn  md  mean"
        parts = line.split()
        if len(parts) == 4 and parts[0] in CASES:
            results[parts[0]] = {
                "min": float(parts[1]),
                "median": float(parts[2]),
                "mean": float(parts[3]),
            }
    return results


def best_of_n(repeats):
    """Across N full benchmark repeats, take the min for each case."""
    best = {}
    for case in CASES:
        best[case] = {
            "min": min(r[case]["min"] for r in repeats),
            "median": statistics.median([r[case]["median"] for r in repeats]),
        }
    return best


def main():
    if not PYTHON_EXE.exists():
        print(f"FATAL: {PYTHON_EXE} does not exist. Build CPython first.",
              file=sys.stderr)
        sys.exit(2)

    ok, line = have_clean_tree()
    if not ok:
        print(f"FATAL: working tree is not clean ({line!r}).", file=sys.stderr)
        print("Commit or stash any modifications before running this script.",
              file=sys.stderr)
        sys.exit(2)

    starting_branch = current_branch()
    print(f"Starting on branch: {starting_branch}")

    results = {}
    try:
        for label, branch, desc in CONFIGS:
            checkout(branch)
            build()
            print(f"  Benchmarking ({REPEATS_PER_CONFIG} repeats)...")
            repeats = []
            for i in range(REPEATS_PER_CONFIG):
                print(f"    repeat {i+1}/{REPEATS_PER_CONFIG}")
                repeats.append(bench_one())
            results[label] = best_of_n(repeats)
            results[f"_{label}_desc"] = desc
            results[f"_{label}_raw"] = repeats
    finally:
        print(f"\nRestoring branch: {starting_branch}")
        run(["git", "checkout", starting_branch])
        build()

    # Save raw JSON for offline analysis
    out_path = REPO / "alloc_results.json"
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nRaw results: {out_path}")

    # Print comparison
    print()
    print("=" * 100)
    print(f"ALLOC {len(CONFIGS)}-WAY COMPARISON  (ns/instance, lower is better)")
    print("=" * 100)
    for label, branch, desc in CONFIGS:
        print(f"  {label:13s}  {branch:40s}  -- {desc}")
    print()

    # Header row
    header = f"  {'case':12s}"
    for label, _, _ in CONFIGS:
        header += f" {label:>13s}"
    for label, _, _ in CONFIGS[1:]:  # delta columns vs baseline
        header += f" {label[:6]+' v b':>11s}"
    print(header)
    print("  " + "-" * (len(header) - 2))

    # Body rows
    for case in CASES:
        row = f"  {case:12s}"
        baseline_val = results["baseline"][case]["min"]
        for label, _, _ in CONFIGS:
            row += f" {results[label][case]['min']:>12.1f}"
        for label, _, _ in CONFIGS[1:]:
            v = results[label][case]["min"]
            pct = 100 * (v - baseline_val) / baseline_val
            row += f" {pct:>+10.1f}%"
        print(row)
    print()
    print("Negative percentage = faster than baseline.")
    print("Numbers reported are min-of-min across "
          f"{REPEATS_PER_CONFIG} full bench repeats, each repeat using "
          f"subprocess isolation per case.")


if __name__ == "__main__":
    main()
