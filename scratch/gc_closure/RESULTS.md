# Closure-sum cycle detector: results (2026-10-17)

Branch `gc-closure-sum` (worktree `C:\develop\cpython-gcclosure`), main @ 21a36aa79ca.
Release build, MSVC v145, no PGO. Scripts and raw data in `scratch/gc_closure/`.

Pushed as `pte/gc-closure-sum`: 7040b68d911 (detector, docs, tests),
26348ad61c9 (statistics, truth mode, gc._closure_* helpers),
c3d47390dc6 (truth-mode test).

## What was built

- `closure_pass()` in `Python/gc.c`: from each not-yet-retired candidate,
  breadth-first closure inside the candidate set; R = sum of refcounts,
  E = internal tp_traverse edges; R == E -> closure moved to `unreachable`,
  else members retired in place. Same post-conditions as `deduce_unreachable()`.
- Modes via `PYTHON_GC_CLOSURE`: 0 off, 1 all collections, 2 gen 0/1 only,
  3 alternate, 4 truth mode (closure pass + old algorithm, statistics).
- `PYTHON_GC_CLOSURE_STATS=<file>`: one JSON line per process;
  `gc._closure_stats()`, `gc._closure_reset_stats()`, `gc._closure_mode()`.
- No tp_traverse2: the edge count comes from a counting visitproc on the
  existing tp_traverse (same information).

## Correctness

- Debug build, truth mode: no phantom closure (E > R) and no pure closure that
  the old algorithm did not also find entirely unreachable, on the synthetic
  cases and on test_gc/test_weakref/test_finalization.
- test_gc, test_weakref, test_finalization pass in modes 0, 2, 4 (Release).
  Mode 1 and 3 fail two test_weakref tests (`test_clearing_weakrefs_in_gc`,
  `test_callback_in_cycle_resurrection`): a garbage class and its garbage
  instance are collected in two successive collections (the class is visited
  first, looks externally referenced because of the instance, and is kept),
  so the "weakref callbacks are cleared before finalizers run" ordering no
  longer spans the whole unreachable set. Inherent to forward closures, not
  an implementation bug. Debug builds additionally differ in the assertion
  text for a broken tp_traverse (test adjusted to accept both).

## Recall (truth mode, per benchmark; pure-closure garbage / true garbage)

| benchmark    | gen 0 | gen 1 | gen 2 (full) | gen 2 blockers              |
|--------------|-------|-------|--------------|-----------------------------|
| gc_collect   | 40.3% | 88.5% | 89.9%        | dict, type, tuple           |
| gc_traversal | 40.3% | 88.5% | 21.9%        | dict, type, tuple           |
| sphinx       | 99.3% | 92.9% | 29.8%        | dict, tuple, type           |
| deltablue    | 99.1% | 99.9% | 24.9%        | dict, tuple, type           |

Notes:
- gc_collect/gc_traversal gen 0: the garbage there is start-up garbage
  (functions, dicts, cells: ~4.7k objects in 18 processes), not the
  benchmark's cycles; those are collected by the explicit `gc.collect()`
  (gen 2). gc_collect gen 2 recall 89.9%: the Node cycles are found.
- Full collections: the whole heap is the candidate set, so a garbage
  instance's reference to its live class, or to a module dict, makes the
  closure unknown. The blockers are exactly the objects a mark-alive pass
  would exclude. Missed garbage in sphinx gen 2: 3.4M objects (lists, dicts,
  docutils Text nodes).
- Closure sizes: median 1 object, p99 under 2^7; pure closures median 2^5-2^6
  (one benchmark cycle plus its dicts); queue high-water up to 2.2M entries
  in sphinx full collections (17 MB transient).

## Timing (pyperformance, final interleaved A/B, two runs per mode)

Same binary, PYTHON_GC_CLOSURE switched per run, runs interleaved
0,1,2,0,1,2 on an otherwise idle machine; sphinx in a separate 0,1,0,1 series.
The first run of each series is still disturbed (12-45% std dev) and is
listed for honesty; compare the clean runs.

| benchmark        | mode 0 (two runs)  | mode 1 (all gens)   | mode 2 (gen 0/1 only) |
|------------------|--------------------|---------------------|-----------------------|
| gc_traversal     | 3.44, 3.35 ms      | 1.73, 1.71 ms (1.95x faster) | 3.39, 3.63 ms (flat) |
| create_gc_cycles | 1.32*, 1.17 ms     | 0.82, 0.80 ms (1.45x faster) | 1.25, 1.33 ms (~8% slower) |
| deltablue        | 6.03*, 5.57 ms     | 5.58, 5.52 ms (flat)         | 5.58, 5.75 ms (flat) |
| sphinx           | 1.90*, 1.11 s      | 1.09, 1.06 s (flat, <5%)     | not run in series (earlier: not significant) |

