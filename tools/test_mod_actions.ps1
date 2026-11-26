# Default: discovery, schema validation and non-writing template and choice checks.
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
foreach($id in 'uruks.generate','uruks.inspect','inventoryeditor.spawn','inventoryeditor.unlock_ring','uruks.get_recruitment','uruks.set_recruitment'){
    if($id -notin $ids){throw "Missing action: $id"}
    $null=Request "help $id"
}
$layout=(Request 'layout').Split([char]10)
$expected=@('Item Spawner | title | Early Unlock Ring','Item Spawner | catalog | Spawn item','Uruks | toggle | Ignore Recruitment Level','Uruks | form | Generate Captain')
$ours=@($layout | Where-Object {$_ -match '^(Item Spawner|Uruks|Loader)[ /]'})
if(Compare-Object $ours $expected){throw "Unexpected generated layout: $ours"}
Rejected 'run unregistered' 'unregistered'
Rejected 'run uruks.set_recruitment enabled=yes' 'expected true or false'
Rejected 'run uruks.set_recruitment enabled=true enabled=false' 'expected unique'
Rejected 'run inventoryeditor.spawn item=invalid count=51' 'out of range'
Rejected 'run uruks.generate nonexistent=x' 'unknown input'
Rejected 'run uruks.save' 'unregistered'
Rejected 'run uruks.generate' 'missing captain'
if((Request 'read uruks.set_recruitment enabled') -ne (Request 'run uruks.get_recruitment')){throw 'Bound setting and handler disagree'}
if((Request 'help uruks.generate') -match 'menu=|followup=' -or
   (Request 'help uruks.generate') -notmatch 'advanced:choice \(refresh=3, dependencies=2'){throw 'Unexpected generator placement or dependent choice metadata'}
