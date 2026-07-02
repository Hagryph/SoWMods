# inject.ps1 - HOT-LOAD HagIPC.dll into the already-running ShadowOfWar.exe.
# Classic LoadLibrary injection: VirtualAllocEx the DLL path into the target, then CreateRemoteThread
# on kernel32!LoadLibraryW. kernel32 is mapped at the same base in every process on a given boot, so
# LoadLibraryW's address in THIS process equals its address in the target. Re-running is safe:
# LoadLibrary of an already-loaded DLL just bumps the refcount (DllMain PROCESS_ATTACH won't re-run).
[CmdletBinding()]
param(
    [string]$Dll = (Join-Path $PSScriptRoot '..\build\Release\HagIPC.dll'),
    [string]$Process = 'ShadowOfWar'
)
$ErrorActionPreference = 'Stop'

$Dll = (Resolve-Path $Dll).Path
$proc = Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw "process '$Process' is not running - launch the game first." }
Write-Host "target: $Process (pid $($proc.Id))"
Write-Host "dll   : $Dll"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Inj {
  [DllImport("kernel32", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool inh, int pid);
  [DllImport("kernel32", SetLastError=true)] public static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, uint size, uint type, uint prot);
  [DllImport("kernel32", SetLastError=true)] public static extern bool VirtualFreeEx(IntPtr p, IntPtr addr, uint size, uint type);
  [DllImport("kernel32", SetLastError=true)] public static extern bool WriteProcessMemory(IntPtr p, IntPtr addr, byte[] buf, uint size, out UIntPtr wrote);
  [DllImport("kernel32", SetLastError=true)] public static extern IntPtr GetModuleHandle(string name);
  [DllImport("kernel32", SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr mod, string name);
  [DllImport("kernel32", SetLastError=true)] public static extern IntPtr CreateRemoteThread(IntPtr p, IntPtr sa, uint stack, IntPtr start, IntPtr param, uint flags, out uint tid);
  [DllImport("kernel32", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32", SetLastError=true)] public static extern bool GetExitCodeThread(IntPtr h, out uint code);
  [DllImport("kernel32", SetLastError=true)] public static extern bool CloseHandle(IntPtr h);
}
"@

$PROCESS_ALL = 0x1F0FFF
$MEM_COMMIT_RESERVE = 0x3000
$MEM_RELEASE = 0x8000
$PAGE_RW = 0x04

$h = [Inj]::OpenProcess($PROCESS_ALL, $false, $proc.Id)
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error())) - run this shell as Administrator." }

try {
    $bytes = [Text.Encoding]::Unicode.GetBytes($Dll + "`0")
    $remote = [Inj]::VirtualAllocEx($h, [IntPtr]::Zero, [uint32]$bytes.Length, $MEM_COMMIT_RESERVE, $PAGE_RW)
    if ($remote -eq [IntPtr]::Zero) { throw "VirtualAllocEx failed" }

    $wrote = [UIntPtr]::Zero
    if (-not [Inj]::WriteProcessMemory($h, $remote, $bytes, [uint32]$bytes.Length, [ref]$wrote)) { throw "WriteProcessMemory failed" }

    $k32 = [Inj]::GetModuleHandle("kernel32.dll")
    $loadLib = [Inj]::GetProcAddress($k32, "LoadLibraryW")
    if ($loadLib -eq [IntPtr]::Zero) { throw "GetProcAddress(LoadLibraryW) failed" }

    $tid = 0
    $th = [Inj]::CreateRemoteThread($h, [IntPtr]::Zero, 0, $loadLib, $remote, 0, [ref]$tid)
    if ($th -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }

    [Inj]::WaitForSingleObject($th, 10000) | Out-Null
    $code = 0
    [Inj]::GetExitCodeThread($th, [ref]$code) | Out-Null
    # exit code = low 32 bits of the loaded HMODULE (0 = LoadLibrary failed in target)
    if ($code -eq 0) { Write-Warning "LoadLibraryW returned 0 in target - DLL failed to load (missing dep? wrong arch?)" }
    else { Write-Host ("injected OK (module handle low32 = 0x{0:x})" -f $code) }

    [Inj]::VirtualFreeEx($h, $remote, 0, $MEM_RELEASE) | Out-Null
    [Inj]::CloseHandle($th) | Out-Null
}
finally {
    [Inj]::CloseHandle($h) | Out-Null
}
Write-Host "done. Connect with tools\hagipc.ps1 (default 127.0.0.1:19000)."
