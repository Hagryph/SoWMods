# sow_return_to_main_menu.ps1 - drive Shadow of War from gameplay back to the main menu.
#
# Expected starting point: in-game control/HUD. Path: ESC -> pause menu, select QUIT, confirm Yes.
param(
    [string]$ProcessName = 'ShadowOfWar',
    [int]$PauseLoadMs = 2500,
    [int]$ConfirmLoadMs = 1000,
    [int]$ReturnWaitMs = 12000,
    [int]$QuitDownCount = 8,
    [int]$HoldMs = 40,
    [switch]$Background
)
$ErrorActionPreference = 'Stop'

$keyTool = Join-Path $PSScriptRoot 'send_keys_window.ps1'
$useForeground = -not $Background.IsPresent

function Send-GameKeys {
    param([string[]]$KeyList)
    if ($useForeground) {
        & $keyTool -ProcessName $ProcessName -Keys $KeyList -HoldMs $HoldMs -Foreground | Write-Output
    } else {
        & $keyTool -ProcessName $ProcessName -Keys $KeyList -HoldMs $HoldMs | Write-Output
    }
}

Write-Output '[sow] gameplay -> pause menu'
Send-GameKeys -KeyList @('ESC')
Start-Sleep -Milliseconds $PauseLoadMs

$quitKeys = @()
for ($i = 0; $i -lt $QuitDownCount; ++$i) { $quitKeys += 'DOWN' }
$quitKeys += 'SPACE'

Write-Output '[sow] pause menu -> quit confirmation'
Send-GameKeys -KeyList $quitKeys
Start-Sleep -Milliseconds $ConfirmLoadMs

Write-Output '[sow] confirm quit -> main menu'
Send-GameKeys -KeyList @('SPACE')
Start-Sleep -Milliseconds $ReturnWaitMs
Write-Output '[sow] return-to-menu wait complete'
