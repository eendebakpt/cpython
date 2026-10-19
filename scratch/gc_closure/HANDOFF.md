# Handoff: closure-sum GC detector (state on 2026-10-19)

Everything needed to continue on another machine. The notes, scripts,
results and the relevant assistant memory files live in this directory,
committed on branch `gc-closure-sum` under `scratch/gc_closure/`. On the
original machine `scratch/` is git-excluded (`.git/info/exclude`), so the
files were added with `git add -f`.

## Branches (all on github.com/eendebakpt/cpython)

| branch | purpose | head |
|---|---|---|
| `gc-closure-young` | the proposal: plain closure pass in generations 0 and 1, classic full collections, runtime switch `PYTHON_GC_CLASSIC=1` (gc.set_debug bit 1<<7) | cdf3f15c51d (base: main 21a36aa79ca) |
| `gc-closure-pep848` | the same pass on PEP 848's reference implementation (markshannon/cpython `incremental-gc` 91b7749df68, base a6ad4482377); `young` flag on gc_collect_region() | 1dfaa6fd5b6 |
| `gc-closure-sum` | experiment branch: modes, DAG verdict, mark-alive for full collections, truth mode, FT port, shutdown rule, blocker sampler, this notes directory | b07d83706a8 + notes commit |

Rules kept so far: never force-push; no issue/PR numbers in commit
messages; commits end with the Co-Authored-By line.

## Read in this order

1. `memory/project_gc_closure_sum.md`: chronological log of findings and
   gotchas (the assistant's memory file).
2. `RESULTS.md`: every measurement with method and reading.
3. `DISCOURSE_DRAFT.md`: the post as it stands (short top, folded
   sections; two tables with TBD run-time columns).
4. `PLAN3.md`: the campaign that fills the TBD columns, not yet run.
5. `PLAN.md`, `PLAN2.md`: earlier plans (phase 1, phase 2), mostly done.

## Key facts

- Young collections: closure pass 0.61-0.89x of classic in gen 0 on main
  (in-process alternating A/B), 0.71-0.81x on PEP 848 aging spaces where
  they are large and live, flat where mostly garbage or tiny, 1.11x with
  4 candidates (raytrace; needs a small-count fallback).
- Run time: GC is at most 19% of run time on pyperformance (async_tree),
  young collections at most 10%; only async_tree can show a run-time
  change. Only measured run-time numbers go in the post.
- Full collections: closure pass needs mark-alive + DAG verdict; wins 1.3-
  1.7x on gc_* microbenchmarks, loses on docutils/networkx (BFS order =
  cache misses; prefetch implemented in gc-closure-sum, unmeasured on a
  quiet machine). Mark-alive alone has the same profile.
- Recall misses in full collections are mostly interpreter-shutdown
  collections (module teardown); shutdown now forces classic.
- Object-level examples of gen-2 blockers were never obtained: the sampler
  records type names only (dict/type/tuple); inside the xml_etree and
  async_tree workloads proper there were zero blockers.

## Scripts (paths are Windows-specific; adapt the constants at the top)

- `build.ps1 -Dir <worktree> [-Config Debug] [-All]`: MSBuild. Without
  `-All` only python + a few test modules are built: no unicodedata, no
  venvlauncher, so pyperformance venvs fail. Use `-All` for benchmarking.
- `pep848/ab_inproc.py` + `pep848/run_inproc.ps1 [-Py <venv python>]
  [-XOpt utf8|gc=incremental] [-Bench "..."] [-Seconds 60] [-Out ...]`:
  in-process alternating A/B (the trustworthy method). Toggles the
  detector after every young collection (gen 0 on PEP 848; gen 0 and 1 on
  main). `pep848/summarize_inproc.py results_pep848/inproc*.jsonl` prints
  the table.
- `pep848/gcstatsdump.py`: venv hook (`.pth` + module) that appends
  gc.get_stats() per worker at exit (PEP 848 branch only; main has no
  durations in get_stats; PLAN3 says to switch it to gc.callbacks timing).
- `run_pep848.ps1`, `run_clean.ps1`, `run_clean_full.ps1`, `run_quick.ps1`,
  `run_pyperf.ps1`, `run_bench_ft.ps1`: pyperformance campaigns (separate
  processes; need an idle machine and duplicate controls; the results in
  results_pep848/pep_*, pep2_* and results_clean/clean_* are INVALID due
  to machine load).
- `compare_md.py`, `summarize_quick.py`, `summarize_pep848.py`,
  `analyze.py`, `m1_recall.py`: summarizers for those campaigns.
- `blockers.py`, `probe_unknown*.py`, `synth.py`, `findbad.py`,
  `repro_resurrect.py`, `pep848/probe.py`: probes.

## Results directories

- `results/`: phase 1-2 campaigns on gc-closure-sum (q2..q6, m1, stats).
- `results_ft/`: free-threaded runs.
- `results_clean/`: clean-branch pyperf campaign (noisy, do not quote).
- `results_pep848/`: `inproc_main.jsonl` (main, valid), `inproc.jsonl`,
  `inproc2.jsonl`, `inproc3.jsonl` (PEP 848, valid), `pep_*`/`pep2_*`
  (invalid).

Not included: virtual environments (`clean/`, `ft/`, `pep848/work*/`),
build logs.

## Next steps (PLAN3.md)

1. Worktree at a6ad4482377, cherry-pick the two gc-closure-young commits,
   full build; PEP 848 port already builds.
2. Hook with gc.callbacks timing; two pyperformance venvs.
3. Four configurations x 12 benchmarks x 3 rounds, overnight, 3% baseline
   acceptance; fill table 2 of the draft; table 1 from the baseline.
4. Instrument real workloads for GC share (mypy, pytest collection, docs
   build, black, Django, asyncio server); A/B those above 10%.
5. Small-candidate fallback in closure_pass (raytrace); peak memory; FT
   table; prefetch measurement for mark-alive.
