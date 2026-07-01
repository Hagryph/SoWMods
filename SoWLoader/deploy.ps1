# Deploy only (no build). Build + deploy is build.ps1; this is the deploy-only shortcut.
& "$PSScriptRoot\build.ps1" -NoBuild @args
