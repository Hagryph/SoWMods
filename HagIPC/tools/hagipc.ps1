# hagipc.ps1 - tiny client for the HagIPC server.
#   .\hagipc.ps1 "ping"                       one command, print reply, exit
#   .\hagipc.ps1 "read 0x141976838 u32"       quote the command (it has spaces)
#   .\hagipc.ps1 -Commands "ping","base"      run several on one connection
#   .\hagipc.ps1                              interactive REPL (blank line / 'quit' to exit)
# Fields in multi-part replies are 0x1f-separated; we print them on their own lines.
[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string]$Command,
    [string[]]$Commands,
    [string]$IPCHost = '127.0.0.1',
    [int]$Port = 19000,
    [string]$Token
)
$ErrorActionPreference = 'Stop'

$client = [Net.Sockets.TcpClient]::new()
$client.Connect($IPCHost, $Port)
$stream = $client.GetStream()
$reader = [IO.StreamReader]::new($stream)
$writer = [IO.StreamWriter]::new($stream); $writer.AutoFlush = $true

# Single-line reliable. hooklog is multi-line: parse "entries=N" and read exactly N more lines +
# the trailing blank (the current deployed server \n-separates; the 0x1f server fix lands next build).
function Send-Line($line) {
    $writer.WriteLine($line)
    $resp = $reader.ReadLine()
    if ($null -eq $resp) { return '(connection closed)' }
    if ($resp -match '^ok entries=(\d+)') {
        $n = [int]$Matches[1]; $extra = @()
        for ($i = 0; $i -lt $n; $i++) { $l = $reader.ReadLine(); if ($null -eq $l) { break }; $extra += $l }
        [void]$reader.ReadLine()   # trailing blank line from Drain's final \n + sendLine \n
        return ((@($resp) + $extra) -join "`n")
    }
    $resp -replace "`u{001f}", "`n"
}

Write-Host (($reader.ReadLine()) -replace "`u{001f}", "`n")   # server greeting
if ($Token) { Write-Host (Send-Line "auth $Token") }

$list = @()
if ($Commands) { $list += $Commands }
if ($Command)  { $list += $Command }

if ($list.Count -gt 0) {
    foreach ($c in $list) { Write-Host "> $c"; Write-Host (Send-Line $c) }
} elseif ([Environment]::UserInteractive) {
    while ($true) {
        $line = Read-Host 'hagipc'
        if ([string]::IsNullOrWhiteSpace($line) -or $line -eq 'quit' -or $line -eq 'exit') { break }
        Write-Host (Send-Line $line)
    }
}
$client.Close()
