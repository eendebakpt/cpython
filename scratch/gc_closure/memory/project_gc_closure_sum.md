---
name: project-gc-closure-sum
description: "User's new GC cycle-detection idea (closure refcount-sum == internal-edge-count); plan in scratch/gc_closure/PLAN.md; key code facts (no mark-alive in default build)"
metadata:
  node_type: memory
  type: project
  originSessionId: f89bab7e-fe7c-47ef-bacc-ee0dd33e62bf
  modified: 2026-10-18T19:51:06.185Z
---

Started 2026-10-17. User wants a second cycle detector interleaved with the
existing one: from a candidate object, traverse its closure inside the
candidate set, sum refcounts (R) and internal tp_traverse edges (E); closure is
garbage iff R == E. Plan: `c:\develop\cpython\scratch\gc_closure\PLAN.md`
(phases: production pass -> stats path with old algorithm as ground truth ->
perf A/B -> mark-alive for gen 2 -> maybe tp_traverse2).

Facts established (main @ 21a36aa79ca, 3.16.0a1):
- Default-build gc.c has NO mark-alive phase; only gc_free_threading.c does
  (GC_ENABLE_MARK_ALIVE). Incremental GC is not on 3.13/3.14/main.
- Free bits during collection: _gc_next bit 0 (UNREACHABLE mask, only used
  between move_unreachable and move_legacy_finalizers). gc_refs lives in the
  prev pointer, so gc_list_move is unusable while gc_refs are stored.
- `_PyGC_VisitStackRef` exempts borrowed stackrefs only for
  `visit == visit_decref`; a new counting visitor must be added there or it
  produces false garbage.
- User asked for a tp_traverse2 slot; I pushed back for phase 1 (counting
  visitproc on tp_traverse gives identical E); assumption to confirm: it was
  meant as a faster enumerator, not a different edge set.

Progress 2026-10-17 (worktree `C:\develop\cpython-gcclosure`, branch
gc-closure-sum, Release+Debug built via scratch/gc_closure/build.ps1):
- Code: Python/gc_closure.c.h (included from gc.c), modes via env var
  PYTHON_GC_CLOSURE (0..4), stats dump PYTHON_GC_CLOSURE_STATS=<jsonl>.
  Scripts: scratch/gc_closure/{synth.py,analyze.py,run_pyperf.ps1,run_all.ps1}.
- Found: (1) survivors must stay in place (cursor walk) -- moving them to a
  new list made it slower and reordered young; (2) forward closures collect
  garbage in topological pieces (class retired impure because the instance
  references it, instance then pure alone) -> test_weakref
  test_clearing_weakrefs_in_gc / test_callback_in_cycle_resurrection fail in
  modes 1/3; this is inherent to the algorithm, not a bug; (3) per-candidate
  deduce time ~parity with old algorithm, missed garbage gets promoted and
  inflates gen1/gen2 work (synth automatic: 14.6ms old vs 34.7ms new);
  (4) pyperf workers set LC_ALL -> printf %f writes "0,0123": dump integers.
- pyperformance 1.14 has no --venv; it makes venv/ under cwd; works with
  the 3.16 dev build (psutil built from source fine).

Refactored + PUSHED 2026-10-17 as pte/gc-closure-sum (3 commits: core
detector 7040b68d911, stats/truth mode 26348ad61c9, truth test c3d47390dc6).
Code lives in gc.c proper (closure_pass/find_unreachable section), stats
struct in pycore_gc.h, clinic gc._closure_{mode,stats,reset_stats}.
Results write-up: scratch/gc_closure/RESULTS.md. Key numbers: gen0 recall
99% (sphinx/deltablue) but 40% on gc_* start-up garbage; full-collection
recall 22-30% (blockers dict/type/tuple = needs mark-alive); gc_traversal
1.97x faster (mode 1), create_gc_cycles 1.6x, deltablue ~1.1x, sphinx
within noise (1.11 vs 1.06-1.09 s; an earlier 1.33x "win" was machine
noise -- first run after load has 45% stddev). Modes 1/3 fail 2
test_weakref tests (piecewise collection), modes 0/2/4 green.

