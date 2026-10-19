---
name: no-force-push-cpython
description: "Never force-push cpython branches, even the user's own PR branches"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 25789ab9-3048-407e-98e2-f8b6f0109e9f
  modified: 2026-08-05T10:02:41.196Z
---

Do not `git push -f` / `--force` any cpython branch (including `pte` fork PR branches).

**Why:** User instruction (2026-08-05) after I amended a merge commit and force-pushed to retrigger CI on gh-150639. Force-pushing rewrites PR history, breaks review anchors, and is against CPython workflow conventions.

**How to apply:** To retrigger CI or fix a branch, use forward-only pushes: new commits, `--allow-empty` commits, or a new merge commit. If history rewrite seems truly required, stop and ask first.
