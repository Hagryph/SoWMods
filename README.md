# SoWMods

Workspace for Hagryph's **Middle-earth: Shadow of War** (Steam, WB/Monolith) mods.
Each mod is a self-contained C++ plugin in its own subfolder **and its own public GitHub repo**.

This is the sibling of `SkyrimMods`, carrying over the same **deliberate, fully-manual
reverse-engineering** approach — **no existing modding framework** (no SKSE analogue, no
CommonLib, no Script Extender, no community loader). We find every address by hand in Ghidra
and build our own loader and hooks.

## Per-mod workflow

1. **Folder:** `SoWMods/<ModName>/` — a complete CMake + vcpkg project.
2. **Repo:** public GitHub repo `Hagryph/<ModName>`, pushed to `origin/main`.
3. **Auto-commit:** the Claude Code *Stop* hook at `.claude/hooks/auto-commit.ps1` commits + pushes
   every changed mod repo after each prompt. Open Claude at this `SoWMods` root so the hook covers all mods.
4. **Build:** MSVC x64 (VS 2022) + Ninja, vcpkg manifest mode.
5. **Deploy:** `<ModName>/deploy.ps1` copies the built DLL (+ assets) into the game folder
   (`...\steamapps\common\ShadowOfWar\x64\`). Shadow of War has **no Mod Organizer 2 support**, so
   we deploy directly (via our own proxy-DLL loader) rather than through a VFS.

## Conventions (carried from the SkyrimMods / LoL / WoW projects)

- **Always research the current approach online** before building (engine, RE technique, API).
- **Full OOP** — all code lives inside classes/namespaces/RAII; the only free functions are
  unavoidable ABI exports, which immediately delegate to a class.
- **Trampoline hooks only** — hook + call the original (MinHook), never destructive detours, so
  mods stay mutually compatible.
- **Trace from primitives** — never infer engine behaviour; follow the chain from the lowest known
  primitive (a Win32 import, a concrete vtable slot) in every direction (callers AND callees).
- **Golden/black theme** — any UI matches the house look (see `docs/`).

## Tech stack (deliberate "manual" approach)

- **Loader:** our own hand-written **proxy DLL** (a system DLL the game already imports — e.g.
  `dinput8` / `version` / `winmm`, TBD once RE confirms a safe candidate). Shadow of War ships **no
  script extender**, so the loader is ours to build. First job of the RE pass is to pick the injection point.
- **Ghidra 12.1.2** (`C:\dev\ghidra`, JDK 21 at `C:\dev\jdk`) to find addresses by hand.
- **MinHook** for x64 trampoline hooks + forwarders.
- **UI:** approach **to be determined by RE** — Shadow of War is not a Scaleform/SKSE game; we first
  identify its native UI/HUD system and prefer hooking it (integrated, no external overlay) over
  bolting on a D3D-present overlay. Decide only after the engine's UI stack is mapped.

## Target binary & DRM

- Game: `C:\Program Files (x86)\Steam\steamapps\common\ShadowOfWar\x64\ShadowOfWar.exe`
  (x64, PE32+, image base `0x140000000`, ~112 MB — Denuvo bloat).
- **DRM:** outer **SteamStub v3.1 (x64)** wrapper + **Denuvo Anti-Tamper** underneath (Steam build).
  Strip SteamStub with **Steamless** first, then analyze the `.unpacked.exe`. Denuvo stays woven into
  `.text`; most engine code is still analyzable, Denuvo-mutated trigger functions are not.
  See `docs/reverse-engineering.md`.

## Layout

| Path | Purpose |
|------|---------|
| `shared/` | Cross-mod headers (`GameOffsets.h` — the hand-found RVA table). |
| `tools/ghidra-scripts/` | Reusable Ghidra headless scripts (Trace, VtableDump, RTTI, FindXrefs, …) + `run_ghidra.ps1`. |
| `tools/analyze_sow.ps1` | One-time import + full auto-analysis launcher. |
| `docs/` | RE notes and design docs. |
| `.claude/` | Stop-hook auto-commit + settings. |
