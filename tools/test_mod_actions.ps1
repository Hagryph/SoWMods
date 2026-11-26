# Default: discovery, schema validation and a non-writing generation plan.
# -SaveCaptain explicitly commits the named recipe to the selected seat.
param([string]$Captain='Faction_MinasIthil:271',[switch]$SaveCaptain,[switch]$DiscoveryOnly)
$ErrorActionPreference='Stop'
$client=Join-Path $PSScriptRoot '../HagIPC/tools/mod-action.ps1'
function Request([string]$line) {
    $r=& $client $line
    if(!$r.StartsWith('ok ')){throw "$line -> $r"}
    $r.Substring(3)
}
function Rejected([string]$line,[string]$reason){
    $r=& $client $line
    if(!$r.StartsWith("err $reason")){throw "$line -> unexpected result: $r"}
}
$ids=(Request 'list').Split(' ')
foreach($id in 'uruks.preview','uruks.save','uruks.inspect','inventoryeditor.spawn','inventoryeditor.unlock_ring','uruks.get_recruitment','uruks.set_recruitment'){
    if($id -notin $ids){throw "Missing action: $id"}
    $null=Request "help $id"
}
$layout=(Request 'layout').Split([char]10)
$expected=@('Item Spawner | title | Early Unlock Ring','Item Spawner | catalog | Spawn item','Uruks | toggle | Ignore Recruitment Level','Uruks | submenu | Captain Generation','Uruks/Captain Generation | form | Generate')
$ours=@($layout | Where-Object {$_ -match '^(Item Spawner|Uruks|Loader)[ /]'})
if(Compare-Object $ours $expected){throw "Unexpected generated layout: $ours"}
Rejected 'run unregistered' 'unregistered'
Rejected 'run uruks.set_recruitment enabled=yes' 'expected true or false'
Rejected 'run uruks.set_recruitment enabled=true enabled=false' 'expected unique'
Rejected 'run inventoryeditor.spawn item=invalid count=51' 'out of range'
Rejected 'run uruks.preview nonexistent=x' 'unknown input'
Rejected 'run uruks.save' 'missing ticket'
if((Request 'read uruks.set_recruitment enabled') -ne (Request 'run uruks.get_recruitment')){throw 'Bound setting and handler disagree'}
if((Request 'help uruks.preview') -notmatch 'menu=Captain Generation' -or
   (Request 'help uruks.preview') -notmatch 'advanced:choice \(refresh=3, dependencies=2'){throw 'Missing submenu or dependent choice metadata'}
Rejected 'read uruks.preview missing' 'field has no such source'
Write-Output "Discovery and validation passed: $($ids.Count) actions"
if($DiscoveryOnly){return}
$catalog=Request 'catalog inventoryeditor.spawn'
if($catalog -notmatch '^[1-9][0-9]* rows$'){throw "Empty item catalog: $catalog"}
$ring=Request 'visible inventoryeditor.unlock_ring'
if($ring -notin 'true','false'){throw "Invalid ring visibility: $ring"}
Write-Output "Item Spawner: $catalog; ring action visible=$ring"
$target="captain=$Captain"
$before=Request "run uruks.inspect $target"
$template=Request "preset uruks.preview $target"
if($template -notmatch '(?m)^immunities=' -or $template -notmatch '(?m)^base=ClassMarker_Defender$'){throw 'Template did not return generator settings'}
$base='ClassMarker_Defender'
$specs=(Request "choices uruks.preview advanced $target base=$base").Split([char]10)
if('Marker_Specialization_Tank' -notin $specs){throw 'Tank not offered for Defender'}
$traits=[ordered]@{
    immunities='Immune_Combat,Immune_Curse,Immune_Frost,Immune_Beast,Immune_Poison,Immune_Ranged,Immune_Stealth'
    hates='Hate_BodyguardDefeat,Hate_ExecutionAttacks,Hate_Unstable,Hate_Vault,Hate_EnragedOrcs,Hate_PowerfulEnemies,Hate_Injury'
    class_traits='Quirk_NoLastChance'
    bonuses='Grenade_Curse,ET_GreatToughness,Monster_Slayer,ET_ChampTAN_FullHPDetermination,Poison_Weapon'
}
$kit="$target base=$base advanced=Marker_Specialization_Tank tribe=TribeDef_Warmonger rarity=Marker_OrcRarity_Legendary"
$argsText=($traits.GetEnumerator() | ForEach-Object {"$($_.Key)=$($_.Value)"}) -join ' '
$plan=Request "run uruks.preview $kit $argsText"
if($plan -notmatch '^ticket=(\d+) '){throw "Invalid ticket: $plan"}
$ticket=$Matches[1]
if((Request 'read uruks.save ticket') -ne $ticket){throw 'Save form did not resolve the preview ticket'}
if((Request "run uruks.inspect $target") -ne $before){throw 'Captain changed during preview'}
Write-Output "Preview passed: $plan"
if(!$SaveCaptain){return}
Write-Output (Request "run uruks.save ticket=$ticket")
Rejected "run uruks.save ticket=$ticket" 'unknown or consumed'
$state=(Request "run uruks.inspect $target").Split([char]10)
$wanted=@($traits.Values | ForEach-Object {$_.Split(',')})
foreach($id in $wanted){if($id -notin $state){throw "Missing after save: $id"}}
if('Vulnerable_Grabknockdown' -in $state){throw 'Unexpected Clumsy weakness'}
Write-Output "Live readback: all $($wanted.Count) selected traits active, no Clumsy; preview ticket consumed."