(* first run of the series, high variance.)

Earlier campaigns taken while builds/tests ran concurrently showed sphinx
1.20x slower, then 1.33x faster, and deltablue 1.06x slower, then 1.10x
faster: all noise. Only the gc_* results were stable across all campaigns.

Deduce-phase time alone (same process, truth mode, closure pass includes
logging overhead): gen 0 closure pass 0.9-1.3x the old algorithm per
candidate; gen 2 closure pass 0.6-0.85x (edge-heavy full collections).
Synthetic cases (mode 1 vs mode 0, no logging): self-cycles 1.6x slower,
instance cycles on par, live tree on par, long chain 1.15x faster; the
"automatic collections" case with 50% contaminated cycles doubled total GC
time because promoted garbage is re-scanned in gen 1/2.

## Free-threaded build (2026-10-18, truth mode, mark-alive on, 4 benchmarks)

Recall of the closure pass on the non-alive remainder, per benchmark
(gen 0 = automatic collections, gen 2 = explicit gc.collect()):

| benchmark | gen 0 | gen 2 | closure pass / classic time on the remainder |
|---|---|---|---|
| gc_collect | 28% (start-up garbage) | 96% (the Node cycles) | 0.93x |
| gc_traversal | 28% | 11% (tiny garbage) | 0.94x |
| sphinx | 73% | 33% | 0.97x |
| deltablue | 97% | 28% | 1.19x |

- `pure_but_live` is 0 everywhere after counting tuples untracked by the
  classic pass as garbage (they were falsely reported as live before).
- With mark-alive already shrinking the candidate set, the closure pass
  costs about the same as the classic three passes on that remainder: the
  traversal saving is gone because the remainder is small and sparse, and
  the pass finds 28-97% of what the classic one finds.
- Blockers are again dicts (121k in sphinx gen 0 over 345 collections):
  live objects that root marking did not reach, i.e. held through non-GC
  objects or interpreter/thread state (exception state, contextvars,
  thread dicts). Added as roots in both builds; to be measured.
- The default build's generation boundary (recall 93-99% in gen 0/1) is a
  better exclusion heuristic than root marking on these workloads: age
  excludes old live objects however they are held.

## Phase 2 (2026-10-18): merging + marking

Test matrix (test_gc, test_weakref, test_finalization; "mark" = mark-alive
from roots before full collections, "merge" = union-find closure merging):

| configuration | result |
|---|---|
| closure everywhere, no mark, no merge (mode 1) | 2 failures (weakref ordering) |
| closure everywhere, no mark, merge | 40 failures: without marking, merging joins garbage with the live state it references, nothing is pure in full collections |
| closure in full collections only, mark, merge (mode 5) | all green |
| closure everywhere, mark in full collections, merge (mode 1 + mark + merge) | all green |
| truth mode, with/without mark, merge | all green |
| FT modes 0/2/4 with the extra roots | all green |

So the combination "closure pass + mark-alive in full collections + merging"
passes the complete GC test suites, including the two weakref/finalizer
ordering tests that the plain closure pass fails: merging collects a garbage
class together with its instances in the same collection, and marking keeps
the live module/class state out of the merged closures.

Milestone-1 truth data (default build, 10 benchmarks, no merge) showed that
root marking alone lifts full-collection recall only where live state is
root-reachable (networkx 0.4% -> 89%, sphinx 30% -> 47%, others flat), which
identified the sink-first visit order as the main cause of missed garbage
in full collections; merging is the fix for that. The first mark pass was
also 10x slower than a traversal because it moved every marked object
between lists; it now only sets a flag and partitions the list sequentially.

## DAG verdict (2026-10-18, quick campaign, --fast, default build)

Union-find merging turned out to be wrong: it also absorbs the live
candidates a closure points at, so recall collapsed (gc_collect 94% -> 0.2%
without marking, sphinx 63% -> 54% with it). Replaced by forward closures
plus a backward sweep over the record DAG (edges between records always
point from a later record to an earlier one; a record is garbage iff its
external references are exactly the edges from records already judged
garbage). Test matrix: all green in every configuration (closure everywhere
with/without marking, full-collections-only, truth modes), including the
weakref/finalizer ordering tests.

