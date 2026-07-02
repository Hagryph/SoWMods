# HagIPC (Shadow of War)

A standalone **localhost debug/IPC server** for Middle-earth: Shadow of War, ported from the SkyrimMods
HagIPC. It exposes live memory read/write, function calls, and raw code execution over a TCP text
protocol so we can **drive the running game by RVA** — verifying reverse-engineered call sequences
before baking them into C++, without paying the (Denuvo-slow) game boot on every iteration.

> **Dev tool. Attached-debugger level of access.** Loopback-only (`127.0.0.1`). Do not enable on an
> untrusted machine and do not ship it enabled to other users.

## Hot-load (no relaunch)

HagIPC is **not** a SoWLoader proxy — it's injected into the already-running process:

```powershell
.\build.ps1                 # build HagIPC.dll
.\tools\inject.ps1          # LoadLibrary-inject it into the live ShadowOfWar.exe
```

`inject.ps1` uses `VirtualAllocEx` + `CreateRemoteThread(LoadLibraryW)`. Re-running is safe (LoadLibrary
just bumps the refcount). On attach, a worker thread reads the config and starts the server.

## Client

```powershell
.\tools\hagipc.ps1 "ping"
.\tools\hagipc.ps1 -Commands "base","read 0x141976838 u32"
.\tools\hagipc.ps1                       # interactive REPL
```

## Protocol

Line-based text, one client at a time. Offsets are **file addresses off `0x140000000`** (e.g.
`0x141976838`, exactly as seen in Ghidra) or `abs:<VA>` for a raw runtime address.

| command | meaning |
|---|---|
| `ping` | `ok pong` |
| `base` | live module base of ShadowOfWar.exe |
| `read <off> <type> [chain..]` | read a typed value; `type` = `u8/16/32/64`, `i8..i64`, `f32/f64`, `ptr` |
| `readb <off> <len> [chain..]` | read up to 4096 raw bytes (hex) |
| `write <off> <type> <val> [chain..]` | write a typed value |
| `call <off> [a0..a7]` | call a function (MS x64 ABI, up to 8 int/ptr args) → RAX |
| `exec <hexblob>` | run a position-independent code blob (entry@0, ends with `ret`) → RAX |

`chain`: each extra offset dereferences then adds — `p = *p + c` — for pointer-chain reads.

All commands run **inline on the socket thread** under SEH guards (a bad address returns an error
instead of crashing the game) — safe for probing load-path/query functions from the front-end menu.
Functions that must run on the game's main/render thread will need frame marshaling (a later addition
once we hook a SoW per-frame point; the Skyrim build did this via the SKSE task queue).

## Config

`%LOCALAPPDATA%\SoWLoader\config\HagIPC.ini` (auto-created): `enabled`, `port` (default 19000), `token`
(optional shared secret; if set, the client's first line must be `auth <token>`). Logs to
`%LOCALAPPDATA%\SoWLoader\logs\HagIPC.log`.
