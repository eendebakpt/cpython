# Closure-sum GC: plan for phase 2 (2026-10-18)

Goal: an approach that is correct in all cases and gives a significant speedup
in many cases, measured on 12 benchmarks, written up for core devs.

## 0. Where we stand

- Default build: the closure pass costs one traversal per object instead of
  two. In gen 0/1 collections that is worth nothing (candidate sets are small
  and sparse: ~1.1 internal edges per candidate on sphinx), in full
  collections of edge-heavy live heaps it is worth up to 2x (gc_traversal
  1.95x). Recall is 93-99% in gen 0/1 and 22-30% in full collections, because
  live class and module state sits inside the candidate set and makes almost
  every closure unknown (blockers: dict, type, tuple).
- Unknown closures are promoted and only resolved by a later classic
  collection. Mode 1 (closure only) therefore never resolves garbage that
  references a permanently live object, and collects garbage structures in
  pieces, which breaks two weakref/finalizer ordering tests. Mode 2 (gen 0/1
  closure, full classic) is exact but gains nothing, by construction.
- Free-threaded build: has a mark-alive phase, so the closure pass runs in
  its intended regime; synthetic recall 100% on the cases the default build
  got half right. Timing campaign pending; a `pure_but_live` artifact
  (tuples untracked by the classic pass after the closure pass saw them) has
  a fix that still needs verification.
- Context: PEP 848 (Mark Shannon, draft 2026-09-16, target 3.16) proposes a
  generational incremental GC that finds garbage by scanning reachability
  from roots and then checking the unreached set for cycles; claims 3-5% on
  pyperformance and up to 100x shorter pauses. The reference implementation
  is the branch reviewed on 2026-09-25 (worktree `cpython-incgc`). Anything
  we propose has to be positioned relative to it; the natural fit is the
  "check the unreached candidates" step and the young collection.

## 1. The decisive experiment first

The data so far cannot tell marking apart from closure sums: the only
configuration with a big win (full collections, mode 1) is also the one
where the candidate set is the whole heap, and the only configuration with
high recall (gen 0/1) is the one where the generation boundary plays the
role of a marking pass. So before optimising anything, build the four
full-collection configurations on the default build and measure them on the
12 benchmarks:

| config | full collection does | exact? |
|---|---|---|
| C0 | classic (baseline) | yes |
| M | mark-alive from roots, classic on the remainder | yes |
| MC | mark-alive, closure pass on the remainder, unknown closures promoted | no |
| MCX | mark-alive, closure pass, classic on the unknown remainder with the pure garbage's references subtracted (section 2.B5) | yes |
| C | closure pass only (current mode 1) | no |

Prediction: M captures most of the 2x on edge-heavy full collections (it also
traverses live objects once); MC is within noise of M; MCX is within noise of
M and exact. If that holds, the proposal to core devs is "mark-alive for
full collections plus an exact, cheap treatment of the remainder", with the
closure pass as the fast pre-filter of the remainder and as a candidate for
PEP 848's cycle-check step. If MC/MCX beat M measurably, the closure pass
carries real weight and the rest of the plan optimises it.

Mark-alive port: ~150 lines from `gc_free_threading.c` without the prefetch
buffer. Roots: `interp->sysdict`, builtins, interp dict, static type dicts
and `tp_subclasses`, thread stacks (`_PyGC_VisitFrameStack`), C stack refs.
"Alive" = clear `PREV_MASK_COLLECTING`, which makes the object "outside S"
for both detectors with no new bit. Cost O(live heap), only in full
collections, where the classic algorithm already pays two traversals of the
live heap.

## 2. Workstreams

### A. Partial / interleaved mark-alive (default build)

- A1. Full-collection mark-alive (above). Required for everything else.
- A2. Stack-only roots for young collections: O(stack depth), marks young
  temporaries held by running frames. Measure gen-0 recall change on sphinx
  and async_tree. Low priority: gen-0 recall is already 93-99%; only worth it
  if the residual blockers are frames/locals.
- A3. Interleaved marking: spread the root scan over the young collections
  between two full collections, so the full collection starts with alive
  bits precomputed. Sound by construction, because an alive bit only ever
  excludes an object from the candidate set (a stale bit delays garbage, it
  never frees a live object). This is the same idea as PEP 848's
  pending/visited spaces; implement only as a comparison point, with a
  budget (objects per young collection) and measure the full-collection
  pause reduction.
- A4. Free-threaded: nothing to build; finish the campaign (12 benchmarks,
  modes 0/1/2/4) once the attribution fix is verified.

### B. Interleaving schemes

- B1. Mode 2 (gen 0/1 closure, full classic): measured flat; keep as control.
- B2. Inverse: young classic, full = mark-alive + closure (+ exact remainder).
  This is the configuration the data points to.
- B3. Adaptive: choose per generation from the previous collection's
  statistics (internal edges per candidate, candidate count). The closure
  pass pays when edges per candidate is high; the classic pass when it is
  low. One comparison per collection, no new state beyond two counters.
- B4. Periodic exactness: every k-th full collection runs the classic
  algorithm on the remainder, bounding the delay of unknown garbage to k
  full collections. Measure peak memory against k.
