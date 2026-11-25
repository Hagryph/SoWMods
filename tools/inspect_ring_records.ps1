# Read-only inspection of the live Inventory.Item table; never calls game functions.
[CmdletBinding()]
param(
    [string]$Pattern = 'Ring|ScriptedUnlock|BrandGrunts|DominateCaptains',
    [long]$DescriptorAddress = 0
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RingRecordReader {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        byte[] buffer, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@
$game = Get-Process ShadowOfWar -ErrorAction Stop | Select-Object -First 1
$handle = [RingRecordReader]::OpenProcess(0x1010, $false, $game.Id)
if ($handle -eq [IntPtr]::Zero) { throw 'Cannot open game for read-only inspection.' }
function Read-Bytes([long]$Address, [int]$Count) {
    $buffer = [byte[]]::new($Count)
    $read = [UIntPtr]::Zero
    if (-not [RingRecordReader]::ReadProcessMemory($handle, [IntPtr]$Address,
        $buffer, [UIntPtr]$Count, [ref]$read) -or $read.ToUInt64() -ne $Count) {
        throw ('Cannot read 0x{0:x} ({1} bytes)' -f $Address, $Count)
    }
    return ,$buffer
}
try {
    $base = $game.MainModule.BaseAddress.ToInt64()
    $descriptor = $DescriptorAddress
    if (-not $descriptor) {
        $descriptor = [BitConverter]::ToInt64((Read-Bytes ($base + 0x2700530) 8), 0)
    }
    if ($descriptor -eq 0) { throw 'Inventory.Item descriptor is not loaded.' }
    $header = Read-Bytes $descriptor 0x40
    $count = [BitConverter]::ToUInt32($header, 0x28)
    $table = [BitConverter]::ToInt64($header, 0x38)
    if ($count -gt 100000 -or $table -eq 0) { throw 'Unexpected item table layout.' }
    $rows = Read-Bytes $table ([int]$count * 0x28)
    for ($i = 0; $i -lt $count; ++$i) {
        $nameAddress = [BitConverter]::ToInt64($rows, $i * 0x28 + 0x20)
        if ($nameAddress -eq 0) { continue }
        $bytes = Read-Bytes $nameAddress 128
        $end = [Array]::IndexOf($bytes, [byte]0)
        if ($end -lt 0) { $end = $bytes.Length }
        $name = [Text.Encoding]::ASCII.GetString($bytes, 0, $end)
        if ($name -match $Pattern) {
            [pscustomobject]@{
                Name = $name
                Row = '0x{0:x}' -f ($table + $i * 0x28)
                Key = '0x{0:x8}' -f [BitConverter]::ToUInt32($rows, $i * 0x28 + 4)
            }
        }
    }
} finally {
    [void][RingRecordReader]::CloseHandle($handle)
}
