# build.ps1 - build the HagOverlay CSS overlay (WPF + WebView2, .NET 9).
# Output: bin\Release\net9.0-windows\HagOverlay.exe. Run it while the game is up and press F8.
[CmdletBinding()] param([switch]$Run)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
dotnet build -c Release -v minimal
if ($LASTEXITCODE -ne 0) { throw "build failed" }
$exe = Join-Path $PSScriptRoot 'bin\Release\net9.0-windows\HagOverlay.exe'
Write-Host "built -> $exe"
if ($Run) { Start-Process $exe; Write-Host 'launched (press F8 over the game)' }
