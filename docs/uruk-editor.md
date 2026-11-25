# Uruk Editor

The Uruks tab opens a nested **Uruk Editor** screen. The title-line back arrow
returns to the previous screen. Subpages do not become extra mod tabs. HagUI ABI
v10 exposes RegisterSubPage, AddSubPageButton and AddPropertyEditor to all mods.
The property widget uses two fields per row, field filtering, a scrolling grid,
and a searchable value dialog. Its callbacks consume snapshots, not native memory.

## Implemented

- Captain selection from all engine faction pools; includes unspawned and dead
  seated captains and preset characters. Members are identified by army, pool
  index, random seed and save-session generation, not a retained object pointer.
- Level 1-85 through the existing native XP setter; native level locks remain.
- Personality, role/title and tribe through their native setters.
- Add, remove or replace trait picker slots. The engine resolves their active
  abilities, including class bonuses, advanced classes, rarity, mounts, gangs,
  epic perks, strengths and weaknesses. Choices come from the live Picker category;
  duplicate and exclusive-group conflicts are filtered and rechecked before edits.
- Weapon and armour item replacement from live Inventory/Items -> ItemBuilder
  records, filtered by weapon/armour slot. Native item construction, character-type
  checks and source replacement priorities remain in force.
- Armour design replacement filtered by ArmorSlot and the captain's body type,
  resolving its SimpleModelPiece from the live design's ModelPicker.
- Read-only inspection of class, combat tree, army, row, location, preset, rarity,
  purpose, scars, level remap, intel, counters, seed, flags, active abilities,
  markers, inventory slots/looks, 23 appearance pieces, 15 designs and 17 palettes.

Equipment and design edits are disabled for spawned captains: a storage-only edit
does not safely refresh an already loaded body's bundles and rig. The engine can
override designs during later appearance rebuilds. Native constraints may reject
or clamp an edit; the resulting state is recaptured and the last result appears
in the grid and Uruks.log. No raw vector allocation, global GameDB edits, save
footer or sidecar is used. Only one task may be pending. Stale save/member/field
selections are rejected.

## Remaining Work

This is not yet a complete editable reflection of every FactionMember field.
Name, base class/model transitions, marker mutation, raw flags, relationships,
arbitrary resolved-trait forcing, palette editing and live-body visual refresh
are not implemented. Their fields are read-only rather than offering unverified
memory writes. The engine exposes no single reflected native object containing
every member field; a GameDB category only enumerates valid records, not the
mutation contract or compatibility requirements of the object consuming them.

Native layout/mutation paths were checked statically. Gameplay editing and
save/reload persistence have not been tested in this change; the game was not
restarted or modified. Automated tests cover snapshot construction, descriptor
types, invalid reads, stale sessions, recruitment policy, configuration and
nonblank ImGui draw data at four desktop resolutions.

## Research

- [SeiKur0's original table thread](https://fearlessrevolution.com/viewtopic.php?t=5132)
  and table download returned access errors; no table scripts were executed.
- [Nemesis objects and saved fields](https://sow-ce.github.io/engine/nemesis.html)
- [Trait slots and native rebuilds](https://sow-ce.github.io/engine/captain-traits.html)
- [Appearance and captain inventory](https://sow-ce.github.io/engine/captain-appearance.html)
- [GameDB descriptor/category layouts](https://sow-ce.github.io/engine/gamedb.html)

Local Ghidra evidence is under C:/dev/re/sow/uruks_editor_*.txt. Native mutation
entry points are centralized in shared/GameOffsets.h and guarded with signatures.
