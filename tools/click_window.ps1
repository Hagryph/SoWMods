# click_window.ps1 - send a mouse click to a window by process name at CLIENT coordinates, WITHOUT
# focusing it (PostMessage), same background spirit as screenshot_window.ps1. X,Y are client-area
# pixels (0,0 = top-left of the window's client region), matching what screenshot_window.ps1 captures.
#
# NOTE: games that read gameplay input via raw input / DirectInput may IGNORE PostMessage'd clicks.
# For those, pass -Foreground to focus the window and issue a real hardware click (SendInput) at the
# equivalent SCREEN position.
#
#   .\click_window.ps1 -X 285 -Y 820                    # left-click client (285,820)
#   .\click_window.ps1 -X 285 -Y 820 -Button Right
#   .\click_window.ps1 -X 285 -Y 820 -Double
#   .\click_window.ps1 -X 285 -Y 820 -Foreground        # real click (for raw-input games)
param(
    [string]$ProcessName = 'ShadowOfWar',
    [Parameter(Mandatory)][int]$X,
    [Parameter(Mandatory)][int]$Y,
    [ValidateSet('Left','Right','Middle')][string]$Button = 'Left',
    [switch]$Double,
    [switch]$Foreground
)
$ErrorActionPreference = 'Stop'

Add-Type -Namespace Win -Name Mouse -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
public struct POINT { public int X, Y; }
'@

# Reliably foreground despite Windows' foreground lock (see send_keys_window.ps1).
function Force-Foreground([IntPtr]$h) {
    $fg  = [Win.Mouse]::GetForegroundWindow()
    $cur = [Win.Mouse]::GetCurrentThreadId()
    $tTid  = [Win.Mouse]::GetWindowThreadProcessId($h,  [IntPtr]::Zero)
    $fgTid = [Win.Mouse]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    [Win.Mouse]::AttachThreadInput($cur, $fgTid, $true) | Out-Null
    [Win.Mouse]::AttachThreadInput($cur, $tTid,  $true) | Out-Null
    [Win.Mouse]::BringWindowToTop($h) | Out-Null
    [Win.Mouse]::SetForegroundWindow($h) | Out-Null
    [Win.Mouse]::AttachThreadInput($cur, $tTid,  $false) | Out-Null
    [Win.Mouse]::AttachThreadInput($cur, $fgTid, $false) | Out-Null
}

$p = Get-Process -Name $ProcessName -ErrorAction Stop |
     Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p) { throw "process '$ProcessName' has no window" }
$h = $p.MainWindowHandle

# per-button message + wParam key-state flags
$msg = switch ($Button) {
    'Left'   { @{ down=0x201; up=0x202; mk=0x0001 } }   # WM_LBUTTONDOWN/UP, MK_LBUTTON
    'Right'  { @{ down=0x204; up=0x205; mk=0x0002 } }   # WM_RBUTTONDOWN/UP, MK_RBUTTON
    'Middle' { @{ down=0x207; up=0x208; mk=0x0010 } }   # WM_MBUTTONDOWN/UP, MK_MBUTTON
}
$lp = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
$times = if ($Double) { 2 } else { 1 }

if ($Foreground) {
    $pt = New-Object Win.Mouse+POINT; $pt.X = $X; $pt.Y = $Y
    [Win.Mouse]::ClientToScreen($h, [ref]$pt) | Out-Null
    Force-Foreground $h; Start-Sleep -Milliseconds 200
    [Win.Mouse]::SetCursorPos($pt.X, $pt.Y) | Out-Null; Start-Sleep -Milliseconds 40
    $dn = switch ($Button) { 'Left' {0x0002} 'Right' {0x0008} 'Middle' {0x0020} }  # MOUSEEVENTF_*DOWN
    $up = switch ($Button) { 'Left' {0x0004} 'Right' {0x0010} 'Middle' {0x0040} }  # MOUSEEVENTF_*UP
    for ($i = 0; $i -lt $times; $i++) {
        [Win.Mouse]::mouse_event($dn, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 30
        [Win.Mouse]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
    }
} else {
    [Win.Mouse]::PostMessageW($h, 0x200, [IntPtr]0, $lp) | Out-Null                 # WM_MOUSEMOVE
    for ($i = 0; $i -lt $times; $i++) {
        [Win.Mouse]::PostMessageW($h, [uint32]$msg.down, [IntPtr]$msg.mk, $lp) | Out-Null
        Start-Sleep -Milliseconds 30
        [Win.Mouse]::PostMessageW($h, [uint32]$msg.up, [IntPtr]0, $lp) | Out-Null
        Start-Sleep -Milliseconds 40
    }
}
Write-Output "$Button click x$times at client ($X,$Y) -> $ProcessName$(if($Foreground){' [foreground/SendInput]'}else{' [background/PostMessage]'})"
