---
name: feedback-benchmarking
description: Benchmark rigor expectations for performance work on this CPython checkout
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d2f13ae3-bc99-4fa8-b657-4fbd510b070c
---

Performance work on CPython must be benchmarked carefully. Microbenchmarks on
this Windows desktop machine are noisy: same-binary back-to-back runs can show
2x variance on instance-allocation benchmarks because of background activity,
GC scheduling between test cases, or thermal/frequency state. Sequential
cases in a single process pollute each other.

**Why:** During the alloc-fast-path work I reported a 2-3x regression on DC /
SlotsCls / TupleSub vs main that turned out to be measurement noise -- run 3
of the same binary showed the same numbers as main. Wasted time, almost
landed the wrong conclusion.

**Use pyperf.** It is the default benchmarking tool for this project -- CPython
PRs are expected to carry pyperf numbers, and reviewers read them. Write
benchmarks with `pyperf.Runner()` and `runner.timeit(name, stmt, setup=...)`.
Prefer `timeit` over `bench_func()` with a small `inner_loops`: timeit lets
pyperf calibrate the loop count per case, and fixed small inner_loops produced
self-contradictory results here (it once reported speedups on code paths the
patch could not reach).

KEEP PYPERF RUNS SMALL. Default settings (20 processes) across a dozen cases
take hours on this machine. `-p 5 -w 1 -n 5` over 3 interleaved rounds is
plenty for effects above the noise floor. Interleave the two binaries round by
round with `--append` into one JSON per binary, then `pyperf compare_to`.

ALWAYS INCLUDE CONTROL CASES -- operations the change provably cannot reach
(e.g. for an x_mul() change: 1-digit x 1-digit, which long_mul() answers via the
compact fast path; squaring, which takes a separate path; long_add). They give a
measured noise floor instead of a guessed one, and they catch bad runs: a
control reading 0.86x or 1.40x means the run is junk, not that the patch worked.

NEVER COMPARE TWO FEATURE BRANCHES DIRECTLY. Two different builds differ by
~1.09x on identical code purely from layout/alignment, which contaminated a
whole run and made a good change look like a 5-19% regression. Always compare
each branch to a common `main` baseline built from the same worktree/toolchain.

**How to apply:**
- For perf work, always benchmark against `main` (not against intermediate
  branches) so the user can reproduce the headline number.
- Run each case in a fresh subprocess if it's reasonable -- no cross-case
  state pollution.
- Disable GC during measurement loops (gc.disable()) and gc.collect() once
  before measuring.
- Use min-of-N, not mean, when comparing micro-allocations.
- If you see a surprising regression, RE-RUN before drawing conclusions.
- Final benchmarks should be run on a quiet system. The user is willing to do
  this themselves -- it's reasonable to commit the work and benchmark scripts
  on a branch and hand off measurement.
- CONFIRMED 2026-06: `pyperf system tune` is a no-op on Windows ("no operation
  available for your platform") -- cannot reduce jitter that way. On a ~100ns
  pyperf microbench, two back-to-back runs of the SAME binary differed by a
  1.37x geomean (e.g. 100/103/125 ns -> 131/159/159 ns). So a single A-then-B
  comparison showing "1.5x faster" is INSIDE the noise floor and worthless.
  To extract signal despite drift, interleave both binaries (need two built
  pythons available simultaneously; copy `PCbuild/amd64` aside before rebuilding
  the other, then alternate runs with `pyperf ... --append`). Even then, prefer
  to hand the final number to the user on a quiet box.

See [[probes-saved-in-misc]] for the probe scripts already written.

**CONFIRMED 2026-10-05 (stronger):** even with ONE binary (runtime flag, so zero
layout difference), process-level pyperf interleaving (3 rounds x `-p 5 -n 5`,
high priority) gave controls of 1.07-1.12x on this box. Process-level
interleaving cannot resolve <10% effects here, full stop. The only method that
produced 1.00 controls is the in-process ABBA harness (`scratch/bench_deadwork_single.py`
template): one fresh process per case, flag toggled every ~15 ms. Still use
pyperf for statistics/tables: the harness dumps pyperf JSON, format with
`scratch/mk_pyperf_table.py` (pyperf significance test, no band column). The
user wants case names that read like code, e.g. `dct.update(dct_5)`, `ctl: a + b`.
User (2026-10-05): keep benchmark rounds short — about 18 cases, 40 ABBA rounds,
5 processes (~4 min default build). Fewer cases beats more cases.
User (2026-10-06, again): benchmarking takes too long - SIMPLIFY. Default round: <= 8 cases
(2 controls + the cases that answer the question), 3 processes x 30 rounds, ONE build unless the
question is specifically about the other build. Do not chain multi-build + multi-variant sweeps;
answer one question per round. Avoid rebuilding both configurations for a variant tweak.
