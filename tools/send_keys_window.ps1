# send_keys_window.ps1 - send keyboard input to a window by process name WITHOUT focusing it
# (PostMessage, so it works while the window is in the background - same spirit as
# screenshot_window.ps1). Reliable for windows that read Win32 messages: our overlay's WndProc
# subclass, and Win32/WM-based UIs.
#
# NOTE: games that read gameplay input via raw input / DirectInput may IGNORE PostMessage'd keys.
# For those, pass -Foreground to focus the window and use real hardware-level input (SendInput).
#
#   .\send_keys_window.ps1 -Keys F8                 # tap F8 (opens our overlay hub)
#   .\send_keys_window.ps1 -Keys SPACE,ENTER        # tap several keys in order
#   .\send_keys_window.ps1 -Text "hello world"      # type text (WM_CHAR)
#   .\send_keys_window.ps1 -Keys A -Foreground      # focus window + SendInput (for raw-input games)
param(
    [string]$ProcessName = 'ShadowOfWar',
    [string[]]$Keys,               # named keys / single chars: F1..F12, SPACE, ENTER, ESC, TAB,
                                   #   UP/DOWN/LEFT/RIGHT, A..Z, 0..9
    [string]$Text,                 # type this string via WM_CHAR
    [int]$HoldMs = 40,             # down->up delay per key
    [switch]$Foreground            # focus the window and use SendInput (real input) instead of PostMessage
)
$ErrorActionPreference = 'Stop'

Add-Type -Namespace Win -Name Key -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
[DllImport("user32.dll", EntryPoint="GetWindowThreadProcessId")] public static extern uint GetWindowThreadProcessIdForPid(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rect);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
[DllImport("user32.dll")] public static extern short VkKeyScanW(char c);
[DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint mapType);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
public struct RECT { public int Left, Top, Right, Bottom; }
'@

# Reliably foreground a window despite Windows' foreground lock: attach our input thread to both the
# current-foreground and target threads, then SetForegroundWindow works.
function Force-Foreground([IntPtr]$h) {
    $fg  = [Win.Key]::GetForegroundWindow()
    $cur = [Win.Key]::GetCurrentThreadId()
    $tTid  = [Win.Key]::GetWindowThreadProcessId($h,  [IntPtr]::Zero)
    $fgTid = [Win.Key]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    [Win.Key]::AttachThreadInput($cur, $fgTid, $true) | Out-Null
    [Win.Key]::AttachThreadInput($cur, $tTid,  $true) | Out-Null
    [Win.Key]::BringWindowToTop($h) | Out-Null
    [Win.Key]::SetForegroundWindow($h) | Out-Null
    [Win.Key]::AttachThreadInput($cur, $tTid,  $false) | Out-Null
    [Win.Key]::AttachThreadInput($cur, $fgTid, $false) | Out-Null
}

# name -> virtual-key code
$VK = @{ SPACE=0x20; ENTER=0x0D; RETURN=0x0D; ESC=0x1B; ESCAPE=0x1B; TAB=0x09; BACK=0x08;
         UP=0x26; DOWN=0x28; LEFT=0x25; RIGHT=0x27; HOME=0x24; END=0x23; DELETE=0x2E; INSERT=0x2D }
0..11 | ForEach-Object { $VK["F$($_+1)"] = 0x70 + $_ }               # F1..F12

function Resolve-Vk([string]$k) {
    $u = $k.ToUpper()
    if ($VK.ContainsKey($u)) { return [int]$VK[$u] }
    if ($u.Length -eq 1) {
        $c = $u[0]
        if ($c -ge 'A' -and $c -le 'Z') { return [int][char]$c }     # VK for A..Z == ASCII
        if ($c -ge '0' -and $c -le '9') { return [int][char]$c }     # VK for 0..9 == ASCII
        return ([Win.Key]::VkKeyScanW($k[0]) -band 0xFF)             # fall back to layout mapping
    }
    throw "unknown key '$k'"
}

function Find-WindowForProcess([string]$Name) {
    $procs = @(Get-Process -Name $Name -ErrorAction Stop)
    $ids = @{}
    foreach ($proc in $procs) { $ids[[uint32]$proc.Id] = $true }
    $script:WindowSearchResult = [IntPtr]::Zero
    $script:WindowSearchArea = 0
    $callback = [Win.Key+EnumWindowsProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $ownerPid = [uint32]0
        [Win.Key]::GetWindowThreadProcessIdForPid($hWnd, [ref]$ownerPid) | Out-Null
        if ($ids.ContainsKey($ownerPid) -and [Win.Key]::IsWindowVisible($hWnd)) {
            $rect = New-Object Win.Key+RECT
            if ([Win.Key]::GetWindowRect($hWnd, [ref]$rect)) {
                $w = $rect.Right - $rect.Left
                $h = $rect.Bottom - $rect.Top
                if ($w -gt 0 -and $h -gt 0) {
                    $area = $w * $h
                    if ($area -gt $script:WindowSearchArea) {
                        $script:WindowSearchResult = $hWnd
                        $script:WindowSearchArea = $area
                    }
                }
            }
        }
        return $true
    }
    [Win.Key]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
    if ($script:WindowSearchResult -eq [IntPtr]::Zero) { throw "process '$Name' has no visible window" }
    return $script:WindowSearchResult
}

$h = Find-WindowForProcess $ProcessName

if ($Foreground) { Force-Foreground $h; Start-Sleep -Milliseconds 250 }

function Tap-Vk([int]$vk) {
    if ($Foreground) {
        $scan = [byte]([Win.Key]::MapVirtualKeyW([uint32]$vk, 0))
        [Win.Key]::keybd_event([byte]$vk, $scan, 0, [UIntPtr]::Zero)         # down
        Start-Sleep -Milliseconds $HoldMs
        [Win.Key]::keybd_event([byte]$vk, $scan, 2, [UIntPtr]::Zero)         # up (KEYEVENTF_KEYUP)
    } else {
        [Win.Key]::PostMessageW($h, 0x100, [IntPtr]$vk, [IntPtr]0) | Out-Null                    # WM_KEYDOWN
        Start-Sleep -Milliseconds $HoldMs
        [Win.Key]::PostMessageW($h, 0x101, [IntPtr]$vk, [IntPtr]::new(0xC0000001)) | Out-Null    # WM_KEYUP
    }
}

if ($Text) {
    foreach ($ch in $Text.ToCharArray()) {
        if ($Foreground) { Tap-Vk (Resolve-Vk ([string]$ch)) }
        else { [Win.Key]::PostMessageW($h, 0x102, [IntPtr][int][char]$ch, [IntPtr]0) | Out-Null } # WM_CHAR
        Start-Sleep -Milliseconds 15
    }
}
if ($Keys) { foreach ($k in $Keys) { Tap-Vk (Resolve-Vk $k); Start-Sleep -Milliseconds 40 } }
Write-Output "sent to $ProcessName (hwnd 0x$($h.ToString('x')))$(if($Foreground){' [foreground/SendInput]'}else{' [background/PostMessage]'})"
