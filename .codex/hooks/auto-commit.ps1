# SoWMods - Codex/Claude "Stop" hook.
# Prompts for a commit message when a prompt ends, commits the dirty workspace, and best-effort pushes.
$ErrorActionPreference = 'SilentlyContinue'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $root '.git'))) { return }

function Read-CommitMessage([string[]]$Changes) {
    $preview = ($Changes | Select-Object -First 25) -join [Environment]::NewLine
    if ($Changes.Count -gt 25) {
        $preview += [Environment]::NewLine + ("... and {0} more" -f ($Changes.Count - 25))
    }

    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    Add-Type -AssemblyName System.Drawing | Out-Null

    $form = New-Object Windows.Forms.Form
    $form.Text = 'SoWMods auto commit'
    $form.StartPosition = 'CenterScreen'
    $form.TopMost = $true
    $form.Width = 620
    $form.Height = 420

    $label = New-Object Windows.Forms.Label
    $label.Text = "Enter a commit message. Leave blank or press Cancel to skip."
    $label.AutoSize = $true
    $label.Left = 12
    $label.Top = 12
    $form.Controls.Add($label)

    $textBox = New-Object Windows.Forms.TextBox
    $textBox.Left = 12
    $textBox.Top = 40
    $textBox.Width = 580
    $form.Controls.Add($textBox)

    $changesBox = New-Object Windows.Forms.TextBox
    $changesBox.Left = 12
    $changesBox.Top = 78
    $changesBox.Width = 580
    $changesBox.Height = 250
    $changesBox.Multiline = $true
    $changesBox.ReadOnly = $true
    $changesBox.ScrollBars = 'Vertical'
    $changesBox.Text = $preview
    $form.Controls.Add($changesBox)

    $ok = New-Object Windows.Forms.Button
    $ok.Text = 'Commit'
    $ok.Left = 410
    $ok.Top = 340
    $ok.Width = 85
    $ok.DialogResult = [Windows.Forms.DialogResult]::OK
    $form.AcceptButton = $ok
    $form.Controls.Add($ok)

    $cancel = New-Object Windows.Forms.Button
    $cancel.Text = 'Cancel'
    $cancel.Left = 505
    $cancel.Top = 340
    $cancel.Width = 85
    $cancel.DialogResult = [Windows.Forms.DialogResult]::Cancel
    $form.CancelButton = $cancel
    $form.Controls.Add($cancel)

    $result = $form.ShowDialog()
    if ($result -ne [Windows.Forms.DialogResult]::OK) { return '' }
    return $textBox.Text.Trim()
}

Push-Location $root
try {
    $dirty = @(git status --porcelain)
    if (-not $dirty -or $dirty.Count -eq 0) { return }

    $message = Read-CommitMessage $dirty
    if ([string]::IsNullOrWhiteSpace($message)) {
        Write-Output '[auto-commit] skipped: no commit message'
        return
    }

    if (-not (git config user.name))  { git config user.name  'Hagryph' }
    if (-not (git config user.email)) { git config user.email 'hagryph.gaming@gmail.com' }

    git add -A | Out-Null
    git commit -m $message | Out-Null
    if ($LASTEXITCODE -ne 0) { return }

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    git push origin $branch 2>$null | Out-Null
    Write-Output "[auto-commit] committed: $message"
} finally {
    Pop-Location
}