2026-10-18 phase 2 (plan: scratch/gc_closure/PLAN2.md; Discourse draft:
scratch/gc_closure/DISCOURSE_DRAFT.md): shared code moved to new
Python/gc_closure.c (Makefile + pythoncore + _freeze_module vcxproj entries
needed); FT closure pass in gc_free_threading.c (UNREACHABLE bit = candidate,
ob_tid high bit = discovered, replaces update_refs/thread-stacks/mark_heap
passes); mark-alive for default-build full collections (PYTHON_GC_MARK=1,
mode 5 = closure in full collections only). Decisive experiment = configs
C0/M/MC/C on 12 benchmarks (scratch/gc_closure/run_m1.ps1). KEY INSIGHT:
speed and recall are anti-correlated on the default build (fast where the
heap is in the set, accurate where it is not); the win may be marking, not
the closure sum -- M1 decides. Gotchas: (1) forward closures retire
everything reachable from earlier-visited objects, so test scenarios need the
live holder visited AFTER the garbage (frame local, or frozen holder);
(2) FT heap order != allocation order (same size class => adjacent);
(3) FT truth mode needs untracked members counted as garbage (tuples
untracked by the classic pass) or pure_but_live is spuriously >0 -- fix in
place, verify; (4) pyperformance can't build psutil for cp316t
(python316t.lib not found) -> scratch/gc_closure/run_bench_ft.ps1 runs bm
scripts directly in a hand-made venv; (5) interp-struct field changes need
-All rebuilds of BOTH builds. PEP 848 (Shannon, draft Sept 2026) is the
context for any core-dev discussion.

2026-10-18 later: switches moved to process-wide `_PyGC_closure` config (no
more -All rebuilds); PYTHON_GC_MERGE=1 union-find merging (test
test_merge_collects_class_with_instance passes); mark pass fixed (flag +
sequential partition; moving per object was 10x slower); extra roots (thread
dicts/context/exc_info/asyncio, codecs, warnings, modules). M1 truth data:
root marking lifts full-collection recall only on networkx (0.4->89%) and
sphinx (30->47%); sink-first ordering is the main miss => merge is the lever.
Exploration now via scratch/gc_closure/run_quick.ps1 (--fast, 6 benchmarks,
configs name:mode:mark:merge) + summarize_quick.py (GC-phase ms from stats).
sqlalchemy_declarative/dask can't install on 3.16 (greenlet/msgpack build).

2026-10-18 evening: union-find MERGE was WRONG (absorbs live candidates ->
recall collapse); replaced by PYTHON_GC_DAG=1 (forward closures + backward
verdict sweep over the record DAG), commit 36084a97b9c pushed. All test
configs green. Recall mark+DAG: sphinx 99%, deltablue 83%, gc_collect 98%,
async_tree 32%, xml_etree 29% (full collections miss garbage records that
also hold live unmarked types/dicts). Timing: DAG pass costs 5-20% more GC
time than classic on young-collection-heavy benchmarks; wins only with
marking on gc_* (0.65-0.7x). First q2 timing batch was disturbed (sphinx
2.4s vs 1.2s) -> use q3. Next: B5 exact remainder or B4 periodic classic
full collection; prefetch/inline traversal for speed; FT campaign with
extra roots not yet run.

2026-10-18 night: DAG shared in gc_closure.c (_PyGC_Dag*), FT DAG port
(83b086ba43a pushed): FT gen0 recall 100%, full 68-99%, FT tests green in
mode 1+DAG; FT wall time flat (mark-alive already there). Default per-gen
(q3/q4): DAG young pass 0.86-0.99x classic on sphinx/xml/gc_collect but
1.2-1.5x on async_tree/docutils/networkx; mark+DAG full collections
0.6-0.8x on gc_*/sympy/sphinx but 1.3-1.8x on docutils/networkx/raytrace
=> implemented PYTHON_GC_CLOSURE=6 adaptive (per-gen cost/candidate, probe
every 16th) + PYTHON_GC_DAG=2 (full only) + PYTHON_GC_CLASSIC_EVERY=k
(c278e435736). q5 campaign (C0/Cp/Ad/Ad2, 12 benchmarks) pending. Next
speed lever: prefetch (FT-style buffer) in mark/closure BFS loops.

2026-10-18 late: CLEAN BRANCH pte/gc-closure-young (51997372ca7, worktree
C:\develop\cpython-gcclean): plain closure pass for gen 0/1, classic full
collections, PYTHON_GC_CLASSIC=1 kill switch, tests green (1338 incl.
test_asyncio.test_tasks). KEY FINDING: the full-collection "misses" on
async_tree/xml_etree are interpreter-SHUTDOWN collections (module
teardown: functions/dicts/tuples/cells/types; a plain import+exit process
shows 8 shutdown full collections, 18% recall); shutdown now forces classic
in the experimental branch (unbuilt/untested as of this note). Machine
noise: Spyder spun at 40% of a core all day (user closed it), later
OneDrive at 90% -> all q6 numbers invalid; always run duplicate control
configs. Clean A/B campaign: scratch/gc_closure/run_clean.ps1 (results in
results_clean, configs C0a/A/C0b/Ab).

