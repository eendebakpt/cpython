---
name: project-incremental-gc-review
description: "Review of markshannon/incremental-gc (2026-09-25) — worktrees, probes, confirmed findings"
metadata:
  node_type: memory
  type: project
  originSessionId: f6640bdf-cef1-4751-adc3-49ce97cfb4e4
  modified: 2026-09-25T07:23:22.179Z
---

Reviewed Mark Shannon's `incremental-gc` branch (single commit 91b7749df68,
"Reference implementation for new generational, incremental GC", merge-base
a6ad4482377) on 2026-09-25.

Setup: worktrees `C:\develop\cpython-incgc` (branch) and
`C:\develop\cpython-incgc-base` (merge-base), Release + Debug built with
`scratch\incgc_build.ps1 -Dir <wt> [-Config Debug]` (builds _freeze_module
first — fresh worktrees fail otherwise). Probes/bench in `scratch\incgc_probe*.py`,
`scratch\incgc_bench.py`, `scratch\incgc_bench_run.py` (per-process interleaved).

Confirmed empirically:
- `gc.collect(0)` needs aging_spaces+1 (=6) calls to reclaim a fresh cycle.
- Automatic gen-1 increments are no-ops when `old_work <= 0` but still fire
  callbacks/stats (4 of 4 in a 300k-list allocation loop).
- Phase asymmetry: young objects (tag 0) reachable from sys get tenured by
  old-gen passes only when `visited_space == 1` (alternating passes).
- `gc.get_count()` int overflow for threshold0=1<<30 (gcmodule lacks cast).
- threshold2 unused in incremental mode; docs say (1000,10,10)/512-byte unit,
  code is (2000,10,10)/1024.
- Old increment pause is unbounded (old_work ∝ survivors): 77 ms on a 1M-object heap.
- Perf: incremental ~0.7–1.05x of base on GC micro-benches; legacy mode on the
  branch is also faster than base because the trigger is byte-based.

**How to apply:** if the branch is revisited (PR), re-check these against the
new version before repeating the analysis; reuse the worktrees/probes.
