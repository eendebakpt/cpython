# Second campaign: per-benchmark statistics (truth mode, fast) then quiet timing reruns.
$script = "C:\develop\cpython\scratch\gc_closure\run_pyperf.ps1"
foreach ($b in "gc_collect", "gc_traversal", "sphinx", "deltablue") {
    & powershell -NoProfile -File $script -Mode 4 -Stats -Bench $b -Fast -Tag "stats4_$b" 2>&1 | Select-Object -Last 4
}
$bench = "gc_collect,gc_traversal,sphinx,deltablue"
foreach ($m in 0, 1, 2) {
    & powershell -NoProfile -File $script -Mode $m -Stats -Bench $bench -Tag "timing2_mode$m" 2>&1 | Select-Object -Last 12
}
"campaign2 done"
