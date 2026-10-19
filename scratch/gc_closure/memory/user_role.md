---
name: user-role
description: User is a CPython core contributor working on performance optimization
metadata: 
  node_type: memory
  type: user
  originSessionId: d2f13ae3-bc99-4fa8-b657-4fbd510b070c
---

Pieter Eendebak is a CPython core contributor (pieter.eendebak@gmail.com) working
on performance optimization in CPython 3.16-dev. Comfortable at every level of the
interpreter -- bytecode/specialize.c, typeobject.c, dictobject.c, the tier-2 JIT,
the optimizer, the case generators. Has authored PRs in this area.

Works on Windows 11 with MSVC. Builds CPython via PCbuild, not autotools.
Frequently runs pyperformance benchmarks for validation.

Communication: terse, technical, no hand-holding required. Will push back on
weak measurements or premature conclusions. Wants me to verify findings before
acting on them (e.g. caught a measurement artifact that motivated A3, ended up
skipping A3 entirely).
