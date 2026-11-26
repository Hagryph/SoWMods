param([Parameter(Mandatory, Position=0)][string]$Command)
$ErrorActionPreference='Stop'
if ($Command -match '[\r\n\x00]' -or [Text.Encoding]::UTF8.GetByteCount($Command) -gt 8190) { throw 'Invalid mod command' }
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.', 'SoW.ModActions.v1', [IO.Pipes.PipeDirection]::InOut)
try {
    $pipe.Connect(4000)
    $bytes=[Text.Encoding]::UTF8.GetBytes($Command+"`n")
    $pipe.Write($bytes,0,$bytes.Length)
    $pipe.Flush()
    $reader=[IO.StreamReader]::new($pipe)
    $reply=$reader.ReadLine()
    if ($null -eq $reply) {throw 'Mod connection closed; inspect game state before retrying'}
    $reply.Replace([char]31,[char]10)
} finally {$pipe.Dispose()}
