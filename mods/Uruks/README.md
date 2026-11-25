# Uruks

Standalone global submod. First option: **Ignore Recruitment Level**.

The **Uruk Editor** sub-screen provides a two-column captain inspector and native,
engine-record-backed editing. See [coverage and limitations](../../docs/uruk-editor.md).
Requires Loader/HagUI ABI v11. Apply stages a popup choice; only Save Uruk changes the captain.
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

Default is off. The HagUI toggle writes this global configuration immediately and
applies to all saves. It does not store settings in a save file. The Loader creates
the INI on typed option registration and retains existing valid values. Runtime status and completed
level adjustments are logged to `%LOCALAPPDATA%/SoWLoader/logs/Uruks.log`.

## Build

Build the loader first to provide the static MinHook dependency, then run
`mods/build_mods.ps1 -Only Uruks`. HagUI ABI v11 and Loader Config API v1 are required.
Older mods retain their append-only API layout.

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
