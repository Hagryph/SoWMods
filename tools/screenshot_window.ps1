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
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public struct RECT { public int Left, Top, Right, Bottom; }
'@
[Native.Cap]::SetProcessDPIAware() | Out-Null   # physical pixels, not DPI-virtualized

$p = Get-Process -Name $ProcessName -ErrorAction Stop |
     Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p) { throw "process '$ProcessName' has no window" }
$hwnd = $p.MainWindowHandle

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
