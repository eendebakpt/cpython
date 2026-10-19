---
name: feedback-single-binary-ab
description: "For perf A/B on this machine, build all variants into ONE binary behind a runtime flag and interleave in-process"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1567eb9d-cdf5-41b3-92cf-0223ca82f253
  modified: 2026-09-21T21:59:50.105Z
---

When benchmarking C-level optimisations, prefer building baseline + all variants into a
single binary behind a runtime flag word, and interleave the variants inside one process
(ABBA, ~15 ms samples), across a few fresh processes. Report the median of paired off/on
ratios with a p10..p90 band; controls use the all-bits mask and must read 1.00.

**Why:** the user asked for this (2026-09-21) to beat the noisy desktop and binary-layout
noise. It worked dramatically: controls went from ±7% (two binaries, interleaved pyperf,
see [[feedback-benchmarking]]) to ±0.5%, and it exposed effects the two-binary run hid
(a mislabelled "control" that was really 1.24x faster; a 70x pathology in one prototype).

**How to apply:** prototype-only plumbing: a global `int` flag word in the .c file, macros
per bit, a METH_STATIC setter on a builtin type (no clinic needed), `extern int` in other
files (codegen can be gated too: compile the statement under each flag). For whole-program
runs (pyperformance scripts) set the flags per process from an env var via a
`sitecustomize.py` on PYTHONPATH and pass `--inherit-environ`. Template harness:
`scratch/bench_dict_single.py`, `scratch/run_pyperformance_flags.py`. Strip the flag
plumbing before any PR; final PR numbers still want a plain two-build pyperf run.
