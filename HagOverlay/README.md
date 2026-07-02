# HagOverlay

A CSS-styled, full-modal HUD overlay for Middle-earth: Shadow of War — rendered by **WebView2
(Chromium)** in a layered top-level window the OS compositor paints **over** the game. It uses its
own renderer; the game's D3D/Scaleform is never touched (no Present hook, no engine dependency).

We went this route after finding SoW's Scaleform is a closed loop: it only advances/renders movie
hosts owned by a UI layer, so an injected additive movie loads fine but never gets ticked/drawn.
An external layered overlay sidesteps that entirely.

## How it works
- **Own renderer:** WebView2 loads `web/index.html` (full HTML/CSS/JS) — the black-and-gold HagUI hub.
- **Attached to the game:** the window is made an *owned* window of the game HWND (so it stays above
  it and minimizes with it) and is repositioned every ~120 ms to exactly cover the game's client rect.
  Works because SoW is borderless-windowed (a layered window can't cover exclusive-fullscreen).
- **Full modal / blocks click-through:** the CSS backdrop is opaque and covers the whole client area;
  when shown, the window takes foreground so it captures all mouse+keyboard (the game gets none).
- **Toggle:** global hotkey **F8** (open/close). `CLOSE` button or `ESC` also close (JS → host via
  `window.chrome.webview.postMessage('close')`), which hides the window and refocuses the game.

## Build & run
```powershell
.\build.ps1 -Run     # needs .NET 9 SDK + the WebView2 runtime (both preinstalled here)
```
Launch the game, then press **F8**. Edit `web/index.html` to restyle the hub (no rebuild needed for
HTML/CSS — just reopen).

## TODO
- Have SoWLoader auto-launch HagOverlay.exe (instead of running it by hand).
- Disable the old in-engine D3D immediate-mode HagUI hub in SoWLoader (it also toggles on F8).
- Tighten input handoff so no stray key reaches the game on toggle.
- Wire real settings pages (tabs) + a JS↔mod bridge (e.g. over HagIPC or a local socket).
