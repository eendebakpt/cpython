# Full pyperformance suite on the clean branch: classic (control), closure
# pass, classic again (control).  Waits for any running pyperformance job to
# finish first.  Results: results_clean\full_<name>.json
param([switch]$Fast = $true, [string[]]$Order = @("C0a", "A", "C0b"))
$host314 = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\python.exe"
$target = "C:\develop\cpython-gcclean\PCbuild\amd64\python.exe"
$workdir = "C:\develop\cpython\scratch\gc_closure\clean"
$out = "C:\develop\cpython\scratch\gc_closure\results_clean"
New-Item -ItemType Directory -Force $workdir, $out | Out-Null
Set-Location $workdir
# wait for an idle machine: no other benchmark process, and the machine's
# total CPU use (all processes, 20 s window) below 15% of one core... in
# practice below 1.0 core-seconds per second.
function Get-CpuRate {
    $s1 = Get-Process | Select-Object Id, CPU
    Start-Sleep -Seconds 20
    $s2 = Get-Process | Select-Object Id, CPU
    $d = 0.0
    foreach ($p in $s2) { $o = $s1 | Where-Object Id -eq $p.Id; if ($o -and $p.CPU -and $o.CPU) { $d += ($p.CPU - $o.CPU) } }
    return $d / 20.0
}
while ($true) {
    $busy = Get-Process | Where-Object { $_.Path -like "$workdir\venv\*" -or $_.Path -like 'C:\develop\cpython-gcclean\PCbuild\*' }
    if (-not $busy) {
        $rate = Get-CpuRate
        "idle check: $([math]::Round($rate, 2)) cores busy at $(Get-Date -Format HH:mm:ss)"
        if ($rate -lt 1.0) { break }
    }
    Start-Sleep -Seconds 60
}
"starting full suite at $(Get-Date -Format HH:mm:ss)"
foreach ($name in $Order) {
    $env:PYTHON_GC_CLASSIC = $(if ($name -like "C0*") { "1" } else { "0" })
    $args = @("run", "--python", $target, "--inherit-environ", "PYTHON_GC_CLASSIC", "-o", "$out\full_$name.json")
    if ($Fast) { $args += "--fast" }
    "== $name (PYTHON_GC_CLASSIC=$env:PYTHON_GC_CLASSIC) at $(Get-Date -Format HH:mm:ss)"
    & $host314 -m pyperformance @args 2>&1 | Tee-Object -FilePath "$out\full_$name.log" | Select-Object -Last 2
}
Remove-Item Env:PYTHON_GC_CLASSIC
"full suite done at $(Get-Date -Format HH:mm:ss)"