**How to apply:** continue from the plan's "Order of work"; reuse
scratch/incgc_bench*.py for timing; stats are env-var driven
(PYTHON_GC_CLOSURE, PYTHON_GC_CLOSURE_STATS). See [[incremental-gc-review]]
for earlier GC worktree/bench setup and [[single-binary-ab]] for A/B method.

2026-10-18 23:15 PAUSED (user: "we will continue later"). All benchmark
jobs aborted; results_clean/clean_* (12-bm A/B) and full_C0a.log are
NOISY/partial, do not quote. DISCOURSE_DRAFT.md updated: results section =
q5 young-collection GC-time table (0.61-0.98x, 12 bms) for the proposal +
q3 table for the mark+DAG experiment; notes-to-self list what is pending.
Uncommitted in cpython-gcclosure: shutdown->classic rule, sysdict_copy/
builtins_copy roots, gc._closure_blockers sampler (type names only, no
referrer chains). OPEN QUESTION from user: object-level examples of gen-2
blockers; only type-level evidence exists (dict/type/tuple; shutdown module
teardown). blockers.py found 0 blockers inside xml_etree/async_tree
workloads (all misses are shutdown collections, not Python-introspectable).
Next: a C-side sampler that records the blocker's owner (dict -> owning
module/type via ma_keys/tp_dict match, or walk referrers in C).

2026-10-19: PEP 848 reference impl = markshannon/cpython branch
incremental-gc (single commit 91b7749df68, base a6ad4482377; fetched as
remote `markshannon`). Worktree C:\develop\cpython-incgc now on branch
gc-closure-pep848 (closure pass ported into gc_collect_region(young flag);
nursery/aging collections use it, old increments/full keep classic;
NEXT_MASK_OLD_SPACE tag preserved). Tests green both modes, incremental and
-X gc=legacy. Probe (1M 2-node cycles): same collected count, young GC time
within noise (garbage-dominated workload). Campaign: run_pep848.ps1 (venv
hook gcstatsdump.py dumps gc.get_stats() per worker to GC_STATS_OUT),
summarize_pep848.py; results in results_pep848/. gc.collect(0) under the
incremental GC needs aging_spaces+1 calls to collect fresh objects (tests
use a young() helper looping 8x).
PEP 848 campaign #1 (results_pep848/pep_*) INVALID: controls differed 2x
(machine loaded 09:20-10:20); rerun as prefix pep2 at 10:30 on an idle
machine. Summarize with summarize_pep848.py results_pep848 pep2. Build
lesson: build.ps1 without -All leaves 18/37 .pyd (no unicodedata) -> venv
pip install fails; venvlauncher.vcxproj must be built for `python -m venv`.
2026-10-19 11:30: PEP 848 result (in-process A/B, pep848/ab_inproc.py +
run_inproc.ps1 -Py/-XOpt; DEBUG_CLASSIC=1<<7 gc.set_debug bit on BOTH
branches, clean branch rebuilt with it, uncommitted): young collections
0.71-0.81x on deltablue/mdp/networkx/async_tree, flat on sphinx/docutils/
xml_etree/sympy, 1.11x raytrace (4 candidates/collection). Table in
RESULTS.md. pyperformance --fast campaigns are useless for small
benchmarks (5 young collections per worker) -> use the in-process driver.
Clean-branch venv: scratch/gc_closure/clean/venv/cpython3.16-4bb89271cd21-*
(all deps). Next: run driver on clean branch (-XOpt dev), update draft,
commit DEBUG_CLASSIC on both branches.
2026-10-19 11:45 DONE: main in-process A/B (inproc_main.jsonl): gen0
0.61-0.89x on 8 benchmarks, gen1 0.73-1.16x; PEP 848 young 0.71-1.11x.
Both tables in RESULTS.md and DISCOURSE_DRAFT.md. DEBUG_CLASSIC commits
pushed: pte/gc-closure-young cdf3f15c51d, pte/gc-closure-pep848
1dfaa6fd5b6. Open: small-candidate fallback (raytrace 4 candidates ->
1.11x), peak memory, docs build, FT table.
2026-10-19 12:45: user wants MEASURED run-time tables only (no derived
"expected saving"). Draft restructured (short top + [details] sections);
table 1 = main-only GC share, table 2 = run time vs main for main+closure /
PEP 848 / PEP 848+closure, TBD. Campaign plan: scratch/gc_closure/PLAN3.md
(two binaries on base a6ad4482377, runtime switch as control, 4 configs x 3
rounds overnight, 3% baseline acceptance; round 2 = real GC-heavy
workloads instrumented for GC share first). NOT RUN YET.
