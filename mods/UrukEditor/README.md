# UrukEditor

Standalone global submod. First option: **Ignore Recruitment Level**.

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

Installed path: `<game>/mods/cfg/UrukEditor.ini`.

```ini
[Recruitment]
IgnoreRecruitmentLevel=0
```

Default is off. The HagUI toggle writes this global configuration immediately and
applies to all saves. It does not store settings in a save file. Default config
deployment never overwrites an existing configuration. Runtime status and completed
level adjustments are logged to `%LOCALAPPDATA%/SoWLoader/logs/UrukEditor.log`.

## Build

Build the loader first to provide the static MinHook dependency, then run
`mods/build_mods.ps1 -Only UrukEditor`. HagUI ABI v9 is required for the persistent,
callback-backed setting toggle. Older mods retain their append-only API layout.

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