Recall (truth mode; T0 = plain closure, TMd = mark + DAG; all generations):

| benchmark | plain | mark + merge | DAG | mark + DAG |
|---|---|---|---|---|
| sphinx | 63% | 54% | 99% | 99% |
| deltablue | 80% | 71% | 85% | 83% |
| gc_collect | 94% | 97% | 92% | 98% |
| async_tree_memoization | 19% | 6% | 37% | 32% |
| gc_traversal | 22% | 6% | 43% | 34% |
| xml_etree | 18% | 8% | 32% | 29% |

Per generation (mark + DAG): gen 0 recall is 87% (async_tree), 95%
(xml_etree), gen 1 is 100%; what is missed sits in the full collections
(28-31%): garbage functions/dicts/tuples/cells whose records also contain
live types, dicts and tuples that root marking does not reach. The exact
fix for that remainder is the classic algorithm on the unknown records
(PLAN2 B5); the cheap one is a periodic classic full collection (B4).

Timing, clean re-run (q3; --fast, one run per cell; "gc" = GC-phase time
from the stats dump, per collection where the loop counts differ):

| benchmark | M: mark + classic | Cd: DAG everywhere | Cdm: DAG + mark in full collections |
|---|---|---|---|
| gc_collect | 0.79x wall, gc 0.78x | 0.80x, gc 0.82x | **0.77x, gc 0.78x** |
| gc_traversal | 0.57x wall, gc/coll 0.64x | 0.68x, gc/coll 0.72x | **0.58x, gc/coll 0.64x** |
| sphinx | 1.16x wall, gc 1.14x | 1.01x, gc 0.92x | **0.98x, gc 0.94x** |
| xml_etree | 1.01x, gc 0.97x | 1.01x, gc 0.91x | **1.01x, gc 0.90x** |
| async_tree_memoization | 1.00x, gc 1.02x | 1.03x, gc 1.19x | **1.00x, gc 1.05x** |
| deltablue | 1.01x, gc 0.98x | 1.03x, gc 0.92x | **1.00x, gc 1.04x** |

Reading: marking alone (M) pays on the gc benchmarks and loses on sphinx
(the live-heap traversal is not recovered); the DAG closure pass alone (Cd)
is 8-10% cheaper than classic on sphinx/xml_etree GC phases and 19% dearer
on async_tree; the combination (Cdm) is never worse than classic in wall
time, 6-10% cheaper in GC time on sphinx/xml_etree, 4-5% dearer on
async_tree/deltablue (invisible in wall time), and 1.3-1.7x faster where
full collections of a live heap dominate. All of this with the complete GC
test suites green.

## Free-threaded build with the DAG verdict (2026-10-18)

The DAG pass ported to the FT collector (record index in `ob_tid` next to
the discovered flag, one extra sequential heap walk to retire the members
of non-garbage records). FT test suites: all green in closure-everywhere
mode with DAG (10 failures without it). Recall on the non-alive remainder
(truth mode):

| benchmark | gen 0 plain -> DAG | gen 2 plain -> DAG |
|---|---|---|
| gc_collect | 28% -> 100% | 96% -> 99% |
| gc_traversal | 28% -> 100% | 11% -> 68% |
| sphinx | 73% -> 100% | 33% -> 94% |
| deltablue | 97% -> 100% | 28% -> 76% |

The extra interpreter/thread-state roots changed nothing measurable on FT
(about 60 more objects marked per collection): the FT misses were the
sink-first ordering effect, not unmarked live state.

FT wall time, classic vs closure+DAG for every collection (pyperf --fast,
two rounds; the first round overlapped with other work, std dev 15-34%,
the second round is the one to read): gc_collect 1.73 vs 1.79 ms,
gc_traversal 3.24 vs 3.24 ms, sphinx 1.86 vs 1.76 s, deltablue 6.86 vs
7.08 ms, i.e. flat within noise. Expected: the FT classic path already
marks the live heap once, so the "traverse once instead of twice" saving
does not exist there; the closure/DAG pass is an order-robust way to
compute the remainder at the same cost, with 94-100% recall.

## Twelve benchmarks, per generation (q5, 2026-10-18 evening)

Configurations: Cp = plain closure pass in young collections, mark + DAG in
full collections; Ad/Ad2 = adaptive (per-generation cheaper detector,
re-probed every 16th collection) with DAG everywhere / in full collections
only. GC-phase time relative to classic:

