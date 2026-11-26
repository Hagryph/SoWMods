# Uruks

Standalone global submod. First option: **Ignore Recruitment Level**.

The generation form is on the **Uruks** tab. Select a captain and edit all generation settings on that screen.
The loader renders these forms from the [registered actions](../../docs/mod-actions.md).
**Generate Captain** immediately replaces the selected captain in his Army seat at his current level.
Build only; do not deploy beside the old
UrukEditor.dll, since both DLLs would install the same recruitment hooks.

When enabled, the player's recruitment option selection ignores the captain/player
level comparison. The normal predicate still evaluates its other requirements.
The positive recruitment choice becomes eligible and the negative too-high-level
fallback becomes ineligible. This does not unlock the domination story ability or
remove unrelated restrictions.

An over-level captain is normalized to the current player's level after the native
follower transition has completed its recruitment bonuses. Already-owned followers,
failed recruitment, unbranding, and captains originally at/below the player's level
are not normalized. Native level/stat refresh is used instead of writing XP fields.

## Configuration

Installed path: `<game>/mods/cfg/Uruks.ini`.

```ini
[Recruitment]
IgnoreRecruitmentLevel=0
```

Default is off. Running **Ignore recruitment level** writes this global configuration and
applies to all saves. It does not store settings in a save file. The Loader creates
the INI on typed option registration and retains existing valid values. Runtime status and completed
level adjustments are logged to `%LOCALAPPDATA%/SoWLoader/logs/Uruks.log`.

## Build

Build the loader first to provide the static MinHook dependency, then run
`mods/build_mods.ps1 -Only Uruks`. The V4 action schema and Loader Config API v1 are required.

## Native References

- `0x1406b8b14`: interrogation option condition, including `CanRecruitTarget`.
  It compares the target's zero-based level to `0x1406ba018` only when that
  configured requirement is nonzero (1 = allowed, 2 = disallowed).
- `0x1402c4970`: builds actionable interrogation options through that predicate,
  using the interrogating character's inventory and the target character.
- `0x1406ba018`: zero-based player level. Its override is thread-local and limited
  to the local player's interrogation option evaluation. Normal reads are unchanged.
- `0x1407d52b0`: follower transition. It handles level bonuses, live-character
  synchronization, notifications, and recruitment events before returning.
- `0x14035188c`: native faction-member XP level setter and derived/live stat refresh.
  Native locked-level constraints are retained; a non-exact result is logged.

Native prologues are checked before patching. All three hooks are rolled back if
installation fails. Policy tests cover dialogue positive/negative predicates,
disabled behavior, partial/failed transitions, existing followers, and level-one
players. An actual above-level recruitment encounter still needs in-game testing.

### Generator templates and save reports

On the **Uruks** tab, **Take Over Template** replaces the draft settings
with the selected captain's class, advanced class, tribe, rarity, and compatible
optional active traits. It does not copy identity, cosmetics, equipment or fixed
kit grants, and does not modify the captain. Edit the draft and press **Generate Captain** once to apply it.

The generator leaves the role random unless the selected class traits need a
specific role. One role must satisfy all picks that the class cannot supply.
Required weapons reserve their slots before the game fills the captain's
inventory. A two-handed weapon reserves both hands; single-handed weapons
leave the other hand available for compatible native kit items. The save
report records reserved slots alongside the equipment actually observed.

The generator can also use a trait reward from an encounter builder. It checks
the builder's tribe, base class, allowed roles and markers, expands
supported reward prerequisites, and plans the required weapons before replacement.
The encounter's Army rank is not a requirement for keeping its reward.
For a rank-dependent picker the generator can verify the requested output at a
different native rank. The captain keeps his real position and relationships;
the Army screen still shows only traits active and visible there.
When checking or generating a draft, campaign-progress predicates on trait
pickers, roles and supported trait rewards do not require the current save to
have reached that act or unlock. No campaign variable is written. Normal game
updates outside generation still evaluate the actual campaign state; equipment,
class, epic limits and mutually exclusive pickers are checked as before.
Only one outcome of a single-draw list can be used. Builders with additional
unmodelled setup and rewards with unknown actions are not replayed. The generation report records the chosen reward, builder and list.

Generator saves append a timestamped report to
`%LOCALAPPDATA%\SoWLoader\logs\Uruks-generation.log`. It compares requested
traits to the Army seat after the save attempt and lists other active traits
separately. Display eligibility is the game's per-captain check at that moment,
not a guarantee that a particular menu has room to render every eligible row.
