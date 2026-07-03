# sow_load_save.ps1 - drive Shadow of War from title/main menu to loading the selected save.
#
# StartAt Title:    Space -> main menu, Space on START -> save selection, Space on selected save -> load.
# StartAt MainMenu: Space on START -> save selection, Space on selected save -> load.
param(
    [string]$ProcessName = 'ShadowOfWar',
    [ValidateSet('Title', 'MainMenu')]
    [string]$StartAt = 'Title',
    [int]$TitleToMainMenuMs = 3500,
    [int]$MainMenuToSaveSelectMs = 3000,
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

if ($StartAt -eq 'Title') {
    Write-Output '[sow] title prompt -> main menu'
    Send-GameKeys -KeyList @('SPACE')
    Start-Sleep -Milliseconds $TitleToMainMenuMs
} else {
    Write-Output '[sow] already at main menu'
}

Write-Output '[sow] main menu START -> save selection'
Send-GameKeys -KeyList @('SPACE')
Start-Sleep -Milliseconds $MainMenuToSaveSelectMs

Write-Output '[sow] selected save -> load'
Send-GameKeys -KeyList @('SPACE')
Write-Output '[sow] load requested'
