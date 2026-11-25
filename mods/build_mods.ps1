# build_mods.ps1 - build every mod under mods/ (each is its own CMake project) and deploy the
# resulting DLLs into the game's  <install root>\mods\  folder (NOT x64\mods\), where the loader
# picks them up in filename order.
#
#   .\build_mods.ps1                 # build + deploy all mods
#   .\build_mods.ps1 -Only SampleMod # just one mod
#
# -GameDir is the x64\ folder (matching build.ps1); mods deploy to its parent (the install root).
[CmdletBinding()]
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\ShadowOfWar\x64',
    [string]$Only
)
$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

$installRoot = Split-Path $GameDir -Parent    # ...\ShadowOfWar
$modsOut = Join-Path $installRoot 'mods'      # ...\ShadowOfWar\mods
New-Item -ItemType Directory -Force $modsOut | Out-Null

$dirs = Get-ChildItem $root -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'CMakeLists.txt') }
if ($Only) { $dirs = $dirs | Where-Object { $_.Name -eq $Only } }
if (-not $dirs) { throw "no mods found under $root (need a subfolder with CMakeLists.txt)" }

foreach ($d in $dirs) {
    $name = $d.Name
    $bld  = Join-Path $d.FullName 'build'
    Write-Host "== building mod: $name =="
    & $cmake -S $d.FullName -B $bld -G 'Visual Studio 17 2022' -A x64 | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $name" }
    & $cmake --build $bld --config Release | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "build failed for $name" }

    $dll = Get-ChildItem $bld -Recurse -Filter '*.dll' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\Release\*' } | Select-Object -First 1
    if (-not $dll) { $dll = Get-ChildItem $bld -Recurse -Filter '*.dll' | Select-Object -First 1 }
    if (-not $dll) { throw "no DLL produced for $name" }

    [System.IO.File]::Copy($dll.FullName, (Join-Path $modsOut $dll.Name), $true)
    Write-Host "deployed $($dll.Name) -> $modsOut"

    # deploy any data files the mod ships beside its DLL
    Get-ChildItem $d.FullName -File -Filter '*.catalog' -ErrorAction SilentlyContinue | ForEach-Object {
        [System.IO.File]::Copy($_.FullName, (Join-Path $modsOut $_.Name), $true)
        Write-Host "deployed $($_.Name) -> $modsOut"
    }
}
Write-Host "`nAll mods built + deployed to $modsOut"
