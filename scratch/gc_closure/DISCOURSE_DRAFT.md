# Draft: discuss.python.org post (category: Core Development)

Title: Experiment: a one-pass, flag-only cycle detector for young collections

---

I prototyped a second way for the cyclic GC to find unreachable objects and
measured it on main and on the PEP 848 reference implementation. Short
version first, details in the folded sections.

**What it is.** Starting from a candidate, collect the closure (everything
in the candidate set reachable from it through `tp_traverse`), sum the
members' refcounts (R) and count the `tp_traverse` edges that stay inside
the candidate set (E). R − E is the number of references into the closure
from outside. If it is zero the closure is garbage; otherwise the closure is
left alone and a later collection with the existing algorithm decides it.
One traversal per object, no per-object counters, no list relinking, no
object state changed, same soundness assumption as today. It is a sound
heuristic, not a replacement.

**What is clear.**

- On young collections it is faster: generation 0 costs 0.61-0.89x of the
  existing algorithm on the eight pyperformance benchmarks that trigger
  young collections, generation 1 0.73-1.16x (main, in-process A/B, see
  below). On PEP 848's aging-space collections it is 0.71-0.81x where the
  aging space is large and live, flat where it is mostly garbage or tiny.
- It finds 87-100% of the garbage in young collections on its own; the
  rest is found by the next older collection. With the existing algorithm
  kept for full collections, `gc.collect()` is exact and the test suites
  pass. Branch: <https://github.com/eendebakpt/cpython/tree/gc-closure-young>.
- In full collections it only works together with a mark-alive pass and a
  deferred verdict; that configuration is 1.3-1.7x faster where full
  collections of a large live heap dominate (gc_traversal, gc_collect) and
  loses on heaps where the breadth-first order defeats the cache. Experiment
  branch: <https://github.com/eendebakpt/cpython/tree/gc-closure-sum>.

**What is not (yet) compelling: total run time.** On pyperformance the GC
is a small part of the run time, and young collections a smaller one.
Where the time goes on main (time inside automatic collections during a
60 s run of each workload, measured with `gc.callbacks`):

| benchmark | time in GC, % of run time | of which young collections (gen 0 and 1), % of run time | of which full collections, % of run time | class |
|---|---|---|---|---|
| async_tree_memoization | 18.8% | 9.8% | 9.0% | GC-relevant |
| sphinx | 11.4% | 2.8% | 8.6% | GC-relevant |
| docutils | 10.0% | 4.3% | 5.7% | GC-relevant |
| networkx | 4.8% | 2.2% | 2.6% | compute-dominated |
| xml_etree | 4.5% | 4.0% | 0.5% | compute-dominated |
| deltablue | 4.4% | 1.3% | 3.1% | compute-dominated |
| sympy | 0.8% | 0.6% | 0.2% | compute-dominated |
| mdp | 0.6% | 0.5% | 0.1% | compute-dominated |
| raytrace, richards | 0.0% | 0.0% (no young collections) | 0.0% | compute-dominated |
| gc_collect, gc_traversal | ~100% | 0% | ~100% (explicit `gc.collect()`) | GC-only microbenchmark |

Run-time comparison, relative to main (lower is better; TBD = campaign
planned, see below):

| benchmark | main + closure pass | PEP 848 | PEP 848 + closure pass | young-collection GC time, main + closure pass vs main (measured) |
|---|---|---|---|---|
| async_tree_memoization | TBD | TBD | TBD | 0.74x |
| sphinx | TBD | TBD | TBD | 0.82x |
| docutils | TBD | TBD | TBD | 0.89x |
| networkx | TBD | TBD | TBD | 0.67x |
| xml_etree | TBD | TBD | TBD | 0.73x |
| deltablue | TBD | TBD | TBD | 0.61x |
| sympy | TBD | TBD | TBD | 0.86x |
| mdp | TBD | TBD | TBD | 0.83x |
| raytrace, richards | TBD | TBD | TBD | no young collections |
| gc_collect | TBD | TBD | TBD | 1.00x (full collections only) |
| gc_traversal | TBD | TBD | TBD | 1.00x (full collections only) |

