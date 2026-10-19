---
name: feedback-commit-msg-no-cross-refs
description: "Never reference ANY PR or issue (gh-NNNN, #NNNN, URLs) in git commit messages, not even the commit's own issue; the gh-NNNN prefix belongs in the PR title only"
metadata:
  node_type: memory
  type: feedback
  originSessionId: acb0445c-5a87-41e0-ae76-8160e32674c4
  modified: 2026-10-15T14:05:51.999Z
---

Do not reference PRs or issues in commit messages at all (user instruction 2026-10-06, made
absolute on 2026-10-15: "do not reference PRs and issues in commit messages, otherwise those
will get too many pings"). This includes the commit's OWN issue: branch commits titled
`gh-87613: Use AC vectorcall for zip()` (2026-10-15) were the trigger; every push of such a
commit adds a "referenced this issue" event to gh-87613's timeline.

**Why:** GitHub adds a cross-reference event to the timeline of every issue/PR mentioned in a
commit, and repeats it each time the commit is pushed, rebased or cherry-picked.

**How to apply:** commit title = plain summary ("Use AC vectorcall for zip()"); describe
related work in words, never by number or URL. Put the `gh-NNNNN:` prefix in the PR TITLE
(CPython requires it there; the squash-merge commit gets it once) and cross-references in the
PR description. Never force-push to fix an already-pushed message ([[feedback-no-force-push]]);
the squash merge replaces it anyway. Related: [[feedback-perf-pr-structure]].
