param(
    [int]$Mode = 0,
    [string[]]$Bench = @("gc_collect", "gc_traversal", "sphinx", "deltablue"),
    [string]$Tag = "",
    [switch]$Stats,
    [switch]$Fast,
    [string]$Target = "C:\develop\cpython-gcclosure\PCbuild\amd64t\python3.16t.exe",
    [string]$VenvDir = "C:\develop\cpython\scratch\gc_closure\ft\venv_manual",
    [string]$Out = "C:\develop\cpython\scratch\gc_closure\results_ft"
)
# pyperformance cannot create a venv for the free-threaded build (psutil fails
# to build for cp316t), so run the benchmark scripts directly with pyperf in a
# hand-made venv: pyperf without psutil (optional dependency), plus sphinx.
$ErrorActionPreference = "Continue"
$site = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\Lib\site-packages\pyperformance\data-files\benchmarks"
New-Item -ItemType Directory -Force $Out | Out-Null
if (-not (Test-Path "$VenvDir\Scripts\python.exe")) {
    & $Target -m venv $VenvDir
    & "$VenvDir\Scripts\python.exe" -m pip install -q --no-deps pyperf==2.10.0 packaging
    & "$VenvDir\Scripts\python.exe" -m pip install -q "sphinx==7.3.7" "python-docs-theme==2024.6"
}
$py = "$VenvDir\Scripts\python.exe"
if (-not $Tag) { $Tag = "mode$Mode" }
$env:PYTHON_GC_CLOSURE = "$Mode"
if (-not $env:PYTHON_GC_DAG) { $env:PYTHON_GC_DAG = "0" }
if (-not $env:PYTHON_GC_MARK) { $env:PYTHON_GC_MARK = "0" }
$inherit = "PYTHON_GC_CLOSURE,PYTHON_GC_DAG,PYTHON_GC_MARK"
if ($Stats) {
    $env:PYTHON_GC_CLOSURE_STATS = "$Out\stats_$Tag.jsonl"
    if (Test-Path $env:PYTHON_GC_CLOSURE_STATS) { Remove-Item $env:PYTHON_GC_CLOSURE_STATS }
    $inherit += ",PYTHON_GC_CLOSURE_STATS"
} else {
    Remove-Item Env:PYTHON_GC_CLOSURE_STATS -ErrorAction SilentlyContinue
}
foreach ($b in $Bench) {
    $script = "$site\bm_$b\run_benchmark.py"
    $json = "$Out\${Tag}_$b.json"
    if (Test-Path $json) { Remove-Item $json }
    $extra = @()
    if ($Fast) { $extra += "--fast" }
    Set-Location "$site\bm_$b"
    & $py $script -o $json --inherit-environ $inherit @extra 2>&1 | Select-Object -Last 3
}
"done $Tag"
