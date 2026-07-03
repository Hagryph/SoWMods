# SoWMods - Codex "Stop" hook.
# Commits the dirty workspace with an automatic message and best-effort pushes.
# Stop hooks must emit JSON or nothing. This script intentionally stays silent.
$ErrorActionPreference = 'SilentlyContinue'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $root '.git'))) { return }

Push-Location $root
try {
    $dirty = @(git status --porcelain)
    if (-not $dirty -or $dirty.Count -eq 0) { return }

    if (-not (git config user.name))  { git config user.name  'Hagryph' }
    if (-not (git config user.email)) { git config user.email 'hagryph.gaming@gmail.com' }

    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $message = "auto: $stamp (Codex Stop hook)"

    git add -A | Out-Null
    git commit -m $message | Out-Null
    if ($LASTEXITCODE -ne 0) { return }

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    git push origin $branch 2>$null | Out-Null
} finally {
    Pop-Location
}
