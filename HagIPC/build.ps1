# build.ps1 - build HagIPC.dll (the standalone debug/IPC DLL).
# HagIPC is not deployed into the game folder; it is hot-injected into the running process with
# tools/inject.ps1 (or auto-loaded by SoWLoader's mods folder). This script just builds it and
# prints the resulting DLL path.
[CmdletBinding()]
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

if (-not $NoBuild) {
    Set-Location $root
    Write-Host '== configure =='
    & $cmake --preset vs2022 | Select-Object -Last 2
    Write-Host '== build =='
    & $cmake --build "$root\build" --config Release | Select-Object -Last 6
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

$dll = Get-ChildItem "$root\build" -Recurse -Filter HagIPC.dll -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dll) { throw 'HagIPC.dll not found - build first (omit -NoBuild).' }
Write-Host "built -> $($dll.FullName)"
