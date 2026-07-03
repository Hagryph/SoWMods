# screenshot_window.ps1 - capture a window into a PNG even when it is NOT in the foreground.
# Default target is the Shadow of War game window; -ProcessName retargets any other window.
# Uses PrintWindow with PW_RENDERFULLCONTENT (0x2), which asks DWM for the composed surface,
# so D3D11-presented content is captured without bringing the window to the front.
#
#   .\screenshot_window.ps1                          -> %TEMP%\ShadowOfWar_<timestamp>.png
#   .\screenshot_window.ps1 -OutPath C:\x\shot.png
#   .\screenshot_window.ps1 -ProcessName Notepad
#
# Prints the saved PNG path on success.
param(
    [string]$ProcessName = "ShadowOfWar",
    [string]$OutPath
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Native -Name Cap -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
[DllImport("user32.dll", EntryPoint="GetWindowThreadProcessId")] public static extern uint GetWindowThreadProcessIdForPid(IntPtr hWnd, out uint pid);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
[Native.Cap]::SetProcessDPIAware() | Out-Null   # physical pixels, not DPI-virtualized

function Find-WindowForProcess([string]$Name) {
    $procs = @(Get-Process -Name $Name -ErrorAction Stop)
    $ids = @{}
    foreach ($proc in $procs) { $ids[[uint32]$proc.Id] = $true }
    $script:WindowSearchResult = [IntPtr]::Zero
    $script:WindowSearchArea = 0
    $callback = [Native.Cap+EnumWindowsProc]{
        param([IntPtr]$hWnd, [IntPtr]$lParam)
        $ownerPid = [uint32]0
        [Native.Cap]::GetWindowThreadProcessIdForPid($hWnd, [ref]$ownerPid) | Out-Null
        if ($ids.ContainsKey($ownerPid) -and [Native.Cap]::IsWindowVisible($hWnd)) {
            $rect = New-Object Native.Cap+RECT
            if ([Native.Cap]::GetWindowRect($hWnd, [ref]$rect)) {
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
    [Native.Cap]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
    if ($script:WindowSearchResult -eq [IntPtr]::Zero) { throw "process '$Name' has no visible window" }
    return $script:WindowSearchResult
}

$hwnd = Find-WindowForProcess $ProcessName

$r = New-Object Native.Cap+RECT
if (-not [Native.Cap]::GetWindowRect($hwnd, [ref]$r)) { throw "GetWindowRect failed" }
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { throw "window has zero size ($w x $h)" }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok  = [Native.Cap]::PrintWindow($hwnd, $hdc, 2)   # 2 = PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
if (-not $ok) { $bmp.Dispose(); throw "PrintWindow failed (window minimized?)" }

if (-not $OutPath) {
    $OutPath = Join-Path $env:TEMP ("{0}_{1}.png" -f $ProcessName, (Get-Date -Format yyyyMMdd_HHmmss))
}
$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output $OutPath
