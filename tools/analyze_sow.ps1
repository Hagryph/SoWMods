# analyze_sow.ps1 - ONE-TIME import + full auto-analysis of the Steamless-unpacked ShadowOfWar exe.
# Produces the Ghidra project that run_ghidra.ps1 then queries with -noanalysis postScripts.
# The 112 MB (Denuvo) exe takes HOURS; run it in the background and tail the log.
$ErrorActionPreference = 'Stop'
$env:JAVA_HOME = 'C:\dev\jdk\jdk-21.0.11+10'
if (-not $env:MAXMEM) { $env:MAXMEM = '24G' }   # 63 GB box; leave headroom
$headless = 'C:\dev\ghidra\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat'
$proj     = 'C:\dev\re\sow\ghidra-proj'
$exe      = 'C:\dev\re\sow\ShadowOfWar.exe.unpacked.exe'
$log      = 'C:\dev\re\sow\analyze.log'

New-Item -ItemType Directory -Force $proj | Out-Null
Write-Host "Importing + analyzing $exe -> project ShadowOfWar (log: $log)"
# Invoke the .bat directly (paths are clean: no spaces/parens). Tee captures Ghidra's
# stdout+stderr to the log while still showing progress.
& $headless $proj ShadowOfWar -import $exe 2>&1 | Tee-Object -FilePath $log
Write-Host "Done. See $log"
