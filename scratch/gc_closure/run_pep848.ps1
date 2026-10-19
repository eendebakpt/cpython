# Benchmark the closure pass ported onto the PEP 848 reference implementation
# (worktree C:\develop\cpython-incgc, branch gc-closure-pep848).
# PYTHON_GC_CLASSIC=1 is the A/B switch; duplicate controls (C0a, C0b) detect
# a disturbed machine.  Every pyperf worker appends gc.get_stats() at exit to
# GC_STATS_OUT (via gcstatsdump.py installed into the venv), which gives the
# per-generation GC time independent of wall-time noise.
# Usage: run_pep848.ps1 [-Fast] [-Bench "a b c"] [-Configs "C0a A C0b Ab"]
param(
    [string]$Bench = "gc_collect gc_traversal deltablue sphinx xml_etree async_tree_memoization docutils sympy networkx mdp raytrace richards",
    [string]$Configs = "C0a A C0b Ab",
    [switch]$Fast,
    [string]$Prefix = "pep"
)
$host314 = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\python.exe"
$target = "C:\develop\cpython-incgc\PCbuild\amd64\python.exe"
$workdir = "C:\develop\cpython\scratch\gc_closure\pep848\work"
$out = "C:\develop\cpython\scratch\gc_closure\results_pep848"
$hook = "C:\develop\cpython\scratch\gc_closure\pep848\gcstatsdump.py"
New-Item -ItemType Directory -Force $workdir, $out | Out-Null
Set-Location $workdir

# venv + stats hook (idempotent)
& $host314 -m pyperformance venv create --python $target 2>&1 | Select-Object -Last 1
$sp = Get-ChildItem "$workdir\venv" -Directory | Where-Object Name -notlike "*-bm-*" | ForEach-Object { Join-Path $_.FullName "Lib\site-packages" } | Where-Object { Test-Path $_ }
foreach ($d in $sp) {
    Copy-Item $hook (Join-Path $d "gcstatsdump.py") -Force
    Set-Content -Path (Join-Path $d "zz_gcstats.pth") -Value "import gcstatsdump"
    "hook installed in $d"
}

$classicOf = @{ "C0a" = "1"; "C0b" = "1"; "C0c" = "1"; "A" = "0"; "Ab" = "0"; "Ac" = "0" }
foreach ($name in ($Configs -split " ")) {
    $env:PYTHON_GC_CLASSIC = $classicOf[$name]
    foreach ($b in ($Bench -split " ")) {
        $tag = "${Prefix}_${name}_$b"
        $env:GC_STATS_OUT = "$out\$tag.stats.jsonl"
        if (Test-Path $env:GC_STATS_OUT) { Remove-Item $env:GC_STATS_OUT }
        $args = @("run", "--python", $target, "-b", $b, "--inherit-environ", "PYTHON_GC_CLASSIC,GC_STATS_OUT", "-o", "$out\$tag.json")
        if ($Fast) { $args += "--fast" }
        "== $tag at $(Get-Date -Format HH:mm:ss)"
        & $host314 -m pyperformance @args 2>&1 | Tee-Object -FilePath "$out\$tag.log" | Select-Object -Last 1
    }
}
Remove-Item Env:PYTHON_GC_CLASSIC, Env:GC_STATS_OUT -ErrorAction SilentlyContinue
"pep848 campaign done at $(Get-Date -Format HH:mm:ss)"
