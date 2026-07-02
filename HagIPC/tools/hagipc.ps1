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

function Send-Line($line) {
    $writer.WriteLine($line)
    $resp = $reader.ReadLine()
    if ($null -ne $resp) { $resp -replace "`u{001f}", "`n" } else { '(connection closed)' }
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
