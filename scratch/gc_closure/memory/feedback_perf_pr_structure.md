---
name: feedback-perf-pr-structure
description: Required structure for every performance-related PR message
metadata:
  type: feedback
---

ALWAYS use this structure for performance-related PR messages:

1. **Short prose** — what changed and why it is faster. Be VERY concise on
   implementation details; the diff already shows them. A few sentences, no
   code walkthrough, no `## Mechanism` section.
2. **One short correctness paragraph** — checksum / test results, inline, not
   its own section.
3. **`## Benchmarks`** — the results table in the message BODY, visible. Include
   control rows (code the patch cannot reach) so the noise floor is legible.
4. **`<details><summary>Benchmark script</summary>`** — the script verbatim,
   folded away. ONLY the script goes in here, not the discussion.

**Why:** the user asked for exactly this after I produced a long PR draft heavy
on mechanism explanation with the benchmarks buried inside `<details>`.
Reviewers want the numbers up front and the script available but out of the way.

**How to apply:** target ~4-5 KB, not 7-10 KB. Do not add sections for asserts,
trade-offs, or design rationale unless the change genuinely needs them. See
[[feedback-benchmarking]] for HOW to measure and
[[project-pidigits-longobject]] for worked examples.
