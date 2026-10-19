# Full campaign: statistics run (mode 4) then timing runs for modes 0, 1, 2.
$script = "C:\develop\cpython\scratch\gc_closure\run_pyperf.ps1"
$bench = "gc_collect,gc_traversal,sphinx,deltablue"
& powershell -NoProfile -File $script -Mode 4 -Stats -Bench $bench -Tag stats_mode4 2>&1 | Select-Object -Last 12
foreach ($m in 0, 1, 2) {
    & powershell -NoProfile -File $script -Mode $m -Stats -Bench $bench -Tag "timing_mode$m" 2>&1 | Select-Object -Last 12
}
"campaign done"
