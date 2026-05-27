# Benchmark: allocator-init variants

Compares four configurations of the instance-allocator code paths.

## What is being compared

| Label       | Branch                              | Change                                                                                                                                            |
|-------------|-------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| baseline    | `main`                              | Unmodified.                                                                                                                                       |
| memset_init | `feature/alloc-init-memset`         | One-line change in `Objects/dictobject.c`: replace the for-loop that zeros `values->values[0..size-1]` with `memset(values->values, 0, size * sizeof(PyObject *))`. Does not touch `typeobject.c`. |
| helper      | `feature/alloc-fast-path-helper`    | A4 (skip-memset) + new `static inline` helper `alloc_inline_values_instance()`. Fast path has zero `INLINE_VALUES` re-checks. Slow path is leaner. |
| reduced     | `feature/alloc-fast-path`           | A4 + single function with one consolidated `if (INLINE_VALUES)` at the top. Smaller diff than `helper`.                                          |

`helper` and `reduced` sit on top of `feature/step2-skip-memset` (A4); the
diff vs `main` includes A4. `memset_init` is independent of A4 and the
fast-path refactor — it touches only `_PyObject_InitInlineValues` in
`Objects/dictobject.c`.

**Note on a tried-and-failed variant:** "Drop the init zero loop entirely"
was attempted on `feature/alloc-skip-init-zero`. It turned out to be
**incorrect**: `_PyObject_GC_New` in `Python/gc.c` is a *second* alloc path
that calls `_PyObject_InitInlineValues` without first zeroing the body.
Removing the loop crashed `NamedTuple` class creation. The
`_PyObject_GC_New` path needs values to remain explicitly zeroed.

**Working hypothesis** (to confirm or refute with this run):
- `memset_init` should be a free or near-free win over baseline (compiler
  vectorizes the memset; the original loop may or may not vectorize on
  this MSVC toolchain).
- `helper` may beat `reduced` slightly if the compiler doesn't inline the
  helper back into `_PyType_AllocNoTrack`. If they're within ~1%, prefer
  `reduced` for the smaller diff.
- `memset_init` stacks with `helper`/`reduced` (different file); a future
  "combined" config could measure that.

## Benchmark cases

`bench_alloc_isolated.py` measures five class-creation patterns, each in
its own subprocess (no cross-case state pollution), with GC disabled
during measurement:

| case        | what it exercises                                                                |
|-------------|----------------------------------------------------------------------------------|
| PlainEmpty  | inline-values fast path, no Python `__init__` frame                              |
| PlainInit   | inline-values fast path + Python `__init__` (5 attribute stores)                 |
| DC          | `@dataclass` with 5 fields (generated `__init__`, same alloc path as PlainInit) |
| SlotsCls    | `__slots__` (no INLINE_VALUES) -- exercises the **slow path** of the allocator   |
| TupleSub    | subclass of `tuple` (tp_itemsize > 0) -- the other slow-path branch              |

Each case warms up with 50 000 iterations to trigger tier-1 specialization,
then times 11 trials of 1 000 000 instantiations each and reports the min,
median, and mean per-instance nanoseconds.

## How to run

On a quiet system (close browsers, disable scheduled tasks, set Windows
power plan to High Performance, etc.):

```powershell
# In c:\develop\cpython (or wherever the repo lives on the other PC):
python run_alloc_3way.py
```

The runner:
1. Verifies the working tree is clean and remembers the starting branch.
2. For each of the three configs:
   - `git checkout <branch>`
   - `msbuild ...` (full rebuild, Release x64, tier-2 interpreter enabled)
   - Runs `bench_alloc_isolated.py` three times, picks min-of-min per case.
3. Restores the starting branch and rebuilds.
4. Prints a 3-column comparison table and saves raw JSON to
   `alloc_3way_results.json` for offline plotting.

## Prerequisites on the other PC

- The repo must contain all three branches (`main`,
  `feature/alloc-fast-path-helper`, `feature/alloc-fast-path`).
- `PCbuild\amd64\python.exe` must already exist (used as the bootstrap
  PythonForBuild). If absent, build once manually:

  ```powershell
  $mb = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\msbuild.exe"  # or your VS path
  & $mb PCbuild\pcbuild.proj /t:Build /p:Configuration=Release /p:Platform=x64 /p:UseTIER2=4 /v:m /nologo /m
  ```

- If your VS path differs, edit `MSBUILD_CANDIDATES` near the top of
  `run_alloc_3way.py`.

## What "good" looks like

We expect, vs `main`:
- **PlainEmpty / PlainInit / DC** (inline-values cases): -5% to -10% --
  these go through the fast path.
- **SlotsCls / TupleSub**: ~0% (within noise) -- these still go through
  the general path. A regression here would mean the refactor hurt the
  slow path.

If `helper` and `reduced` are within ~1% of each other on every case,
land `reduced` (smaller diff). If `helper` is consistently faster by more
than that, land `helper`.

## Manual one-shot bench

If you just want to time a single configuration without the 3-way driver:

```powershell
git checkout feature/alloc-fast-path
# rebuild...
python bench_alloc_isolated.py
```