- B5. Exact remainder (makes any mode "correct in all cases"): after the
  closure pass, run the classic algorithm on the unknown survivors S' with
  one extra pass that subtracts the references the pure garbage P holds into
  S'. Then unreachable(S') equals classic(S) minus P exactly: external counts
  of S' members are refcount minus edges from S' minus edges from P, and
  propagation from externally referenced objects never passes through P.
  Cost: two traversals of S' plus one of P. Pointless without mark-alive
  (S' is almost everything), cheap with it. Gives identical unreachable sets
  and hence identical weakref/finalizer ordering to today.

### C. Making the new code faster

- C1. Inline traversal of list, tuple and dict in the visitor loop (what
  `gc_mark_enqueue_span` does for FT mark-alive): removes the indirect
  `tp_traverse` call and the per-edge callback for the three dominant types.
- C2. Prefetch buffer for the BFS queue (FT's 256-entry ring with prefetch on
  push): the closure pass is a pointer chase; FT's mark-alive got its win
  from this.
- C3. Reuse the queue across collections (keep it in `gcstate`, sized from
  the last high-water mark); currently malloc'd per collection.
- C4. Cheaper "is in S" test: today `_PyObject_IS_GC` loads `tp_flags`
  (another cache line) before the flag check. Same cost as the classic
  visitor, so not a relative loss, but C1 removes most of these loads.
- C5. Profile one sphinx full collection with ETW/VTune sampling (or Linux
  perf on the CI box) to split visitor-call overhead from cache misses before
  spending effort; decide C1/C2 from that.
- Target for whichever configuration survives section 1: deduce phase at
  most 0.6x classic on full collections of live heaps, at most 1.0x in gen 0.

### D. Ordering of objects

- D1. Newest-first cursor (walk `young` from the tail) versus allocation
  order (default build) and heap order (FT). Cheap to test; expect it to
  help instance-before-class structures and hurt container-before-children
  structures. Measure recall per benchmark in truth mode.
- D2. Merging closures (the real fix for ordering): when a closure reaches a
  retired *unknown* closure, merge it (R += R_j, E += E_j + this edge) instead
  of treating it as external. Union-find over closure records; the verdict
  then no longer depends on visit order for garbage DAGs, and the "class
  visited before its instance" case is collected in one go, which should
  make the two test_weakref ordering tests pass in mode 1. Needs a closure
  id per object: `ob_tid` on FT; on the default build the `gc_refs` bits of
  `_gc_prev`, which means garbage is unlinked by a final list-rebuilding
  pass (as `move_unreachable` does) instead of in place. Still not exact
  when a closure contains a live object; that remains B5's job.
- D3. SCC-based (Tarjan) variant: strictly finer than forward closures, one
  DFS, but a per-SCC verdict needs incoming-from-garbage bookkeeping in
  topological order. Evaluate only if D2 leaves measurable recall on the
  table.

### E. Other ideas

- E1. Plug the closure pass into PEP 848's reference implementation as its
  cycle check on the unreached candidates and compare with its current
  check, on the same branch (worktree `cpython-incgc`). This is the most
  direct way to make the work relevant to the ongoing GC discussion.
- E2. Memory: `pyperformance run --track-memory` (peak working set) for every
  configuration; the cost of delayed garbage has not been measured.
- E3. Semantics: write down precisely what changes under unknown closures
  (piecewise collection, callback/finalizer ordering) and show that B5 and
  D2 restore today's behaviour; this is what reviewers will ask first.
- E4. Free-threaded remainder: with mark-alive present, compare "classic on
  remainder" with "closure on remainder" head to head (campaign A4); that is
  the cleanest measurement of the closure pass's own value.

## 3. Benchmarks: 12 instead of 4

Kept: gc_collect, gc_traversal (synthetic GC cost), sphinx (large document
tree, the workload that exposed the 3.13 incremental GC regression),
deltablue (constraint objects with cycles).

Added, all in pyperformance 1.14 as installed here:

| benchmark | why |
|---|---|
| async_tree_memoization | asyncio task/future graphs; the async_tree family was added for GC-heavy asyncio workloads |
| xml_etree | large element trees (parse/iterparse/generate), many tracked dicts and lists |
| docutils | document node trees, smaller cousin of sphinx |
| sqlalchemy_declarative | ORM mapper and instance state with cycles |
| sympy | expression trees plus caches, long-lived heap |
| dask | task graphs; a workload where users tune or disable the GC |
| networkx | dict-of-dict graphs, large live heap |
| mdp | deep nested dicts/lists, object-heavy |

Alternates if one of these turns out GC-insensitive: raytrace, richards, go,
tornado_http. Outside pyperformance: the CPython `Doc/` Sphinx build as a
one-number real-world check (the 3.13 incremental GC regression was first
seen there), and the synthetic suite in `scratch/gc_closure/synth.py` for
the delayed-garbage cost. Every benchmark is run in truth mode once for
recall/precision and in the timing configurations interleaved on one binary,
two rounds, idle machine, plus `--track-memory`.

## 4. Milestones and gates

1. M1, decisive experiment (section 1): mark-alive for full collections,
   B5 exact remainder, configurations C0/M/MC/MCX/C on the 12 benchmarks.
   Gate: where does the speedup come from.
2. M2, ordering and merging (D1, D2): gate: test_weakref and test_gc green
   in the closure-only mode, recall per benchmark reported.
3. M3, speed (C1-C3, C5 first): gate: deduce phase at most 0.6x classic on
   full collections, at most 1.0x in gen 0, pyperformance geometric mean at
   least 1.02x, no benchmark more than 2% slower, peak memory within 5%.
4. M4, write-up: design, correctness argument (sound by trial deletion,
   exact with B5), semantics, numbers, relation to PEP 848, diff size.
   Discourse draft plus the branch.

Throughout: truth mode on every benchmark and the test suite in Debug must
report `pure_but_live == 0` and `phantom_closures == 0` (the FT fix for the
untracked-tuple artifact to be confirmed first).

Effort: M1 2 days, M2 2 days, M3 2 days, M4 1 day; campaigns run unattended.
