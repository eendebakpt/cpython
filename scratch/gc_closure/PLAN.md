# Closure-sum cycle detection for the default-build GC: plan

Written 2026-10-17 against main @ 21a36aa79ca (3.16.0a1+dev).

## 0. What the code actually looks like today

- `Python/gc.c` is the 3-generation collector. `deduce_unreachable()` =
  `update_refs()` (gc_refs = ob_refcnt, set COLLECTING, untrack immortals)
  -> `subtract_refs()` (one tp_traverse per object, decrement children's
  gc_refs) -> `move_unreachable()` (walk list, re-traverse every object with
  gc_refs > 0, relink prev pointers, untrack tuples).
  Live objects are traversed twice, garbage once.
- There is NO "mark alive from roots" phase in the default build. It exists
  only in `gc_free_threading.c` (`gc_mark_alive_from_roots`, behind
  `GC_ENABLE_MARK_ALIVE`). The incremental GC that had a visited-space scheme
  is not on main, 3.13 or 3.14. So step (i) of the proposal is itself new work
  for the default build, and it is O(live heap), i.e. only affordable in full
  (gen 2) collections.
- Bits available during a collection: `_gc_prev` bit 0 FINALIZED, bit 1
  COLLECTING, bits 2+ = gc_refs (this *is* the prev pointer, restored by
  move_unreachable). `_gc_next` bit 0 = UNREACHABLE, valid only between
  move_unreachable and move_legacy_finalizers/clear_unreachable_mask. All
  `gc_list_*` helpers assume `_gc_next` bit 0 is clear.
- Post-conditions the rest of `gc_collect_main` needs from the deduce phase:
  `unreachable` members have COLLECTING set and `_gc_next` bit 0 set
  (move_legacy_finalizers asserts it per object, then clears it);
  survivors in `young` have COLLECTING clear and valid prev pointers;
  `visit_move` / `handle_weakref_callbacks` use `gc_is_collecting` as the
  "is in unreachable" test; `handle_resurrected_objects` re-runs the old
  `deduce_unreachable` on the unreachable set (keep that as is).
- `_PyGC_VisitStackRef` skips borrowed stackrefs ONLY when
  `visit == visit_decref`. Any new counting visitor needs the same exemption,
  otherwise it counts edges that carry no refcount -> false "garbage".
- `update_refs` untracks immortal objects, so they never enter the candidate
  set: this already implements "if immortal, do not add".
- `_PyObjectStack` (pycore_object_stack.h) works in the default build; a plain
  growable array is simpler for what we need (see 3.2).
- 258 distinct `*_traverse` functions in Objects/Modules/Python; slot id
  `Py_tp_traverse = 71` lives in generated `Include/slots_generated.h`;
  PyTypeObject currently ends with tp_vectorcall, tp_watched,
  tp_versions_used.

## 1. The algorithm, stated precisely

Let S be the candidate set (the merged `young` list; COLLECTING set on every
member). For a start object x in S, the closure C(x) is every object of S
reachable from x via tp_traverse edges that stay inside S. Edges that leave S
are neither counted nor followed. Compute over the closure:

    R = sum of Py_REFCNT(o)                for o in C
    E = number of tp_traverse visits o->t  with o in C and t in S
        (t in S implies t in C, because C is closed)

R - E is the number of references into C that do not come from C: references
from older generations, from the C stack, from untracked or non-GC holders,
and edges a tp_traverse forgot to report. Verdict: C is garbage iff R == E.

Properties:

- Soundness rests on the same premise as `subtract_refs`: tp_traverse reports
  exactly the owned strong references. An over-reporting traverse (visiting a
  borrowed ref) can give R == E for a live closure; today the same bug gives a
  gc_refs underflow ("refcount is too small" assert in debug builds). The sum
  loses locality: a phantom edge anywhere in the closure can cancel an
  external reference anywhere else. Under-reporting only leaks, as today.
- It is trial deletion (Bacon-Rajan) with one aggregate counter instead of
  per-object counters. The per-object variant is precisely
  subtract_refs + move_unreachable.
- Granularity: C(x) is a forward closure, not a strongly connected component.
  If x is garbage but C(x) contains one live member of S, the entire closure
  is "unknown" and is kept (promoted). If x is live, C(x) is all live and the
  "unknown" verdict is exact; this case costs one traversal per member
  instead of two today, which is where the speed-up has to come from.
- Generations already play the role of mark-alive for gen 0/1 collections:
  instance->class and instance->module-dict edges leave S (the class and the
  module dict are old), so they neither count nor contaminate. Mark-alive
  only matters for full collections, where S is the whole heap.
