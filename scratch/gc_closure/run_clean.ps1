# Benchmark the clean branch (gc-closure-young): PYTHON_GC_CLASSIC=1 is the
# A/B switch (classic algorithm everywhere).  Duplicate control runs detect
# a disturbed machine.  Usage: run_clean.ps1 [-Fast] [-Bench "a b c"]
param(
    [string]$Bench = "gc_collect gc_traversal deltablue sphinx xml_etree async_tree_memoization docutils sympy networkx mdp raytrace richards",
    [switch]$Fast,
    [string]$Prefix = "clean"
)
$host314 = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\python.exe"
$target = "C:\develop\cpython-gcclean\PCbuild\amd64\python.exe"
$workdir = "C:\develop\cpython\scratch\gc_closure\clean"
$out = "C:\develop\cpython\scratch\gc_closure\results_clean"
New-Item -ItemType Directory -Force $workdir, $out | Out-Null
Set-Location $workdir
foreach ($cfg in @(@("C0a", 1), @("A", 0), @("C0b", 1), @("Ab", 0))) {
    $name, $classic = $cfg
    $env:PYTHON_GC_CLASSIC = "$classic"
    foreach ($b in ($Bench -split " ")) {
        $tag = "${Prefix}_${name}_$b"
        $args = @("run", "--python", $target, "-b", $b, "--inherit-environ", "PYTHON_GC_CLASSIC", "-o", "$out\$tag.json")
        if ($Fast) { $args += "--fast" }
        & $host314 -m pyperformance @args 2>&1 | Tee-Object -FilePath "$out\$tag.log" | Select-Object -Last 1
    }
}
Remove-Item Env:PYTHON_GC_CLASSIC
"clean campaign done"
