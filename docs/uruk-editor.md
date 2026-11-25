# Uruk Editor (Steam 1.21)

Open Uruks > Uruk Editor and select a seated Orc or Olog from a campaign pool.
The inspector shows editable properties, not engine-managed state or individual
members of generated equipment/trait sets. Presets, player characters and online pools are not targets.
Spawned, dead and temporary captains cannot be edited.

## Regenerate Captain (HagUI ABI 14)

Open **Uruks > Regenerate Captain**, select the captain to replace, then choose
the base class, advanced class, tribe and rarity. The last selector prepares
a private profile at the selected captain's level. Pick the base class before
the advanced class. Changing base clears an incompatible advanced class. Tribe
membership does not depend on class; individual tribal attacks can be restricted.
Core, class and tribal
grants are automatic; there is no Class Traits checklist. The seven optional
checklists start empty. Empty means **no extra picks**, not an empty class kit.
Changing a selector clears optional picks and prepares a new kit.
Select **Generate** at the bottom left to check the assembled profile or retry
kit preparation. Apply
accepts that preview; editing the recipe invalidates it. Only **Save Uruk**
requests an in-place replacement. No second Army slot or custom save file is used.

Choices are trait definitions linked to the Army menu's UI groups, not picker
names or samples from previous rolls. The editor checks alternative picker
routes, exclusive groups, class markers and rarity limits. The next optional
pick is checked against the fixed profile and current choices on the game thread. Final generation
also checks the actual profile: a picker with several possible outputs has to
resolve to the requested definition at this seat and level. A rejected **profile**
does not remove that trait from the catalog.

The preflight uses the chosen faction combat tree and tribe, generates a
personality, look, role, markers and natural picker list, and keeps unmanaged
non-Army slots (including dormant ones). It takes class grants from compatible
advanced-class reward records and tribal attacks from the selected tribe;
native marker gates still apply. It replays the merged list through the
explicit-grant builder before offering Apply. The live build uses the native
same-seat replacement path, retaining level and affiliation; catch-up XP rewards
are suppressed only for the replacement being built. Normal later level-ups
are unaffected. Scripted scar/event roles are not previewed.

The save operation rebuilds the live captain. It verifies the resulting seat,
level, markers, pickers and resolved traits, but is **not a rollback mechanism**.
Test the first replacement and save/reload on a backed-up campaign.

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

1. Use **Regenerate Captain** to assemble optional traits around an automatic
   kit. The inspector does not expose a second raw picker list.
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