| benchmark | young, Cp | full (per collection), Cp | wall, Cp |
|---|---|---|---|
| gc_collect | 0.61x | 0.72x | 0.77x |
| gc_traversal | 0.98x | 0.54x | 0.55x |
| async_tree_memoization | 0.76x | 0.91x | 0.95x |
| sphinx | 0.74x | 0.93x | 1.06x* |
| xml_etree | 0.73x | 1.07x | 1.00x |
| sympy | 0.81x | 0.83x | 1.01x |
| mdp | 0.77x | 1.10x | 0.93x |
| raytrace | 0.79x | 1.05x | 0.99x |
| richards | 0.78x | 1.12x | 1.01x |
| deltablue | 0.96x | 1.17x | 1.00x |
| docutils | 0.95x | 1.50x | 1.02x |
| networkx | 0.93x | 1.48x | 1.02x |

(* single --fast run; sphinx wall varies by ±5% between identical runs.)

Two clear findings:

1. The plain closure pass is cheaper than the classic algorithm in every
   young collection (5-40%, 0.61-0.98x). The earlier "cost-neutral"
   reading came from the DAG bookkeeping; without it the single traversal
   wins. Young collections with the plain pass and classic full collections
   (mode 2) is therefore an exact configuration (the classic full
   collection resolves every unknown closure) with a consistent GC-time
   gain and no semantic change for `gc.collect()`.
2. Mark + DAG full collections win where the live heap is large relative
   to the work the classic pass does (gc_*, sympy, sphinx, async_tree) and
   lose up to 1.5x per full collection on docutils and networkx. The
   breadth-first passes visit objects in discovery order, every header a
   cache miss, while the classic passes walk the list in allocation order;
   prefetching the queue is the next step (q6), and the adaptive mode did
   not rescue networkx (its probes are too expensive there).

## Where the full-collection misses come from (2026-10-18, late)

A sampling diagnostic (`gc._closure_blockers()`, truth mode) found no
unknown-with-garbage record in full collections during the xml_etree or
async_tree workloads themselves (12 rounds each). A process that only
imports asyncio, xml.etree and json and exits shows them all: 8 full
collections at interpreter shutdown, 27.6k garbage objects of which 22.6k
missed (18% recall), types functions 7.5k, dicts 3.7k, tuples 2.5k,
builtin methods 1.1k, cells 0.9k, types 0.8k, blockers dicts/tuples/types.
That is module teardown: namespaces become garbage while module objects,
their dicts and classes are still referenced by the finalization machinery
(sys.modules entries being replaced, copies of sys and builtins dicts,
per-module state). The benchmark workers each shut down once, so this
dominates the "28-31% full-collection recall" of async_tree and xml_etree
and contributes to the others.

Consequences: shutdown collections now always use the classic algorithm
(both builds), and the runtime relevance of full-collection recall is
better than the aggregate suggested. The truth mode's `missed_types`
histogram with functions/cells/types/builtin methods on top is the
signature of shutdown garbage.

Concrete blocker examples seen: frame-held temporaries (a list created
after a cycle and still referenced from a running frame's local: the
frame's stack is a root, so marking fixes it, the generation boundary does
not); objects held by a frozen holder (`gc.freeze()`); class objects whose
instances are alive but whose own holder chain is not root-reachable
during shutdown; module dicts during teardown.

## Interpretation

- Where the candidate set is edge-heavy and mostly live (gc_traversal, full
  collections of a big live heap) a single traversal instead of two is a
  clear win: up to 2x on the deduce phase.
- In young collections the pass is roughly cost-neutral per candidate; the
  old algorithm's two passes are cheap there because most young objects have
  few internal edges. On deltablue and sphinx (application-like allocation
  patterns) no mode changes wall time measurably.
- The real cost is lost precision: full-collection recall of 22-30% on
  sphinx/deltablue means mode 1 leaves most cyclic garbage of a full
  collection alive until the next collection that runs the old algorithm;
  mode 2 (closure pass only for gen 0/1, where recall is 89-99%) avoids that
  and keeps a small win. Memory impact was not measured (follow-up:
  pyperf --track-memory).
- Gate for a mark-alive phase in full collections: blockers are dict/type/
  tuple = classes and module state, which mark-alive from sysdict/builtins/
  stacks would exclude; expected to lift gen-2 recall substantially.
- Semantic difference to keep in mind: piecewise collection of garbage
  DAGs across collections (weakref callback / finalizer ordering).

