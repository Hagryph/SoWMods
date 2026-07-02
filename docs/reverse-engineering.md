# Shadow of War — Reverse-Engineering Notes

Running log of confirmed facts about the target binary and the RE toolchain. Append verified
findings here; keep guesses out (mark anything unverified explicitly).

## Target binary

| Fact | Value |
|------|-------|
| Game path | `C:\Program Files (x86)\Steam\steamapps\common\ShadowOfWar\` |
| Executable | `x64\ShadowOfWar.exe` |
| Size | ~112 MB (Denuvo bloat) |
| Arch | x64, PE32+ (machine `0x8664`, optional magic `0x20B`) |
| Image base | `0x140000000` |
| Sections | 17 |
| Steam AppID | 356190 |
| Engine | Monolith proprietary (LithTech lineage suspected) — **to be confirmed via RE** |

## DRM (two layers)

1. **Outer: SteamStub Variant 3.1 (x64)** — a `.bind` section stub. Removed with
   **Steamless** (`C:\dev\steamless\Steamless.CLI.exe`). Notably, Steamless reported the **code
   section was NOT encrypted** by SteamStub (unlike Skyrim SE), so `.text` is plaintext on disk once
   the stub is stripped.
2. **Inner: Denuvo Anti-Tamper** — present on the Steam build (the GOG build is DRM-free). Denuvo is
   woven into `.text` (mutated/virtualized trigger functions + inline checks); it is NOT a separate
   encrypted section, so the bulk of the code decompiles fine while Denuvo-protected functions read as
   opaque blobs. Steamless does not touch Denuvo. Runtime MinHook trampolines generally still work on
   Denuvo titles (Denuvo targets static cracking, not in-process hooks) — to be validated live.

## Working copies & Ghidra project

| Item | Path |
|------|------|
| Clean copy (no spaces/parens) | `C:\dev\re\sow\ShadowOfWar.exe` |
| Steamless output (ANALYZE THIS) | `C:\dev\re\sow\ShadowOfWar.exe.unpacked.exe` |
| Ghidra project | `C:\dev\re\sow\ghidra-proj\ShadowOfWar` (program `ShadowOfWar.exe.unpacked.exe`) |
| Analysis log | `C:\dev\re\sow\analyze.log` |

## Toolchain

- **Ghidra** 12.1.2 at `C:\dev\ghidra\ghidra_12.1.2_PUBLIC`; **JDK 21** at `C:\dev\jdk\jdk-21.0.11+10`
  (set `JAVA_HOME`; system Java 1.8 is too old). `MAXMEM=24G` for the headless run (63 GB box).
- **analyzeHeadless GOTCHA:** its `.bat` breaks on paths with spaces/parens — always analyze the copy
  under `C:\dev\re\sow\` (clean path), never the `Program Files (x86)` original.
- One-time import+analyze: `tools/analyze_sow.ps1`. Query the analyzed DB: `tools/ghidra-scripts/run_ghidra.ps1 <Script.java> <args>`.

## Address convention

All RVAs recorded in `shared/GameOffsets.h` are file RVAs off `0x140000000`. At runtime convert with
`FromRVA()` = `GetModuleHandle(NULL) + (fileRVA - 0x140000000)`.

## Verified findings (post-analysis, 2026-07-01)

**Full Ghidra auto-analysis completed** — `Analysis succeeded`, `Import succeeded`. Total analyzer
time **3612 sec (~60 min)** for the 112 MB unpacked exe (JAVA_HOME=JDK21, MAXMEM=24G). For reference,
SkyrimSE.exe (~37 MB, no Denuvo) took 40-50 min — size alone doesn't scale linearly with Denuvo present.

**Section layout is Denuvo-mutated.** 17 sections total, several with non-standard MSVC names:
`.arch` (30 MB), `.sdata`, `.xtext` (8.5 MB), `.idata`, `.pdata`, `.data1`, `.link`, `.tls`, `.edata`,
`.bss` (73 MB), `.rsrc`, **`.text` (only 20 bytes!)**, `.sbss`, `.text1`, `.reloc`, `.srdata`. A
near-empty `.text` with large `.xtext`/`.arch` sections is a known Denuvo fingerprint — the protector
relocates the real code out of the nominal `.text` section. Treat `.xtext`/`.arch` as where the actual
game code lives; expect Denuvo-mutated/virtualized stretches within them.

**Static imports (21 DLLs)** — parsed directly from the PE import table (file offset math in
`docs/`, not yet scripted): `advapi32`, the `api-ms-win-crt-*` set, `bink2w64`, `gdi32`, `kernel32`,
`msvcp140`, `ole32`, `shell32`, `shlwapi`, **`steam_api64`**, `user32`, `vcruntime140`, `winhttp`,
`wininet`.

**Delay-loaded imports (2):** `d3d11.dll`, `amd_ags_x64.dll` → **confirms a DirectX 11 renderer**
(no `d3d12`/`dxgi` anywhere in either import table — the game likely calls the single-shot
`D3D11CreateDeviceAndSwapChain`, which pulls `dxgi` in transitively without a direct import).

**DLLs shipped in `x64\` alongside the exe but NOT in either import table** (loaded via
`LoadLibrary`/`GetProcAddress` at runtime, not by name at load time): `AnselSDK64.dll` (NVIDIA Ansel
photo mode), `oo2core_5_win64.dll` (Oodle — decompresses the `*.arch06` archives). Also present:
`steam_appid.txt` (356190), `default.archcfg` (archive manifest, unexamined).

**Loader injection point — leading candidate: proxy `steam_api64.dll`.** It is BOTH statically
imported by name AND physically shipped in the game's own `x64\` folder (not `System32`/KnownDLLs) —
the classic, well-established precondition for a DLL-proxy mod loader (rename the real
`steam_api64.dll`, drop a forwarding stub with that name that also loads our loader DLL, forward every
export to the renamed original). Steam-DRM titles query it very early at startup, giving a
deterministic early injection point. `bink2w64.dll` is a viable fallback (same side-loaded property)
but only gets called at cutscene playback — much later and less deterministic. **Not yet live-tested**
— next step is to validate the proxy/forward scheme actually loads before building anything on it.

## RE plan (next steps)

1. ~~Confirm engine identity + DirectX version~~ — **done: D3D11, via delay-load table.**
2. ~~Map the loader injection point~~ — **candidate identified: `steam_api64.dll` proxy, pending live validation.**
3. Live-validate the `steam_api64.dll` proxy/forward scheme (minimal DLL, log-on-load, forward all exports).
4. Identify the main loop / tick, input, and UI/HUD subsystems (trace from Win32 primitives outward).
5. Enumerate RTTI (`.?AV…@@`) class roster to seed the object model.
6. Inspect `default.archcfg` and the `.arch06` container format if asset access becomes relevant.

## START menu entry registration — deep RE (2026-07-02)

Goal: find where the START menu entries (Start / WBPlay / Options / Run Benchmark / Quit) get
registered, to add a native "HagUI" item. **Result: the menu is fully DATA-DRIVEN — there is no
hardcoded "add these entries" initializer.** Architecture (from `C:\dev\re\sow\re\*`):

- **Class-type registry initializer `FUN_141b09bbc`** maps every front-end UI resource path to a C++
  class descriptor at startup, e.g. `Interface/Menu/MenuLayer/FrontEndRoot -> CUIFrontEndRootLayer`
  (class global `DAT_142701df8`). Per class it calls `kTypeIdFromString` (`FUN_14115b52c`, path→id)
  then the **register primitive** `kRegisterUIClass` (`FUN_14045a0e0`, registry+id→classDesc) and
  stores the result in a `DAT_142701xxx` global. This function is enormous — Ghidra's decompiler
  returns "Response buffer size exceeded"; read it by disassembly around the string refs instead.
- **Menu items are instances**, not a code list. Each item ("FrontEnd_Start", "FrontEnd_WBPlay", …)
  is an instance of the menu-item class `DAT_1427013b8`, registered by name-id in that class's
  instance registry (`+0x38`) and found via `kMenuItemFindByName` (`FUN_141b08b08(name,ctx)`).
- **The visible layer's local item collection** (`CUIFrontEndRootLayer`): pointer at `layer+0x53f8`,
  count `+0x5400`, is **3 intrusive doubly-linked lists** (heads at container `+0x4b8/+0x490/+0x568`;
  node `+0x10` = item object, node `+0x8` = next). Look-up in it: `kFrontEndFindItem`
  (`FUN_14195ca2c(collection,name)`). Neither `+0x53f8` nor `+0x5400` is ever written directly — the
  container ptr is stored through the `+0x48` sub-object base (i.e. offset `+0x53b0`); the allocator
  is `kMenuContainerBuild` (`FUN_14071eda0`, `MOV [base+0x53b0],RAX`), shared by sibling menu-class
  ctors (`FUN_140e18010` @ class `DAT_142701e08`, `FUN_140e3c0f0` @ class `DAT_142701150`).
- Item instances are created from the `FrontEndRoot` menu definition (packed in the Oodle `.arch06`
  archives) by the engine's generic object-graph loader; consumed each frame by `kFrontEndItemRefresh`
  (`FUN_141977e3c`) and `FUN_14197c614`, and activation is dispatched by name in `kFrontEndSelectHandler`
  (`FUN_14197703c`).

**Implication for a native "HagUI" entry:** two viable routes, both more work than the overlay entry
we already ship: (a) edit the `FrontEndRoot` resource in the `.arch06` archive (needs Oodle archive
tooling); or (b) at runtime after the layer's container is built, construct a menu-item instance of
class `DAT_1427013b8` (name-id + localized label + selection wiring) and link it into one of the 3
lists — hookable at `FUN_141977e3c`/`FUN_14197c614`, but requires instantiating a real engine
menu-item object. Offsets recorded in `shared/GameOffsets.h`.
