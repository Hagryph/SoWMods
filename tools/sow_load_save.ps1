# Continue the existing campaign through the loader. No window input or capture.
param([ValidateRange(10,300)][int]$TimeoutSeconds=150)
$ErrorActionPreference='Stop'
$client=Join-Path $PSScriptRoot '../HagIPC/tools/mod-action.ps1'
$until=[datetime]::UtcNow.AddSeconds($TimeoutSeconds)
do {
    $reply=& $client 'run loader.load_save'
    if($reply -eq 'err A save is already loaded.'){return (& $client 'run loader.load_status')}
    if($reply -eq 'err Waiting for the active player profile at the title screen.'){
        $state=& $client 'run loader.load_status'
        if($state.StartsWith('ok World loaded;')){return $state}
        Start-Sleep -Milliseconds 500
    } else {break}
} while([datetime]::UtcNow -lt $until)
if(!$reply.StartsWith('ok ')){throw $reply}
do {
    $state=& $client 'run loader.load_status'
    if($state.StartsWith('ok World loaded;')){return $state}
    if($state -notmatch '^ok (Queued|Starting|Waiting)'){throw $state}
    Start-Sleep -Milliseconds 400
} while([datetime]::UtcNow -lt $until)
throw 'Timed out waiting for a campaign session.'
