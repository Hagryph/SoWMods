# build.ps1 - build steam_api64.dll (the SoWLoader proxy) and deploy it into the game folder.
# Deploy is SAFE + idempotent: it renames the real steam_api64.dll -> steam_api64_org.dll ONCE
# (only if _org doesn't already exist), then drops our proxy in as steam_api64.dll.
[CmdletBinding()]
param(
    [switch]$NoBuild,
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\ShadowOfWar\x64'
)
$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

if (-not $NoBuild) {
    Set-Location $root
    $env:VCPKG_ROOT = 'C:\dev\vcpkg'
    Write-Host '== configure =='
    & $cmake --preset vs2022 | Select-Object -Last 2
    Write-Host '== build =='
    & $cmake --build "$root\build" --config Release | Select-Object -Last 6
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

$dll = Get-ChildItem "$root\build" -Recurse -Filter steam_api64.dll -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notlike '*_org*' } | Select-Object -First 1
if (-not $dll) { throw 'steam_api64.dll not found - build first (omit -NoBuild).' }

$live = Join-Path $GameDir 'steam_api64.dll'
$orig = Join-Path $GameDir 'steam_api64_org.dll'

# One-time: preserve the real Steam DLL as _org. Never overwrite an existing _org (that IS the real one).
if (-not (Test-Path $orig)) {
    if (-not (Test-Path $live)) { throw "no steam_api64.dll in $GameDir - is this the right game folder?" }
    Rename-Item $live $orig
    Write-Host "backed up original -> steam_api64_org.dll"
} else {
    Write-Host "steam_api64_org.dll already present (original already preserved) - good"
}

# Overwrite in place with .NET File.Copy (no delete step: deleting under Program Files is
# blocked by the sandbox, and an overwrite-copy doesn't need one).
[System.IO.File]::Copy($dll.FullName, $live, $true)
Write-Host "deployed proxy -> $live"
Write-Host "`nTo restore vanilla: delete steam_api64.dll and rename steam_api64_org.dll back to steam_api64.dll"