Rejected 'read uruks.generate missing' 'field has no such source'
Write-Output "Discovery and validation passed: $($ids.Count) actions"
if($DiscoveryOnly){return}
$catalog=Request 'catalog inventoryeditor.spawn'
if($catalog -notmatch '^[1-9][0-9]* rows$'){throw "Empty item catalog: $catalog"}
$ring=Request 'visible inventoryeditor.unlock_ring'
if($ring -notin 'true','false'){throw "Invalid ring visibility: $ring"}
Write-Output "Item Spawner: $catalog; ring action visible=$ring"
$target="captain=$Captain"
$before=Request "run uruks.inspect $target"
$template=Request "preset uruks.generate $target"
if($template -notmatch '(?m)^immunities=' -or $template -notmatch '(?m)^base=ClassMarker_Defender$'){throw 'Template did not return generator settings'}
$templateArgs=$template.Split([char]10) -join ' '
function CheckKitChange([string]$field,[string]$value,[string[]]$removeBonuses=@()){
    $lines=@(foreach($line in $template.Split([char]10)){
        if($line.StartsWith("$field=")){"$field=$value"}else{$line}
    })
    $draft=$lines -join ' '
    $result=Request "reconcile uruks.generate $target $draft"
    $expected=@(foreach($line in $lines){
        if($line.StartsWith('bonuses=')){
            $kept=$line.Substring(8).Split(',') | Where-Object {$_ -notin $removeBonuses}
            'bonuses='+($kept -join ',')
        }else{$line}
    })
    if($result -cne ($expected -join [char]10)){throw "Unexpected changes after setting $field to $value : $result"}
}
if((Request "reconcile uruks.generate $target $templateArgs") -cne $template){throw 'Unchanged kit lost selections'}
if($template -match '(?m)^advanced=Marker_Specialization_Tank$' -and
   $template -match '(?m)^tribe=TribeDef_Warmonger$' -and $template -match 'ET_ChampTAN_FullHPDetermination' -and
   $template -match '(?m)^class_traits=Quirk_NoLastChance$'){
    CheckKitChange tribe TribeDef_Dark
    CheckKitChange base ClassMarker_Infantry
    CheckKitChange advanced Marker_Specialization_Commander @('ET_ChampTAN_FullHPDetermination')
}
$unsupported=$templateArgs -replace 'advanced=\S+','advanced=Marker_Specialization_Marksman'
if((Request "reconcile uruks.generate $target $unsupported") -cne 'advanced='){throw 'Unsupported advanced class cleared unrelated fields'}
$offered=@((Request "choices uruks.generate class_traits $target $templateArgs").Split([char]10) | Where-Object {$_})
$selected=($template.Split([char]10) | Where-Object {$_ -like 'class_traits=*'}) -replace '^class_traits=',''
foreach($pick in $selected.Split(',')){if($pick -and $pick -notin $offered){throw "Cannot remove imported class trait: $pick"}}
if('Determination' -in $offered){throw 'Automatic Tank ability was offered as an optional class trait'}
if((Request "choices uruks.generate class_traits $target").Trim()){throw 'Trait options leaked before choosing the kit'}
if((Request "run uruks.inspect $target") -ne $before){throw 'Reading class traits changed the captain'}
Write-Output "Template class traits ($($offered.Count)): $($offered -join ', ')"
$base='ClassMarker_Defender'
$specs=(Request "choices uruks.generate advanced $target base=$base").Split([char]10)
if('Marker_Specialization_Tank' -notin $specs){throw 'Tank not offered for Defender'}
$positionKit="$target base=$base advanced=Marker_Specialization_Tank rarity=Marker_OrcRarity_Legendary class_traits=Quirk_NoLastChance"
$terrorChoices=(Request "choices uruks.generate class_traits $positionKit tribe=TribeDef_Terror").Split([char]10)
if('Quirk_Summoner_Spiders' -notin $terrorChoices){throw 'Terror encounter reward unavailable outside the source seat'}
if('Quirk_Summoner_Spiders' -in (Request "choices uruks.generate class_traits $positionKit tribe=TribeDef_Warmonger").Split([char]10)){throw 'Encounter tribe requirement lost'}
if('Hate_HenchmanDamage' -notin (Request "choices uruks.generate hates $positionKit tribe=TribeDef_Terror").Split([char]10)){throw 'Rank-conditioned Hate unavailable'}
$gangKit="$target base=$base advanced=Marker_Specialization_Tank tribe=TribeDef_Terror rarity=Marker_OrcRarity_Legendary"
$gangs=(Request "choices uruks.generate bonuses $gangKit search=Quirk_HasGang_Normal").Split([char]10)
foreach($gang in 'Quirk_HasGang_Normal_Defenders','Quirk_HasGang_Normal_Savages'){
    if($gang -notin $gangs){throw "Campaign-gated gang unavailable: $gang"}
}
if('Quirk_HasGang_Normal_Savages' -in (Request "choices uruks.generate bonuses $gangKit bonuses=Quirk_HasGang_Normal_Defenders search=Quirk_HasGang_Normal").Split([char]10)){
    throw 'Campaign exception allowed two mutually exclusive gangs'
}
$traits=[ordered]@{
    immunities='Immune_Combat,Immune_Curse,Immune_Frost,Immune_Beast,Immune_Poison,Immune_Ranged,Immune_Stealth'
    hates='Hate_BodyguardDefeat,Hate_ExecutionAttacks,Hate_Unstable,Hate_Vault,Hate_EnragedOrcs,Hate_PowerfulEnemies,Hate_Injury'
    class_traits='Quirk_NoLastChance'
    bonuses='Grenade_Curse,ET_GreatToughness,Monster_Slayer,ET_ChampTAN_FullHPDetermination,Poison_Weapon'
}
$kit="$target base=$base advanced=Marker_Specialization_Tank tribe=TribeDef_Warmonger rarity=Marker_OrcRarity_Legendary"
$argsText=($traits.GetEnumerator() | ForEach-Object {"$($_.Key)=$($_.Value)"}) -join ' '
if(!$SaveCaptain){Write-Output 'Read-only checks passed.';return}
$beforeLevel=$before.Split([char]10) | Where-Object {$_ -match '^level=\d+$'}
if(!$beforeLevel){throw 'Missing original level'}
Write-Output (Request "run uruks.generate $kit $argsText")
$state=(Request "run uruks.inspect $target").Split([char]10)
if($beforeLevel -notin $state){throw 'Generation changed the level'}
if('base=ClassMarker_Defender, advanced=Marker_Specialization_Tank, tribe=TribeDef_Warmonger, rarity=Marker_OrcRarity_Legendary' -notin $state){throw 'Generation changed the requested kit'}
Write-Output "Seat $Captain : $($state[0]), $beforeLevel"
$wanted=@($traits.Values | ForEach-Object {$_.Split(',')})
foreach($id in $wanted){if($id -notin $state){throw "Missing after generation: $id"}}
if('Vulnerable_Grabknockdown' -in $state){throw 'Unexpected Clumsy weakness'}
Write-Output "Live readback: all $($wanted.Count) selected traits active, no Clumsy; original level and kit verified."
