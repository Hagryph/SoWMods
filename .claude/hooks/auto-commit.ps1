# SoWMods - Claude Code "Stop" hook.
# SoWMods is a SINGLE monorepo (not per-mod sub-repos), so this commits + best-effort pushes the
# workspace ROOT repo after each prompt. Open Claude at the SoWMods root so the hook fires here.
$ErrorActionPreference = 'SilentlyContinue'

# hooks -> .claude -> SoWMods (the repo root)
$root  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

if (-not (Test-Path (Join-Path $root '.git'))) { return }

Push-Location $root
try {
    $dirty = git status --porcelain
    if ($dirty) {
        # Ensure a local identity exists so the commit never fails.
        if (-not (git config user.name))  { git config user.name  'Hagryph' }
        if (-not (git config user.email)) { git config user.email 'hagryph.gaming@gmail.com' }

        git add -A | Out-Null
        git commit -m "auto: $stamp (Claude Stop hook)" | Out-Null

        $branch = (git rev-parse --abbrev-ref HEAD).Trim()
        # Best-effort push; stay silent/non-fatal when offline or no upstream.
        git push origin $branch 2>$null | Out-Null
        Write-Output "[auto-commit] SoWMods @ $stamp"
    }
} finally {
    Pop-Location
}