So far the honest statement is: a 10-40% cheaper young collection, which
on these benchmarks can amount to at most a 1-2% run-time win
(async_tree), and a 1.3-1.7x win on the GC microbenchmarks only with the
all-generations configuration. The run-time columns will come from one
campaign: two binaries on the same base commit (main + closure pass, and
PEP 848 + closure pass, each with the pass switchable at runtime so that
each binary is its own control), four configurations interleaved, three
rounds overnight on an idle machine, accepted only if the three baseline
rounds agree within 3%. To make the case either way beyond pyperformance I
also need workloads where the GC is 10% or more of run time; candidates to
instrument first (GC share via `gc.callbacks`, no A/B): mypy on its own
sources, pytest collecting the CPython test suite, the full CPython docs
build, black on a large tree, the Django test suite, and pyperformance's
sqlalchemy and dask once they install on 3.16. Pointers to known GC-bound
applications are welcome.

**Questions.**

1. Is a heuristic detector that defers some garbage to a later collection
   acceptable, given the ordering effects on weakref callbacks and
   finalizers, or only an exact form (closure pass first, existing algorithm
   on the unknown remainder)?
2. Given the numbers, is a young-collection-only change worth a PR on its
   own, or only as part of the PEP 848 work?
3. Which workloads would convince you?

[details="The detector"]

Let S be the candidate set (the generation being collected, with younger
generations merged). Starting from a candidate x, collect the closure C(x):
everything in S reachable from x through `tp_traverse` edges that stay
inside S; edges leaving S are neither counted nor followed. Sum two numbers
over the closure:

- R = sum of `ob_refcnt` over the members,
- E = number of `tp_traverse` visits from a member to a member of S.

Every reference counted in E comes from inside C, so R − E is the number of
references into C from anywhere else. If R == E the closure is unreachable
and goes to the unreachable list; otherwise it is "unknown", its members are
retired in place and treated as survivors, exactly as the existing algorithm
treats reachable objects. The pass is breadth-first with an explicit queue;
the per-object state is two existing flag bits (`PREV_MASK_COLLECTING`, the
low bit of `_gc_next`). The per-object version of the same count is what
`deduce_unreachable()` computes in three passes (`update_refs` overwrites
every candidate's `_gc_prev` with its refcount, `subtract_refs` traverses
every candidate, `move_unreachable` traverses every reachable candidate
again while rebuilding the lists). Live objects, the vast majority, are
traversed twice there and their GC header rewritten twice; here once and
not at all.

Soundness rests on the same assumption as today: `tp_traverse` reports
exactly the owned references. An over-reporting traverse (a visited borrowed
reference) can make a live closure look pure, in the same way it makes
`gc_refs` underflow today; the debug build asserts in both cases. Deferred
stack references get the same exemption as in `visit_decref`.

The limitation is granularity: a closure is a forward-reachable set, not a
strongly connected component. If x is garbage but C(x) contains one live
object, the whole closure is unknown and the garbage waits for a later
collection that runs the existing algorithm. A garbage class and its garbage
instance can also be collected in two successive collections (the class is
older, is visited first, looks externally referenced because of the
instance, and is retired; the instance is then collected alone). Two tests
in test_weakref that assume one unreachable set per garbage structure fail
in the "closure pass only" mode for that reason; the interleaved mode
passes everything. No new type slot is needed: the edge count is the number
of `visit` calls `tp_traverse` makes.

[/details]

[details="Branches"]