- Expected failure modes (to be measured, not assumed): garbage that
  references a live *young* object (a temporary that was also stored in a
  live container), cycles through frames/tracebacks that reference frames of
  running code, any closure with a missing tp_traverse edge.

Cost model per young collection: old = 1 list walk + 2 traversals of every
live object + 1 of every garbage object + prev-pointer relinking; new = 1
traversal per object + one queue push/pop and one bit set/clear per object.
The hypothesis is roughly half the traversal work, paid for by delayed
collection of garbage that sits in impure closures.

## 2. tp_traverse2: not in phase 1

E is, by definition, what tp_traverse reports: a counting visitproc gives the
same number and the same children. A second slot costs a PyTypeObject field
(appending is ABI-compatible, cf. tp_watched), a Py_tp_traverse2 slot id, a
HAVE_TRAVERSE2 flag with a tp_traverse fallback for every third-party type,
258 in-tree implementations, docs and a C API WG round. None of that is
needed to find out whether the idea works. Assumption: tp_traverse2 is meant
as a *faster* edge enumerator (no per-edge indirect call), not a different
edge set. If it is meant to be a different edge set (e.g. including non-GC
referents), say so; that changes the formula, since non-GC objects' refcounts
are irrelevant to cycles. Whether the per-edge callback is the bottleneck is
a phase-4 measurement; the cheaper fix with the same effect is inline
specialised traversal of list/tuple/dict in the visitor loop, which the
free-threaded build already does (`gc_mark_enqueue_span`).

## 3. Implementation (branch `gc-closure-sum`, all in gc.c + a gcmodule.c hook)

### 3.1 Scaffolding
- `GCState` gets `closure_mode` and `closure_stats`, read in `_PyGC_Init`
  from env vars `PYTHON_GC_CLOSURE` (0 old, 1 new for all gens, 2 new for
  gen 0/1 and old for gen 2, 3 alternate per collection) and
  `PYTHON_GC_CLOSURE_STATS=1`. Prototype only; no config plumbing.
- Private `gc._closure_stats()` returning a dict of counters, and a dump to
  stderr in `_PyGC_Fini` when the stats env var is set, so `-m test` runs and
  arbitrary scripts produce data without modification.

### 3.2 The pass: `deduce_unreachable_closure(young, unreachable, &reachable)`
Production path (no gc_refs storage, prev pointers stay valid):

1. Set COLLECTING on every member of `young` and untrack immortals. This is
   `update_refs` minus the gc_refs store; factor a flag out of it.
2. Worklist = growable `PyObject **queue` (PyMem_Malloc, doubling), reused
   across closures. BFS, so the array is both the worklist (head index) and
   the member list of the current closure. No recursion.
3. Loop: while `young` is non-empty, x = GC_NEXT(young). Discover(x): set
   `_gc_next` bit 0 ("discovered"), append, R += Py_REFCNT(x). Process
   queue[h++]: `tp_traverse(op, visit_closure, &state)`.
   `visit_closure(t)`: not GC -> return; `!COLLECTING` -> return (outside S:
   not counted, not followed); E++; already discovered -> return; Discover(t).
4. Closure end: verdict = (R == E). Walk the queue array: clear bit 0, then
   `gc_list_move` into `unreachable` (verdict) or a local `survivors` list
   (clear COLLECTING, `_PyTuple_MaybeUntrack` exact tuples instead of moving
   them, count `reachable`). gc_list_move clobbers prev's `_gc_next`, which
   may be a later member's discovered bit; harmless because the bit is only
   consulted during the BFS, which has finished. The list heads never carry
   the bit.
5. After the loop: merge `survivors` back into `young`; set `_gc_next` bit 0
   on every `unreachable` member (what move_legacy_finalizers expects).
   Post-conditions now match `deduce_unreachable`, so the rest of
   `gc_collect_main` is untouched.
6. OOM on queue growth: clear discovered bits via the queue, merge survivors
   and unreachable back into young, clear COLLECTING, fall back to the old
   `deduce_unreachable`. Record the fallback in stats.
7. `_PyGC_VisitStackRef`: extend the `visit == visit_decref` test to also
   match `visit_closure`.
8. Debug asserts: R >= E per closure (E > R means a phantom edge);
   `validate_list(young, collecting_clear_unreachable_clear)` after the pass.

Stats path (`closure_stats=1`, runs instead of the production path):

