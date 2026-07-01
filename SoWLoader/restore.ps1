# restore.ps1 - put the game back to vanilla: remove our proxy, restore the real steam_api64.dll.
[CmdletBinding()]
param([string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\ShadowOfWar\x64')
$ErrorActionPreference = 'Stop'
$live = Join-Path $GameDir 'steam_api64.dll'
$orig = Join-Path $GameDir 'steam_api64_org.dll'
if (-not (Test-Path $orig)) { Write-Host "nothing to restore (no steam_api64_org.dll)"; return }
if (Test-Path $live) { Remove-Item $live -Force }
Rename-Item $orig $live
Write-Host "restored vanilla steam_api64.dll"
