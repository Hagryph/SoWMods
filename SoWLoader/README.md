# SoWLoader

The mod loader for Middle-earth: Shadow of War — a hand-written **`steam_api64.dll` proxy**.

## Why steam_api64.dll

It is the one DLL that is BOTH statically imported by `ShadowOfWar.exe` AND physically shipped in
the game's own `x64\` folder (not `System32`/KnownDLLs) — the standard precondition for a DLL-proxy
loader. Steam-DRM titles query it very early at startup, so it is a clean, deterministic injection
point. See `../docs/reverse-engineering.md` and the SoWMods memory `reference_sow-import-table`.

## How it works

- All **796** exports of the real `steam_api64.dll` are **forwarded** to the renamed original
  `steam_api64_org.dll` via `src/steam_api64.def` — so every Steam call resolves straight to the real
  DLL and Steam DRM/functionality is preserved untouched.
- Our own code runs only in `DllMain(DLL_PROCESS_ATTACH)` → `sow::Loader::OnAttach`, which logs and
  spawns a worker thread. **No `LoadLibrary` in `DllMain`** — mod loading is deferred to the worker.

## Build / deploy / restore

```powershell
.\build.ps1      # build + deploy (backs up the real DLL to steam_api64_org.dll first, once)
.\deploy.ps1     # deploy the last build without rebuilding
.\restore.ps1    # remove the proxy, restore vanilla steam_api64.dll
```

Launch the game normally (through Steam). Success = a `SoWLoader.log` appears next to the exe in
`x64\` with `injection point VALIDATED`, and the game still reaches the main menu (proving the
forwarders + Steam DRM are intact).

## Status

Milestone 1: prove the injection point (log-on-load + forward all exports). The loader does not yet
load any mods — that's the next milestone.