- `update_refs` as today (gc_refs stored, so no list moves are possible,
  and none are needed). BFS as above but the visitor also does
  `gc_decref(t)`. Because edges into C come only from C, each member's
  gc_refs is final when its closure finishes: for impure closures, scan the
  members and histogram the type names of those with gc_refs > 0 (the
  "blockers"). Record per closure: size, R, E, verdict, start type.
  Members are kept in an arena (closure boundaries as offsets).
- Then `move_unreachable(young, unreachable)` runs unchanged as ground truth
  (the BFS decrements are exactly what subtract_refs would have done).
- Attribute truth to closures: per closure, count members with COLLECTING
  still set (= in unreachable). Invariants to assert: a pure closure is
  entirely in unreachable; an impure closure has at least one member with
  gc_refs > 0. "Missed garbage" = unreachable members of impure closures.
- Counters: per generation: collections, candidates, closures, pure
  closures, objects in pure closures, true garbage, missed garbage,
  blocker-type histogram, closure-size log2 histogram, queue high-water,
  traversals new (= |S|) vs old (|S| + reachable), object_visits if built
  with Py_STATS (`build.bat --pystats` is supported on Windows).

### 3.3 Workloads and harness (scratch/gc_closure/)
- `synth.py`: parametrised generators, each run with explicit
  `gc.collect(g)` for g in 0..2 and with automatic collection:
  self-cycles (`l.append(l)`), 2-object cycles, instance cycles via
  `__dict__` (A<->B, classes old), the failure case (instance cycle that also
  references a young object stored in a live list), exception/traceback
  cycles (`except E as e: self.e = e`), function<->cell cycles, a large live
  young structure (10^5 dicts) for the live-majority cost case.
- Real workloads: `-m test test_gc test_asyncio test_json test_decimal
  test_email test_dataclasses test_typing test_unittest` with the stats
  dump; the existing `scratch/incgc_bench.py` + `incgc_bench_run.py`
  (per-process interleaved runner) for timing; pyperf 2.10 is available on
  the external 3.14.
- `analyze.py`: parse dumps into one table per generation: recall
  (garbage in pure closures / true garbage), missed garbage by blocker type,
  closure size p50/p99/max, traversal counts new vs old.

### 3.4 Performance
- Single-binary A/B (runtime flag, interleaved in-process; same rules as
  scratch benches so far): modes 0 vs 1 vs 2 on the same build.
- Metrics: deduce-phase time (add a `PyTime_PerfCounterRaw` pair around the
  deduce call, aggregated per mode), total GC time from `gc.get_stats()`
  duration, workload wall time via pyperf, peak working set via ctypes
  `GetProcessMemoryInfo` to price the delayed garbage.

### 3.5 Decision gates
- Gate A (stats): recall of pure-closure garbage in gen 0/1 on the real
  workloads, and what blocks the rest. If one blocker class dominates (e.g.
  frames), evaluate a cheap targeted root pass (thread stacks only, O(stack
  depth), as the free-threaded `GC_MARK_ALIVE_STACKS` does) before anything
  heap-wide.
- Gate B (perf): deduce-phase time new/old on live-majority workloads. If the
  traversal halving does not show up here, the idea does not pay regardless
  of recall.
- Phase 3 (only if A and B pass): mark-alive for gen 2 collections, ported
  from gc_free_threading.c without the prefetch machinery. No new bit is
  needed: "alive" = clear COLLECTING, which makes the object "outside S" for
  the closure pass by construction. Roots: sysdict, builtins, interp dict,
  type dicts, thread stacks.
- Phase 4: profile the visitor loop; inline list/tuple/dict traversal;
  only then decide about a tp_traverse2 slot.

### 3.6 Risks and non-goals
- Free-threaded build untouched in phase 1 (its deduce works over mimalloc
  heap walks; the closure idea would replace `deduce_unreachable_heap`, a
  separate project).
- gc.freeze / permanent generation, weakref handling, legacy finalizers,
  resurrection: unchanged by construction (same post-conditions).
- Queue memory is O(largest closure); a live young tree can make that
  |S| pointers (8 MB per million objects, transient). Record high-water.
- Interleaving (mode 2/3) is sound because "unknown" closures are promoted
  exactly as the old algorithm promotes reachable objects; the only cost is
  garbage living until a collection that runs the old algorithm.

## 4. Order of work
1. Scaffolding + production pass + debug asserts, Release and Debug builds,
   test_gc green in modes 0..3.
2. Stats path + synth.py + analyze.py; verify invariants on synth cases.
3. Stats campaign on the real workloads; write up Gate A.
4. Perf A/B; write up Gate B.
5. Decide on phase 3/4.
