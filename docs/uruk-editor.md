# Uruk Editor (Steam 1.21)

Open Uruks > Uruk Editor and select a seated Orc or Olog from a campaign pool.
The inspector shows editable properties, not engine-managed state or individual
members of generated equipment/trait sets. Presets, player characters and online pools are not targets.
Spawned, dead and temporary captains cannot be edited.

## Edits and previews

HagUI ABI 11 adds AddPopupEditor without changing the ABI 10 property structs.
Generate or select a candidate, then Apply it to the draft or Cancel. Save Uruk
is the only operation that commits to the game; Discard abandons the draft.
This changes the live captain, not the on-disk format or the autosave schedule.
A draft cannot change captains. The game thread checks the world, captain key
and a fingerprint of the original captain state before generating or saving.

- Name: native army/class name selection, native string localization, and a
  final name-availability check. Never edits the derived 32-byte display buffer.
- Title: the game's weighted role generator checks class, personality, tribe,
  row, size and appearance. Roles with unpreviewed outfit, marker, scar or event
  transitions are excluded. Applying a title invalidates the draft name and
  look. Both show a red exclamation and block Save until regenerated and applied.
- Appearance: builds separate native SimpleModel storage using member-derived
  parameters and a local variation seed, applies the existing outfit, checks
  body, skeleton and title compatibility, and frees the temporary on the game
  thread. Save regenerates the look and compares the pieces, designs, palette,
  tags and material overrides against the accepted result before copying it.
- Weapon: candidates are items from the captain's personality, role, tribe or
  class kit. A temporary native item build resolves the slot and actual look.
  Both native character-type and replacement checks must pass.
- 3D preview (HagUI ABI 12): open the captain's native Army detail view, then
  open the editor. With no prior editor selection it picks the Army selection.
  Generate an appearance or select a weapon to see the animated Army scene in
  the popup **before Apply**. The preview deep-copies the summary, changes only
  that copy, and uses the normal stage-character redraw. The Loader copies the
  game frame on the GPU before drawing the overlay; it does not render a second
  mesh or change the camera. No preview is shown without the matching Army
  selection. Apply, Cancel, closing the hub, or changing candidates releases
  the temporary stage look. Save Uruk remains the only campaign write.
- Engine-managed fields and equipment slots without editable choices are hidden.

## Traits and progression

Two different engine paths apply. Initial generation is not the limit on what a
captain can earn.

1. **Trait list** starts with zero selections (or reopens the staged list).
   Check and uncheck individual picker records. The remaining choices must
   coexist with every selected record in at least one observed set: the captain's
   existing set or one of up to 32 native generic-builder rolls on private storage.
   This is a conservative sample, not an exhaustive compatibility database.
   Apply stages the replacement list, including an explicitly empty list;
   Cancel preserves the previous draft. Save Uruk commits it. Builder-specific,
   trait-locked and reward-history captains remain protected.
2. **Level-up reward** previews a one-level advancement. The native XP path
   (0x14034F9F0) collects scheduled lists (0x1403513DC), picks a weighted reward
   (0x140350500), tests eligibility (0x140105AEC) and applies it (0x140351AF8).
   The preview uses these list/selection checks on the projected level and shows
   potential individual picker gains and removals from the chosen reward.
   At Save the normal XP setter runs. Only during that call, for that captain,
   list and level, the mod can replay the accepted reward once, and only if the
   engine awarded a roll and the reward is still eligible with positive weight.
   The engine controls the reward's internal random choice and any other rolls.
   If the expected list did not fire, the result reports the native fallback.
   An ordinary level change remains available without selecting a reward.

**Training Order checks** are advisory, not permission to grant a trait or use
a paid order. The editor runs the native contract failure evaluator
(0x140FBC6D0) against a private copy of the selected captain's army summary.
It projects the chosen name, role and level; for generated picker sets it uses
the native slot resolver on private slots to project active traits. Other
summary fields remain at their existing values; predicates can also consult
the current world. An absent summary is unknown, not eligible. A rejected order
is not necessarily an invalid captain. No contract is consumed or dispatched.

## Verification

Native call shapes were checked against the local unpacked executable in Ghidra.
Generator entry points are fingerprinted before use. Fixture tests cover draft
isolation, title dependencies, disabled choices, stale sessions, and Save
dispatch. These are not in-game generation, rendering or save/reload tests.

Background: [Nemesis system](https://sow-ce.github.io/engine/nemesis.html),
[traits](https://sow-ce.github.io/engine/captain-traits.html),
[appearance](https://sow-ce.github.io/engine/captain-appearance.html),
[Army summaries](https://sow-ce.github.io/engine/ui.html).
