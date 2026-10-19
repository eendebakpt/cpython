# Quick exploration campaign: pyperf --fast, a GC-sensitive core set, one run
# per (config, benchmark) so the stats file gives per-benchmark GC-phase
# times.  Configs: "name:mode:mark:merge".
param(
    # space separated "name:mode:mark:merge" entries (powershell -File cannot pass arrays)
    [string]$Configs = "C0:0:0:0 M:0:1:0 MC:5:1:0 MCm:5:1:1 Cmm:1:1:1",
    [string]$Bench = "gc_collect gc_traversal deltablue sphinx xml_etree async_tree_memoization",
    [string]$Prefix = "q",
    [switch]$Truth
)
$script = "C:\develop\cpython\scratch\gc_closure\run_pyperf.ps1"
$benchList = $Bench -split " "       # a [string] param cannot hold the array
foreach ($c in ($Configs -split " ")) {
    $name, $mode, $mark, $merge = $c -split ":"
    if ($Truth) { $mode = 4 }
    foreach ($b in $benchList) {
        & powershell -NoProfile -File $script -Mode $mode -Mark $mark -Merge $merge -Stats -Fast -Bench $b -Tag "${Prefix}_${name}_$b" 2>&1 | Select-Object -Last 1
    }
}
"quick campaign done ($Prefix)"
