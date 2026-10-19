# In-process alternating A/B (ab_inproc.py), one process per benchmark.
# -Py selects the interpreter (default: the PEP 848 branch's benchmark venv,
# which has all benchmark dependencies); -XOpt is passed as a -X option
# (the main-based clean branch ignores -X gc=..., pass "dev" there).
param(
    [string]$Bench = "deltablue richards raytrace mdp networkx sympy docutils xml_etree async_tree_memoization sphinx",
    [double]$Seconds = 60,
    [string]$Out = "C:\develop\cpython\scratch\gc_closure\results_pep848\inproc.jsonl",
    [string]$Py = "C:\develop\cpython\scratch\gc_closure\pep848\work\venv\cpython3.16-d38aef4fc8ea-compat-31b33d68c68a\Scripts\python.exe",
    [string]$XOpt = "gc=incremental"
)
"using $Py"
if (Test-Path $Out) { Remove-Item $Out }
foreach ($b in ($Bench -split " ")) {
    "== $b at $(Get-Date -Format HH:mm:ss)"
    & $Py -X $XOpt "C:\develop\cpython\scratch\gc_closure\pep848\ab_inproc.py" $b $Seconds $Out 2>&1 | Where-Object { $_ -notlike "{*" }
}
"inproc done at $(Get-Date -Format HH:mm:ss)"
