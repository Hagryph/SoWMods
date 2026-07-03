#pragma once
// shared/GameOffsets.h - single source of truth for hand-found RVAs in Middle-earth: Shadow of War.
//
// Every offset here is a FILE RVA relative to the PE image base (0x140000000), exactly as read in
// Ghidra from the Steamless-unpacked ShadowOfWar.exe. Convert to a live runtime address at load time
// with FromRVA(): GetModuleHandle(NULL) + (fileRVA - kImageBase).
//
// This is the ONLY file that owns raw addresses. Each mod's src/Offsets.h includes THIS header and
// re-exports the k-names it needs. Additions must be VERIFIED against the disassembly - no guesses,
// no Address-Library-style lookup (there is none for SoW anyway); trace every offset from a primitive.
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace game {

// PE image base of ShadowOfWar.exe (x64, PE32+). Verified from the optional header.
inline constexpr std::uintptr_t kImageBase = 0x140000000ull;

// Live module base of the running ShadowOfWar.exe (the main image).
inline std::uintptr_t Base() {
    static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    return base;
}

// File RVA (as seen in Ghidra, off kImageBase) -> live runtime address.
inline std::uintptr_t FromRVA(std::uintptr_t fileRVA) {
    return Base() + (fileRVA - kImageBase);
}

// ---------------------------------------------------------------------------
// Hand-found RVAs, grouped by subsystem. From the Ghidra analysis of
// C:\dev\re\sow\ShadowOfWar.exe.unpacked.exe (image base 0x140000000).
// ---------------------------------------------------------------------------

// --- Platform / main window (RE'd 2026-07-02 from the "Shadow of War" class-string refs) ---
// FUN_140c24db0 = the engine's MAIN-THREAD platform function (names its thread "MainThread"):
// registers the L"Shadow of War" window class, creates THE game window via the engine's own
// resolved-API table (DAT_141d92c18 = CreateWindowExW; style 0xcf0000, initially 336x239 — the
// import is called through data slots, which is why the IAT entry has zero code refs), stores the
// HWND into the globals below, sets icons/timer/hotkeys, ShowWindow(hwnd, 1), then enters the
// message/frame loop. It never returns until game exit — so the window-created moment is NOT its
// return, it is the first engine call AFTER the create+show sequence:
inline constexpr std::uintptr_t kWinMainThread   = 0x140c24db0; // main-thread func (creates the window)
inline constexpr std::uintptr_t kPostWindowInit  = 0x1411798ac; // first call AFTER create+ShowWindow; gates the main loop -> our console trigger
inline constexpr std::uintptr_t kMainWindowHwnd  = 0x142702640; // global HWND of the game main window
inline constexpr std::uintptr_t kMainWindowHwnd2 = 0x142c88000; // second copy the engine keeps

// NOTE: DAT_1427030c8 is a cursor-clamp-disable flag (every cursor clamp/hide/recenter routine gates on
// `== 0`), BUT setting it to 1 in gameplay had NO observable effect — the "pause + free cursor" seen
// while probing it was actually the game pausing on WINDOW FOCUS LOSS (our tooling stole focus), not
// this flag. Left here only as a warning: it is NOT the pause lever.

// PAUSE — what the ESC hotkey calls (RE'd 2026-07-03, live-verified: toggled the pause menu closed).
// The ESC handler does `FUN_1406cdf0c(*(DAT_1426ffa98 + 0xe38))` — a ONE-arg toggle of the "PauseMenu"
// screen, which runs the game's real pause (sim freeze via the time-scale system + cursor). Reproduce it
// as: uiCtx = *(*(kEngineSingleton) + kUiCtxOff); ((void(__fastcall*)(void*))kPauseToggle)(uiCtx). MUST be
// called on the game's message/main thread (our WndProc qualifies) — it mutates UI state.
inline constexpr std::uintptr_t kPauseToggle    = 0x1406cdf0c; // FUN_1406cdf0c(uiCtx): toggle pause menu
inline constexpr std::uintptr_t kEngineSingleton = 0x1426ffa98; // DAT_1426ffa98: ptr-to the engine singleton
inline constexpr std::uintptr_t kUiCtxOff       = 0xe38;       // engine + 0xe38 -> the pause UI context

// --- Front-end / start menu (LithTech "Kraken" ClientShell) ---
// CUIFrontEndRootLayer::CUIFrontEndRootLayer — constructor of the front-end menu's ROOT UI layer.
// Runs when the start-menu UI is built/shown, so this is our "start menu displayed" init trigger.
// Called with this=RCX (up to 3 register args from the UI-layer factory) and returns `this`.
// Verified in Ghidra: writes the CUIFrontEndRootLayer vtables + references the class-name string;
// decompiles cleanly (not Denuvo-mutated).
inline constexpr std::uintptr_t kCUIFrontEndRootLayerCtor = 0x141976838ull;

// Front-end root layer LIFECYCLE = the menu-vs-in-save signal (RE'd 2026-07-03, trace from the player
// vtable 0x141f995e0 down to the world-load command system, then the front-end vtable). The layer is a
// heap object (size 0x61c8); it is virtual-`delete`d when the menu is torn down for gameplay. We latch:
//   ctor fires  -> Menu ;  vtable slot 0 (scalar deleting dtor) fires -> InGame (save loaded).
// Verified: slot 0 == FUN_141976a24 { FUN_14197694c(); if (flags&1) free(this,0x61c8); }. The actual
// delete is an indirect vtable call (not statically traceable) — GameHooks swaps slot 0 at runtime.
inline constexpr std::uintptr_t kFrontEndRootLayerVtable   = 0x141f90d08ull; // *(this) after ctor
inline constexpr std::uintptr_t kFrontEndRootLayerDtor     = 0x14197694cull; // real (member) destructor
inline constexpr std::uintptr_t kFrontEndRootLayerDelDtor  = 0x141976a24ull; // vtable slot 0: deleting dtor
inline constexpr std::uintptr_t kPlayerBaseVtable          = 0x141f995e0ull; // shared Character base (Talion + orcs) — NOT a unique in-save anchor

// FrontEndLoadWorld — loads the front-end 3D BACKDROP world + background images. NOT the
// "menu shown" moment: hooking 0x141d6f0a8 installed fine but the game did NOT call it when the
// main menu displayed (the backdrop world was already loaded/cached). Kept for reference only.
inline constexpr std::uintptr_t kFrontEndLoadWorld = 0x141d6f0a8ull;

// --- Menu-entry registration internals (deep RE 2026-07-02; docs/reverse-engineering.md) ---
// The START menu is DATA-DRIVEN: there is no hardcoded "add these 5 entries" list. Two layers:
//  1) Class-type registry INITIALIZER: at startup maps every UI resource path -> C++ class desc,
//     e.g. "Interface/Menu/MenuLayer/FrontEndRoot" -> CUIFrontEndRootLayer (class global 0x142701df8).
//     Huge function (won't decompile whole). Calls the two primitives below once per class.
inline constexpr std::uintptr_t kUIClassRegistryInit   = 0x141b09bbc; // registers all front-end UI classes
inline constexpr std::uintptr_t kTypeIdFromString      = 0x14115b52c; // (out, path) -> type-id
inline constexpr std::uintptr_t kRegisterUIClass       = 0x14045a0e0; // (registry, type-id) -> classDesc
//  2) Menu ITEMS are instances of the menu-item class (class global 0x1427013b8); registered by
//     name-id in that class's instance registry (+0x38) and looked up by name:
inline constexpr std::uintptr_t kMenuItemFindByName    = 0x141b08b08; // (name, ctx) -> item instance
// The visible layer's local item collection = 3 intrusive linked lists; ptr at layer+0x53f8, count
// +0x5400. Allocated by the menu ctor helper below (writes the container ptr via the +0x48
// sub-object base, i.e. +0x53b0). Consumed/refreshed + activated by the layer methods below.
inline constexpr std::uintptr_t kFrontEndFindItem      = 0x14195ca2c; // (collection, name) -> item node
inline constexpr std::uintptr_t kMenuContainerBuild    = 0x14071eda0; // stores the item-container ptr
inline constexpr std::uintptr_t kFrontEndItemRefresh   = 0x141977e3c; // reads the item list each update
inline constexpr std::uintptr_t kFrontEndSelectHandler = 0x14197703c; // dispatches item activation by name

// Front-end menu ITEM object (live-verified 2026-07-02). Standard item: class desc 0x141f8c488,
// vtable 0x141f8c498, size 0x260 bytes. Ctor sets vtable + class + base members only (the loader
// wires ~20 sub-objects per item afterward). Layout: +0x00 vtable, +0x18 classDesc, +0x20 sequential
// index, +0x28 name object (interned "FrontEnd_*"), +0x40 highlight-style name object,
// +0x168 EMBEDDED list node (node+0x8 next, node+0x10 -> item); items live in list L1
// (container+0x490). The DISPLAY label is NOT stored on the item — it is localized from the +0x28
// name key at render time (so a native entry cannot just set a label string).
inline constexpr std::uintptr_t kMenuItemClass  = 0x141f8c488;
inline constexpr std::uintptr_t kMenuItemVtable = 0x141f8c498;
inline constexpr std::uintptr_t kMenuItemCtor   = 0x14193d824; // ctor(item,ctx1,ctx2,classNode); 0x260 bytes

// Generic UI object FACTORY: FUN_141940900(ctx1, ctx2, classNode) allocates (via kGameAlloc) + builds
// the object whose class descriptor *(classNode+0x18) matches a big dispatch table; for a menu item
// (class 0x1427013b8) it allocs 0x260 and calls kMenuItemCtor. This is how the resource loader makes
// each item from its resource-definition node — so a native item needs a (cloned) resource node fed
// here, not just a name. Item name/display-label/action come from that node.
inline constexpr std::uintptr_t kUIObjectFactory = 0x141940900;
inline constexpr std::uintptr_t kGameAlloc       = 0x1403de240; // (size) -> heap block (game allocator)

// --- Localization + Scaleform text bridge (the LABEL unlock, RE'd 2026-07-02) ---
// The front-end IS Scaleform (GFxValue). Each menu button's text is set at build via:
//   loc = kLocalize(asciiKey);  gfxTextField[vtbl+0x330](loc);   // GetMember(+0x298) walks the path
// kLocalize(const char* key) -> wchar_t* : looks the key up in the global StringDatabase
// (DAT_142702000; returns L"[ERROR: StringDatabase Not Initialized]" if null). HOOK THIS to control
// any native label — return your own wchar_t* for a chosen key. VERIFIED LIVE: overriding keys
// containing "Benchmark" made the native "RUN BENCHMARK" button read "HagUI".
inline constexpr std::uintptr_t kLocalize      = 0x140471f2c; // (const char* key) -> wchar_t* localized
inline constexpr std::uintptr_t kTextBridge    = 0x14195fcbc; // sample loc(key)->GFx SetText path
inline constexpr unsigned       kGFxGetMember  = 0x298;       // GFxValue vtable slot: GetMember(name)
inline constexpr unsigned       kGFxSetText    = 0x330;       // GFxValue vtable slot: SetText(wstr)
// Live front-end button loc keys: START=UI_Button_Start, WBPlay=UI_WBID_Menu, OPTIONS=UI_Menu_Options,
// QUIT=UI_QuitToDesktop, "RUN BENCHMARK"=<key containing Benchmark>.

}  // namespace game