- `gc-closure-young` (<https://github.com/eendebakpt/cpython/tree/gc-closure-young>):
  the proposal. One commit, about 280 lines in `gc.c` plus tests and an
  InternalDocs section. Generations 0 and 1 use the closure pass, full
  collections keep `deduce_unreachable()`, so every closure the pass cannot
  decide is resolved by the next full collection and `gc.collect()`
  semantics are unchanged. test_gc, test_weakref, test_finalization and
  test_asyncio.test_tasks pass. `PYTHON_GC_CLASSIC=1` (a `gc.set_debug()`
  bit) is a temporary A/B switch.
- `gc-closure-pep848` (<https://github.com/eendebakpt/cpython/tree/gc-closure-pep848>):
  the same pass on top of the PEP 848 reference implementation
  (markshannon/cpython `incremental-gc`). `gc_collect_region()` takes a
  `young` flag; nursery and aging-space collections use the closure pass,
  old increments and full collections keep `deduce_unreachable()`. Tests
  pass with the pass on and off, incremental and `-X gc=legacy`.
- `gc-closure-sum` (<https://github.com/eendebakpt/cpython/tree/gc-closure-sum>):
  the experiment branch, default and free-threaded builds, with every
  variant tried (marking before full collections, deferred verdicts,
  adaptive policy, a measurement mode that runs both detectors in the same
  collection and records recall, the types of live objects that hid garbage,
  and time per detector). Environment variables and private functions, no
  NEWS.

[/details]

[details="Measurement method"]

Wall-time A/B runs of separate processes were not usable on my machine:
identical control runs differed by up to 2x under background load, and
pyperf `--fast` runs trigger only a handful of young collections per
worker on the smaller benchmarks. Instead each pyperformance workload runs
in one process for 60 s with the detector switched between the existing
algorithm and the closure pass after every young collection, in a balanced
non-periodic sequence. Both detectors see the same workload phases and the
same machine load; the time of every collection comes from `gc.callbacks`,
the candidate counts from `gc.get_stats()`. Old increments (PEP 848) and
full collections (main), which use the existing algorithm in both modes,
act as the noise control. Driver and raw records are in the experiment
branch's `scratch` notes; happy to share.

[/details]

[details="Results: young collections on main"]

| benchmark | gen 0 collections (classic / closure) | gen 0 time, closure / classic | gen 1 time, closure / classic |
|---|---|---|---|
| deltablue | 1765 / 1755 | 0.61x | 0.73x |
| networkx | 851 / 823 | 0.67x | 0.82x |
| xml_etree | 6145 / 6107 | 0.73x | 0.82x |
| async_tree_memoization | 11774 / 11739 | 0.74x | 0.81x |
| sphinx | 2013 / 1984 | 0.82x | 1.09x |
| mdp | 520 / 516 | 0.83x | 0.89x |
| sympy | 372 / 363 | 0.86x | 1.16x |
| docutils | 3986 / 3951 | 0.89x | 0.97x |

Generation 0 is 11-39% cheaper on all eight; generation 1, whose
collections are rarer and whose candidate sets hold the survivors of
several gen-0 passes, is cheaper on six and within noise on two. raytrace
and richards allocate too few container objects to trigger a young
collection in 60 s; gc_collect and gc_traversal only run explicit full
collections. sqlalchemy_declarative and dask could not be installed on this
3.16 build (greenlet and msgpack do not compile yet).

Recall (measurement mode; objects the closure pass found on its own over
objects the existing algorithm found unreachable in the same collection):
87-100% in generation 0 and 89-100% in generation 1 across twelve
benchmarks, e.g. sphinx 99%/93%, deltablue 99%/100%, async_tree 87%/100%.
A miss is a cycle that references a live object of the same generation (a
temporary also stored in a live container, a frame local created after the
cycle); the next older collection gets it. In full collections recall drops
to 20-30%: the whole heap is the candidate set, garbage references live
class and module state, and interpreter shutdown contributes most of that
garbage. That is why the proposal keeps the existing algorithm there.

[/details]

[details="Results: on top of PEP 848"]

Under PEP 848 a young collection is one aging space, i.e. objects that
survived five nursery rotations, so its candidate set is mostly live and
the garbage is largely found by the old increments. Same in-process method,
60 s per benchmark, old increments (existing algorithm in both modes) as
the noise control:

| benchmark | young collections | candidates per young collection | young time, closure / classic | old-increment time (control) | young share of GC time |
|---|---|---|---|---|---|
| deltablue | 158 | 24,081 | 0.71x | 1.09x | 18% |
| mdp | 1599 | 925 | 0.76x | 1.04x | 42% |
| networkx | 132 | 4,873 | 0.77x | 1.27x | 75% |
| async_tree_memoization | 1187 | 19,279 | 0.81x | 0.99x | 53% |
| sympy | 689 | 911 | 0.95x | 0.79x | 69% |
| docutils | 713 | 15,179 | 0.97x | 1.04x | 60% |
| sphinx | 1055 | 4,752 | 1.00x | 1.04x | 54% |
| xml_etree | 895 | 498 | 1.03x | 0.90x | 47% |
| raytrace | 912 | 4 | 1.11x | 1.01x | 58% |

A 19-29% saving where the aging space is large and live, flat where it is
mostly garbage (sphinx, docutils: the garbage handling dominates) or has a
few hundred candidates (xml_etree, sympy), and a 10% loss on raytrace,
where a young collection has four candidates and the worklist allocation is
the whole collection (a fallback to the existing algorithm below a small
candidate count would remove that; not done yet).

[/details]

[details="Results: full collections (experiment branch)"]

Using the closure pass for full collections needs two additions: a
mark-alive pass from the roots before the full collection (as the
free-threaded collector has) so that live class and module state leaves
the candidate set, and a deferred verdict that judges closures in a backward
sweep over the DAG of closure records, so that a garbage structure is found
whole whichever of its parts is visited first. With both, the closure pass
used for every collection passes test_gc, test_weakref and
test_finalization on both builds, and full-collection recall rises to
sphinx 99%, gc_collect 98%, deltablue 83%. Timing against the existing
algorithm (interleaved pyperf runs on an idle machine):

| benchmark | wall time | GC-phase time |
|---|---|---|
| gc_traversal | 0.58x | 0.64x per collection |
| gc_collect | 0.77x | 0.78x |
| sphinx | 0.98x | 0.94x |
| xml_etree | 1.01x | 0.90x |
| async_tree_memoization | 1.00x | 1.05x |
| deltablue | 1.00x | 1.04x |

It wins where full collections of a large live heap dominate and loses up
to 1.5x per full collection on docutils and networkx: the breadth-first
passes visit objects in discovery order while the existing passes walk the
list in allocation order, and every header becomes a cache miss. The same
applies to a mark-alive pass on its own (mark + existing algorithm:
gc_traversal 0.57x, gc_collect 0.79x, sphinx 1.16x wall), which is why
marking is not simply always on in the default build; the free-threaded
collector hides the misses with a prefetch buffer, and that is the next
thing to measure here. On the free-threaded build, whose collector already
marks the live heap once, wall time is flat within noise; the pass there is
an order-robust way to compute the remainder, with 100% gen-0 recall.

[/details]

[details="Prior work"]

The idea is not new, only its use here is. The per-object version of the
count is trial deletion, which CPython runs today. The single-traversal,
per-subgraph variant, i.e. this pass including its incompleteness, was
published by Chin-Yang Lin and Ting-Wei Hou:

- ["A Lightweight Cyclic Reference Counting Algorithm"](https://link.springer.com/chapter/10.1007/11745693_35),
  GPC 2006, LNCS 3947, pp. 346-359.
- ["A simple and efficient algorithm for cycle collection"](https://dl.acm.org/doi/10.1145/1273039.1273041),
  ACM SIGPLAN Notices 42(3), 2007: a sub-graph instead of individual cycles
  as the unit of collection, garbage cycles found in one trace,
  completeness not guaranteed, about 6% better application performance
  than a trial-deletion collector on Jikes RVM / SPECjvm98.
- ["An efficient approach to cyclic reference counting based on a
  coarse-grained search"](https://www.sciencedirect.com/science/article/abs/pii/S0020019010003108),
  Information Processing Letters, 2010.

What is new here is the fit with CPython's generations (the generation
boundary supplies the external references that make young closures
decidable), the deferred-verdict sweep over the record DAG, and the
measurements.

Related but different: M. J. Parkinson, S. Clebsch, T. Wrigstad,
["Reference Counting Deeply Immutable Data Structures with Cycles"](https://www.microsoft.com/en-us/research/publication/reference-counting-deeply-immutable-data-structures-with-cycles-an-intellectual-abstract/),
ISMM 2024, keep one reference count per strongly connected component of a
frozen graph, computed once at freeze time (Tarjan plus union-find) and
maintained incrementally; it works because immutability fixes the SCCs.
Same identity (external references = refcounts minus internal edges), but a
persistent representation for immutable data rather than a transient
detection pass over a mutable heap.

[/details]

---
Notes to self before posting (2026-10-19 12:30): Discourse folds
`[details="Title"] ... [/details]` (also `<details><summary>`); tables
inside need blank lines around them. Numbers: young-collection tables and
the GC-share table are from the in-process alternating runs
(results_pep848/inproc_main.jsonl, inproc*.jsonl; pep848/ab_inproc.py); the
full-collection table and the mark-only numbers are from the q3 interleaved
pyperf run. The pyperf campaigns on the PEP 848 branch (pep_*, pep2_*) are
invalid (machine load): do not quote. Still to do before posting: GC-share
instrumentation of the candidate real workloads (mypy, pytest collection,
full docs build, black, Django), the small-candidate fallback, peak memory,
FT table, link to the InternalDocs section.