## PEP 848 (2026-10-19): closure pass in the incremental collector's young collections

Reference implementation: markshannon/cpython branch `incremental-gc`
(91b7749df68, base a6ad4482377).  Port: branch `gc-closure-pep848`
(worktree C:\develop\cpython-incgc, pushed to pte).  `gc_collect_region()`
takes a `young` flag; nursery/aging-space collections (and legacy
generations 0 and 1) use `closure_pass()`, old increments and full
collections keep `deduce_unreachable()`.  Tests green with the pass on and
off, incremental and `-X gc=legacy`.

Two pyperformance `--fast` campaigns (results_pep848/pep_*, pep2_*) were
invalidated by machine load (identical classic runs differed up to 2x);
with `--fast` the small benchmarks also run only 5-6 young collections per
worker, so their GC counters are meaningless.  Replaced by an in-process
A/B (`pep848/ab_inproc.py`, `run_inproc.ps1`): the detector is a
`gc.set_debug()` bit (`DEBUG_CLASSIC = 1<<7`) toggled after every young
collection in a balanced non-periodic sequence, time per collection from
`gc.callbacks`, candidates from `gc.get_stats()` deltas.  Old increments are
classic in both modes and serve as the noise control.  60 s per benchmark.

| benchmark | young collections | candidates / young collection | young GC time closure/classic | per candidate | old-increment control | young share of GC time |
|---|---|---|---|---|---|---|
| deltablue | 158/158 | 24,081 | 0.71x | 0.70x | 1.09x | 18% |
| mdp | 1599/1599 | 925 | 0.76x | 0.77x | 1.04x | 42% |
| networkx | 132/132 | 4,873 | 0.77x | 0.65x | 1.27x | 75% |
| async_tree_memoization | 1187/1185 | 19,279 | 0.81x | 0.82x | 0.99x | 53% |
| sympy | 689/690 | 911 | 0.95x | 0.92x | 0.79x | 69% |
| docutils | 713/713 | 15,179 | 0.97x | 0.95x | 1.04x | 60% |
| sphinx | 1055/1055 | 4,752 | 1.00x | 0.98x | 1.04x | 54% |
| xml_etree | 895/893 | 498 | 1.03x | 0.99x | 0.90x | 47% |
| raytrace | 912/912 | 4 | 1.11x | 1.11x | 1.01x | 58% |
| richards | 0 | - | no young collections in 60 s | - | - | - |

Reading: under PEP 848 a "young" collection is one aging space, i.e.
objects that survived five nursery rotations, so it is mostly live
(deltablue/mdp/async_tree/networkx collect 0 objects in young
collections; their garbage is found by the old increments).  That is the
case the closure pass is good at: one traversal, no moves, 0.65-0.8x per
candidate.  Where the aging space holds mostly garbage (sphinx: 2.6k of
4.7k candidates per collection, docutils 11k of 15k) the garbage handling
dominates and the deduce saving is invisible; where candidate sets are tiny
(raytrace 4, xml_etree 500) fixed costs dominate.  Old increments and full
collections are unchanged by construction.  gc_collect/gc_traversal call
gc.collect() and only run full collections: no change possible.

## Main-based clean branch (gc-closure-young), in-process A/B (2026-10-19)

Same driver (pep848/ab_inproc.py, run_inproc.ps1 -Py <clean venv> -XOpt
utf8), detector toggled after every gen-0 and gen-1 collection, 60 s per
benchmark.  Full collections are not balanced by the toggle (their mode is
incidental), so they are not a control here.

| benchmark | gen 0 collections (classic/closure) | gen 0 time closure/classic | gen 1 time | gen 0+1 per candidate |
|---|---|---|---|---|
| deltablue | 1765/1755 | 0.61x | 0.73x | 0.66x |
| networkx | 851/823 | 0.67x | 0.82x | 0.64x |
| xml_etree | 6145/6107 | 0.73x | 0.82x | 0.74x |
| async_tree_memoization | 11774/11739 | 0.74x | 0.81x | 0.75x |
| sphinx | 2013/1984 | 0.82x | 1.09x | 0.90x |
| mdp | 520/516 | 0.83x | 0.89x | 0.82x |
| sympy | 372/363 | 0.86x | 1.16x | 0.90x |
| docutils | 3986/3951 | 0.89x | 0.97x | 0.90x |
| richards | 4/6 | too few | - | - |
| raytrace | 0/0 | none | - | - |

Consistent with the q5 campaign (0.61-0.98x) but with a clean control.
