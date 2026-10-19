# Plan 3: measured run-time comparison main vs closure vs PEP 848 (to run later)

Goal: two tables for the Discourse post, all numbers measured, nothing derived.

Table 1 (main only): where the time goes.
Table 2: run time relative to main for (1) main + closure pass,
(2) PEP 848, (3) PEP 848 + closure pass; same benchmarks, including
gc_collect and gc_traversal.

## Binaries (two, four configurations)

The runtime switch (`PYTHON_GC_CLASSIC=1`, a `gc.set_debug()` bit) makes
each binary its own control, so only two builds are needed and each pair is
a single-binary A/B:

| binary | PYTHON_GC_CLASSIC=1 | PYTHON_GC_CLASSIC=0 |
|---|---|---|
| B: gc-closure-young rebased onto a6ad4482377 | main (baseline) | main + closure |
| P: gc-closure-pep848 (1dfaa6fd5b6, base a6ad4482377) | PEP 848 | PEP 848 + closure |

Both must share the base commit a6ad4482377 (the PEP 848 merge-base), so
the comparison across binaries measures only the collector. Steps:

1. `git worktree add C:\develop\cpython-cmpbase a6ad4482377`, branch
   `gc-closure-young-cmp`, cherry-pick the two gc-closure-young commits
   (51997372ca7 + cdf3f15c51d; expect a clean apply, gc.c around
   handle_resurrected_objects is unchanged between the bases). Full
   Release build with `build.ps1 -All` (the python-only build lacks
   unicodedata and the venv launcher; see memory).
2. P is already built in C:\develop\cpython-incgc (full Release).
3. One pyperformance venv per binary (`pep848\work\venv` exists for P);
   install the `gcstatsdump.py` hook in both. Extend the hook to time every
   collection through `gc.callbacks` (main's `gc.get_stats()` has no
   durations), recording per generation: collections, collected, time.
   Also record `time.process_time()` at exit for the GC share.

## Benchmarks

Round 1, the current twelve: gc_collect, gc_traversal, deltablue, sphinx,
xml_etree, async_tree_memoization, docutils, sympy, networkx, mdp,
raytrace, richards. sqlalchemy_declarative and dask if greenlet/msgpack
install by then.

Round 2, GC-heavy real workloads, instrumented for GC share first (the
hook, one run each, no A/B), kept only if GC is 10% or more of run time:
mypy type-checking its own sources; pytest collecting Lib/test; the full
CPython docs build (Doc/, html builder); black formatting Lib/; the Django
test suite (or a subset); an asyncio server benchmark with many live tasks
(the async_tree shape is the one measurable win so far).

## Protocol

- Idle machine: Spyder, OneDrive, Outlook closed; VS Code without the
  cpptools indexer on the new worktree; run overnight with the idle-wait
  from run_clean_full.ps1 (below 1.0 core busy).
- pyperformance per benchmark, no `--fast` (the fast runs trigger too few
  young collections on small benchmarks). Order per round: B1, B0, P1, P0
  (1 = classic, 0 = closure), three rounds, interleaved, so every
  configuration sees the same drift.
- Acceptance: the three B1 rounds (the baseline) must agree within 3%;
  otherwise the round is discarded. Report the median of the rounds.
- Metrics per configuration and benchmark: wall time per iteration
  (pyperf), time in automatic collections as % of process time (hook),
  split young / old-increment / full.
- Estimated duration: 12 benchmarks x 4 configurations x 3 rounds x
  ~2.5 min = ~6 h, overnight.

## Table 1 (main only, from the B1 configuration)

| benchmark | run time per iteration | time in GC, % of run time | young collections, % of run time | full collections, % of run time | class |
|---|---|---|---|---|---|

"class": GC-relevant if GC is 5% or more of run time, otherwise
compute-dominated; gc_collect and gc_traversal are GC-only microbenchmarks.
Until the campaign runs, the draft carries the in-process 60 s numbers for
the share columns (measured, both detectors alternating) and no run-time
column.

## Table 2 (run time relative to main, lower is better)

| benchmark | main + closure | PEP 848 | PEP 848 + closure | young-collection GC time, main + closure vs main (already measured) |
|---|---|---|---|---|

The last column is the in-process gen-0 ratio already in hand; the three
run-time columns are filled from the campaign. gc_collect and gc_traversal
are included: they run explicit full collections only, so main + closure
should be 1.00x there and PEP 848's full collection (mark from sys, then
collect) is what changes.

## Scripts to write

- `run_cmp.ps1`: the four-configuration interleaved campaign with
  idle-wait, duplicate-control acceptance, per-benchmark result files
  `results_cmp/<round>_<config>_<bench>.json` and `.stats.jsonl`.
- `summarize_cmp.py`: the two tables above, medians over accepted rounds,
  control spread column for honesty.
- `gcstatsdump.py`: add `gc.callbacks` timing, process time at exit.
