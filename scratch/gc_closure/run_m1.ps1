# Milestone 1 campaign (default build, 12 benchmarks):
#   truth-mode statistics with and without mark-alive, then timing of the
#   configurations C0 (classic), M (mark + classic), MC (mark + closure in
#   full collections), C (closure everywhere), two interleaved rounds.
param([switch]$StatsOnly, [switch]$TimingOnly)
$script = "C:\develop\cpython\scratch\gc_closure\run_pyperf.ps1"
$benches = @("gc_collect", "gc_traversal", "sphinx", "deltablue",
             "async_tree_memoization", "xml_etree", "docutils", "sqlalchemy_declarative",
             "sympy", "dask", "networkx", "mdp")
$all = $benches -join ","
if (-not $TimingOnly) {
    foreach ($b in $benches) {
        & powershell -NoProfile -File $script -Mode 4 -Mark 0 -Stats -Bench $b -Fast -Tag "m1stats_nomark_$b" 2>&1 | Select-Object -Last 2
        & powershell -NoProfile -File $script -Mode 4 -Mark 1 -Stats -Bench $b -Fast -Tag "m1stats_mark_$b" 2>&1 | Select-Object -Last 2
    }
}
if (-not $StatsOnly) {
    $configs = @(@{Mode = 0; Mark = 0; Name = "C0"}, @{Mode = 0; Mark = 1; Name = "M"},
                 @{Mode = 5; Mark = 1; Name = "MC"}, @{Mode = 1; Mark = 0; Name = "C"})
    foreach ($round in 1, 2) {
        foreach ($c in $configs) {
            & powershell -NoProfile -File $script -Mode $c.Mode -Mark $c.Mark -Stats -Bench $all -Tag "m1_$($c.Name)_r$round" 2>&1 | Select-Object -Last 2
        }
    }
}
"m1 campaign done"
