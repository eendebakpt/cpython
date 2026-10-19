---
name: feedback-diff-size
description: Prefer minimal diffs to core CPython files when the perf is equivalent
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d2f13ae3-bc99-4fa8-b657-4fbd510b070c
---

When modifying core CPython files (typeobject.c, dictobject.c, bytecodes.c,
ceval.c), prefer **minimal in-place edits** over factoring out helper
functions, when the resulting performance is equivalent.

**Why:** Smaller diffs are easier to review and easier to land. A `static inline`
helper that's only called from one place adds review burden without producing
different machine code -- the compiler inlines it back anyway. Specifically
called out after I refactored `_PyType_AllocNoTrack` into an
`alloc_inline_values_instance()` helper that nearly duplicated the
slow-path body.

**How to apply:**
- First instinct should be to consolidate branches in-place, not to extract.
- Use a helper only when:
  - It's called from 2+ sites, OR
  - The slow and fast paths share <30% of their code, OR
  - The user explicitly asks for one.
- When unsure: write the minimal version first and offer the factored version
  as an alternative.
