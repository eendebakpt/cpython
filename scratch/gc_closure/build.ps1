param([string]$Dir = "C:\develop\cpython-gcclosure", [string]$Config = "Release", [switch]$All)
$python = "C:\Users\eendebakpt\AppData\Local\Python\pythoncore-3.14-64\python.exe"
$mb = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\msbuild.exe"
Set-Location $Dir
$log = "C:\develop\cpython\scratch\gc_closure\build_$((Split-Path $Dir -Leaf))_$Config.log"
if (Test-Path $log) { Remove-Item $log }
$common = @("/p:Configuration=$Config", "/p:Platform=x64", "/p:PythonForBuild=$python",
            "/p:PlatformToolset=v145", "/p:ExternalsDir=C:\develop\cpython\externals\", "/v:m", "/nologo", "/m")
if ($All) {
    & $mb "PCbuild\pcbuild.proj" /t:Build @common *>> $log
    "pcbuild.proj exit=$LASTEXITCODE"
} else {
    foreach ($p in @("_freeze_module", "python", "_testinternalcapi", "_testcapi", "_ctypes", "_testlimitedcapi", "_testclinic", "_socket", "select", "_asyncio", "_overlapped")) {
        & $mb "PCbuild\$p.vcxproj" /t:Build @common *>> $log
        "$p exit=$LASTEXITCODE"
        if ($LASTEXITCODE -ne 0 -and $p -in @("_freeze_module", "python")) { break }
    }
}
Get-Content $log | Select-String -Pattern " error |warning C" | Select-Object -First 20
