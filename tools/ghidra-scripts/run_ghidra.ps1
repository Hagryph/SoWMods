# run_ghidra.ps1 - run a Ghidra headless postScript against the analyzed (unpacked) ShadowOfWar.
#   .\run_ghidra.ps1 Trace.java <outfile> <mode> <targets...>
# Queries the already-analyzed project with -noanalysis (fast). Run analyze_sow.ps1 once first.
param(
    [Parameter(Mandatory=$true)][string]$ScriptName,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$ScriptArgs
)
$ErrorActionPreference = 'Stop'
$env:JAVA_HOME = 'C:\dev\jdk\jdk-21.0.11+10'   # system Java is 1.8, too old for Ghidra 12
if (-not $env:MAXMEM) { $env:MAXMEM = '24G' }
$headless = 'C:\dev\ghidra\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat'
$proj     = 'C:\dev\re\sow\ghidra-proj'
$scripts  = $PSScriptRoot
& $headless $proj ShadowOfWar -process 'ShadowOfWar.exe.unpacked.exe' -noanalysis -scriptPath $scripts -postScript $ScriptName @ScriptArgs
