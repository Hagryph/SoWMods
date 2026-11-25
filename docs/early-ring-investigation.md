# Early Ring Unlock: Investigation

Status: implemented in Inventory Editor, built, and deployed. The action uses
native inventory grants and detects the loaded character's unlock records; it
does not create a sidecar, append save data, or write an unlock flag to an INI.
The action has not been pressed during verification, so native ring-slot behavior
and the later story grant still require an in-game compatibility test.

## Current UI

`Early Unlock Ring` appears above the item list in F8 -> Item Spawner when the
loaded character is eligible and either required ability unlock is missing. It
hides after the native unlock records are detected, including after a normal story
grant or loading an already-unlocked save. All ring-specific behavior is in
`mods/InventoryEditor/src`; the loader only supplies generic conditional buttons
and an in-memory world generation used to reject stale queued work.

The missing button reported in the screenshot was traced to older installed DLLs:
the deployed hashes differed from both new builds, and the running mod log lacked
the ring-action registration. Both DLLs were deployed together and the restarted
game confirmed registration. Visibility changes are logged to InventoryEditor.log
with eligibility and native ability flags. Policy tests cover locked, partial,
unlocked, unknown, pending-action, and stale-world states.

## Verified

- The current standalone mod is `mods/InventoryEditor`, displayed as Item Spawner.
  It registers a save-local page and can queue native calls through `QueueGameTask`.
- `Inventory.Item` descriptor is at preferred VA `0x142700530`. Its row table
  (`descriptor + 0x38`, count at `+0x28`) uses 0x28-byte rows, name at `+0x20`.
- Live records include:
  - `ScriptedUnlock_SkillGrant_BrandGrunts`, key `0x087fec77`.
  - `ScriptedUnlock_SkillGrant_DominateCaptains`, key `0x16b66e0d`.
  - `Primary_6Ring_1Default`, key `0xf81ecf2e`.
- Read-only queries on the running save found the default ring present and both
  scripted unlock records absent. Ring possession alone is not sufficient evidence
  that the requested progression is unlocked.
- `0x140500768` is the Lua `CCharacter.AddInventoryItem` wrapper. It resolves an
  Inventory.Item record and calls `0x1401e6b88(character, row)`.
- `0x1401e6b88` calls `0x1401c01e0(character->inventory at +0x2348, row, 1)`
  then `0x14114e0c8(character)` to queue a character refresh.
- Disassembly of `0x1401c01e0` shows a three-argument wrapper around
  `0x1401c1224`; it returns whether that function's added count is nonzero.
- `0x14024df7c(inventory, row)` is the native possession query used by Lua
  `CCharacter.HasInventoryItem`. The live queries above used this read-only path.
- `0x1401c1224` handles quantities, generated gear data, child items, callbacks,
  and inventory notifications. Replacing this entire function with a generic
  success return would bypass required behavior.

## Rejected Lead

`0x1417e22d0` selects `Player_Ring` for slot enum 8 in an equipment picker.
It is not the progression unlock function.

## Remaining Compatibility Verification

1. Trace the normal story grant to establish whether BrandGrunts, DominateCaptains,
   another record, or a combination controls the ring slot and initial ability.
2. Identify the persisted progression predicate and its load/save path. A process
   boolean or global INI flag must not suppress grants for other saves.
3. Establish the exact duplicate grant call boundary and caller expectations,
   including output parameters and unrelated story side effects.
4. The one-shot button runs through the game task queue and rechecks state at
   execution time. Verify the visible slot/ability changes on a backed-up save.
5. Test repeated clicks, save/reload, switching saves, and the normal story unlock
   after an early grant, on a backed-up save.

`tools/inspect_ring_records.ps1` lists matching live item records using only
`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ)` and
`ReadProcessMemory`. Runtime addresses must never be copied into mod offsets.
