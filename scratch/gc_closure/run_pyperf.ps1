param(
    [int]$Mode = 0,
    [string]$Bench = "gc_collect,gc_traversal,sphinx,deltablue",
    [string]$Tag = "",
    [switch]$Stats,
    [switch]$Fast,
    [switch]$FreeThreaded,
    [int]$Mark = 0,
    [int]$Merge = 0
)
# Runs pyperformance benchmarks on the gc-closure-sum worktree build with
# PYTHON_GC_CLOSURE=$Mode. With -Stats, PYTHON_GC_CLOSURE_STATS points at a
# JSON-lines file that every worker process appends to.
$ErrorActionPreference = "Continue"
$host314 = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\python.exe"
$target = "C:\develop\cpython-gcclosure\PCbuild\amd64\python.exe"
$workdir = "C:\develop\cpython\scratch\gc_closure"          # pyperformance creates venv/ here
$out = "C:\develop\cpython\scratch\gc_closure\results"
if ($FreeThreaded) {
    $target = "C:\develop\cpython-gcclosure\PCbuild\amd64t\python3.16t.exe"
    $workdir = "C:\develop\cpython\scratch\gc_closure\ft"   # separate venv (cp316t ABI)
    New-Item -ItemType Directory -Force $workdir | Out-Null
    $out = "C:\develop\cpython\scratch\gc_closure\results_ft"
}
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $Tag) { $Tag = "mode${Mode}_mark${Mark}_merge$Merge" }
$env:PYTHON_GC_CLOSURE = "$Mode"
$env:PYTHON_GC_MARK = "$Mark"
$env:PYTHON_GC_DAG = "$Merge"
$inherit = "PYTHON_GC_CLOSURE,PYTHON_GC_MARK,PYTHON_GC_DAG"
if ($Stats) {
    $env:PYTHON_GC_CLOSURE_STATS = "$out\stats_$Tag.jsonl"
    if (Test-Path $env:PYTHON_GC_CLOSURE_STATS) { Remove-Item $env:PYTHON_GC_CLOSURE_STATS }
    $inherit += ",PYTHON_GC_CLOSURE_STATS"
} else {
    Remove-Item Env:PYTHON_GC_CLOSURE_STATS -ErrorAction SilentlyContinue
}
$args = @("run", "--python", $target, "-b", $Bench,
          "--inherit-environ", $inherit, "-o", "$out\$Tag.json")
if ($Fast) { $args += "--fast" }
Set-Location $workdir
& $host314 -m pyperformance @args 2>&1 | Tee-Object -FilePath "$out\$Tag.log"
"exit=$LASTEXITCODE"
