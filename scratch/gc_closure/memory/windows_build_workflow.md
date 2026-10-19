---
name: windows-build-workflow
description: How to build and regenerate cases on this Windows / MSVC CPython checkout
metadata: 
  node_type: memory
  type: reference
  originSessionId: d2f13ae3-bc99-4fa8-b657-4fbd510b070c
  modified: 2026-09-15T13:11:48.354Z
---

CPython checkout at `c:\develop\cpython`. Build is via MSBuild + PCbuild, not autotools.

## Build command

Call msbuild directly (build.bat's regen step is unreliable). Two overrides
are REQUIRED on this machine as of 2026-05:

- `/p:PlatformToolset=v145` -- VS18 BuildTools ships only the v145 toolset;
  the projects default to v143, which is not installed (error MSB8020).
- `/p:PythonForBuild=<external python>` -- the regen step runs scripts that
  `import json`; if PythonForBuild is the in-tree `PCbuild\amd64\python.exe`
  it imports the *source* `Lib/json`, which may need a newer `_json` than the
  built binary has -> AttributeError. Use an EXTERNAL, self-consistent Python
  (it imports its own stdlib). Also, the in-tree python.exe may be a different
  CPython minor version than the branch (see below).

```powershell
$python = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\python.exe"  # external 3.14
$mb = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\msbuild.exe"
& $mb PCbuild\python.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 `
    /p:PythonForBuild="$python" /p:PlatformToolset=v145 /p:UseTIER2=4 /v:m /nologo /m
```

Build `python.vcxproj` (not just pythoncore) so `python.exe` itself is
relinked -- otherwise a stale `python.exe` from another branch/version loads
the wrong `python3XX.dll`. Check `python.exe -VV` matches the branch.

`UseTIER2=4` enables the tier-2 interpreter (required for `_opcode.get_executor`
and most test_opt assertions to work).

`UseTIER2` is NOT the JIT. The real copy-and-patch JIT (`_Py_JIT`, machine-code
stencils in `jit_stencils-*.h`) is gated on `UseJIT=true` in
`PCbuild/pythoncore.vcxproj` (~line 108), not on `UseTIER2`. A `UseTIER2=4`
build runs the tier-2 *interpreter* with NO stencils generated and NO machine
code emitted. To get a genuine JIT build: `/p:UseJIT=true /p:UseTIER2=1
/p:IncludeLLVM=true` (produces `jit_stencils-x86_64-pc-windows-msvc.h`, ~2.3MB).
TRAP: `sys._jit.is_available()` / `is_enabled()` returns True for ANY `_Py_TIER2`
build (`Python/sysmodule.c:~4228`) -- it reports tier-2, NOT a real JIT. So a
"JIT is on" claim plus a 0xC0000005 on a `UseTIER2=4` build is NOT a JIT-stencil
bug (no stencils exist); suspect the stale-frozen-modules/pyc artifact instead.
Verified 2026-06: a reported "_LOAD_DEREF JIT crash" on `UseTIER2=4` could not
be reproduced on a real `UseJIT=true` build -- it was a build-cache artifact.
Also note: the optimizer test module is `test_optimizer`; the JIT/tier-2
assertions live in `Lib/test/test_capi/test_opt.py` (run via `test_capi`).

## Regenerating after bytecodes.c changes

The case generators must be invoked manually -- build.bat won't (see above).
The authoritative generator->output mapping is in `Makefile.pre.in` (the
`regen-*` targets). After editing `Python/bytecodes.c` or
`Python/optimizer_bytecodes.c`, run each generator with `-o <output>` and
`Python\bytecodes.c` as the positional input:

- `opcode_id_generator.py`      -> `Include\opcode_ids.h`  (NOTE: writes the
  PUBLIC header directly; there is NO `pycore_opcode_ids.h`)
- `target_generator.py`         -> `Python\opcode_targets.h`
- `uop_id_generator.py`         -> `Include\internal\pycore_uop_ids.h`
- `py_metadata_generator.py`    -> `Lib\_opcode_metadata.py`
- `tier1_generator.py`          -> `Python\generated_cases.c.h`
- `record_function_generator.py`-> `Python\record_functions.c.h`
- `tier2_generator.py`          -> `Python\executor_cases.c.h`
- `opcode_metadata_generator.py`-> `Include\internal\pycore_opcode_metadata.h`
- `uop_metadata_generator.py`   -> `Include\internal\pycore_uop_metadata.h`
- `tier1_generator.py`          -> `Modules\_testinternalcapi\test_cases.c.h`
  (extra positional input `Modules\_testinternalcapi\testbytecodes.c`)
- `target_generator.py`         -> `Modules\_testinternalcapi\test_targets.h`
  (same extra input)
- `optimizer_generator.py`      -> `Python\optimizer_cases.c.h`
  (inputs: `Python\optimizer_bytecodes.c Python\bytecodes.c`)

## Gotchas

- LINE ENDINGS: the case generators write CRLF on Windows, but the repo stores
  all these generated files as LF and `core.autocrlf=false`. Raw regen makes
  every file look 100%-changed. After regenerating, convert each output back to
  LF (read text, replace `\r\n`->`\n`, write UTF-8 no-BOM). Verify the real diff
  with `git diff --ignore-cr-at-eol --stat`.
  TWO TRAPS (hit 2026-06): (1) msbuild's regen step re-runs during the next
  python.vcxproj build and re-writes the outputs with CRLF, silently undoing a
  manual LF conversion -- convert AFTER the last build, immediately before
  `git add`, and verify the staged blob (`git cat-file blob :<path> | tr -dc
  '\r' | wc -c` in bash). (2) PowerShell .NET file APIs
  ([System.IO.File]::ReadAllBytes etc.) resolve RELATIVE paths against the
  process cwd, not the PS Set-Location -- a relative-path check can silently
  read a file in the wrong checkout; always pass absolute paths.
  (3) `Tools/clinic/clinic.py` also writes CRLF on Windows (hit 2026-09-26
  merging PR 157483); strip with `sed -i 's/
$//'` before `git add`. Also
  `clinic.py --make --srcdir .` ABORTS on Windows at the first
  `@permit_long_summary` warning (_elementtree.c, then pyexpat.c) -- it is
  not a real failure; instead run clinic per-file on the sources that matter,
  e.g. `grep -rl defining_class Modules Objects | grep -v clinic`.
- `Lib/_opcode_metadata.py` is read by `dis` -- if it's stale,
  `dis.dis(fn, adaptive=True)` raises `AssertionError` inside dis.py's
  label resolution rather than a clear "unknown opcode" error.
- FRESH WORKTREES have no `externals/`. Verified 2026-09-15: `PCbuild\build.bat -p x64 -c Release`
  (no `-E`) fetches them and builds in ~5 min. Do NOT pass `/p:ExternalsDir=...` through build.bat
  from PowerShell: cmd splits the argument at `=` ("MSBuild switch error"), and `$env:ExternalsDir`
  was not honoured either (zlib-ng skipped -> `binascii.c: zlib.h not found`). If reusing the main
  checkout's externals, call msbuild directly with `/p:ExternalsDir=`.
- FRESH WORKTREE via direct msbuild (verified 2026-09-25): (1) build `_freeze_module.vcxproj`
  FIRST or getpath.c fails with `Python/frozen_modules/getpath.h: No such file`; (2) pass
  `/p:ExternalsDir=C:\develop\cpython\externals\` WITH the trailing backslash -- without it the
  path is concatenated as `externalsbzip2-1.0.8` and `_ctypes` fails on `ffi.h` (pythoncore itself
  still links, so the mistake only shows when building extension .pyds); (3) python.vcxproj does
  NOT build `_ctypes.pyd`/other extensions -- build `_ctypes.vcxproj` explicitly if a script needs it.
- JIT BUILD IN A FRESH WORKTREE (verified 2026-10-17): `/p:UseJIT=true /p:UseTIER2=1
  /p:IncludeLLVM=true` fails with `RuntimeError: Can't find clang-21!` because
  `Tools/jit/_llvm.py` looks for `<EXTERNALS_DIR or <worktree>/externals>/llvm-21.1.4.0/bin`,
  NOT the msbuild `ExternalsDir` property. Fix: `$env:EXTERNALS_DIR = "C:\develop\cpython\externals"`
  before msbuild (the LLVM tarball is already unpacked there). Also: `python -I` implies `-E`, so
  `PYTHON_JIT=0` is IGNORED under `-I`; drop `-I` (or use `-X jit=0` if supported) for JIT on/off A/Bs.
- MSBUILD FROM GIT BASH: `/p:Foo=bar` and `/nologo` get MSYS path-converted (`/nologo` ->
  `C:/Program Files/Git/nologo`, MSB1008 "Only one project"). Use `-p:`, `-t:`, `-v:m`,
  `-nologo`, `-m` (dash form) or run via PowerShell. `cmd //c script.cmd` from bash also
  failed to find the script; use PowerShell `cmd /c <abs path>`.
- `gh` (GitHub CLI) is NOT installed on this machine. Use git + WebFetch / web
  searches instead.
- `make` is not available (Cygwin bash is present but make targets fail).
- STALE FROZEN MODULES across branches/versions = startup crash. The frozen
  module headers live in `PCbuild/obj/<NNN>_frozen/Python/frozen_modules/*.h`
  and are GITIGNORED (only `Python/frozen_modules/README.txt` is tracked), so
  switching branches does NOT update them. If a newer-main build left frozen
  importlib referencing more common-constants than the current source defines,
  a fresh `python.exe` runs `-VV` fine but CRASHES (`0xC0000005`) the instant it
  executes any bytecode; a Debug build shows `Assertion failed: oparg <
  NUM_COMMON_CONSTANTS, generated_cases.c.h`. FIX (verified 2026-08): no delete
  needed — `msbuild _freeze_module.vcxproj /t:Rebuild` regenerates all frozen
  headers directly; or the older way: `rm -rf PCbuild/obj/*_frozen` then build
  `PCbuild/_freeze_module.vcxproj`
  (uses the in-tree _freeze_module.exe, version-correct), THEN rebuild
  python.vcxproj. NOT caused by JIT/tier2/LTCG (ruled those out). The external
  3.14 PythonForBuild is fine here -- _freeze_module.exe does the actual freeze,
  the host python only orchestrates.
- ADDING A NEW TIER-1 OPCODE (an `inst(...)` in bytecodes.c) renumbers opcodes,
  which breaks TWO caches that the normal build does NOT refresh: (1) the frozen
  modules -> re-freeze (`_freeze_module.vcxproj`) or startup crashes 0xC0000005
  executing frozen importlib; (2) the committed `.pyc` files under Lib (and the
  magic number) -> you MUST bump `PYC_MAGIC_NUMBER` in
  `Include/internal/pycore_magic_number.h`, else stale `.pyc`s load with old
  opcode numbers and produce bizarre errors (e.g. "type object 'AssertionError'
  has no attribute 'collections'", NameErrors on `sys`). Quick unblock: delete
  `Lib/**/__pycache__`. A tier2-ONLY uop (`tier2 op(...)`) does NOT need either
  (uops aren't in frozen/.pyc bytecode).
