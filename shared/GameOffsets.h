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

// --- Front-end / start menu (LithTech "Kraken" ClientShell) ---
// CUIFrontEndRootLayer::CUIFrontEndRootLayer — constructor of the front-end menu's ROOT UI layer.
// Runs when the start-menu UI is built/shown, so this is our "start menu displayed" init trigger.
// Called with this=RCX (up to 3 register args from the UI-layer factory) and returns `this`.
// Verified in Ghidra: writes the CUIFrontEndRootLayer vtables + references the class-name string;
// decompiles cleanly (not Denuvo-mutated).
inline constexpr std::uintptr_t kCUIFrontEndRootLayerCtor = 0x141976838ull;

// FrontEndLoadWorld — loads the front-end 3D BACKDROP world + background images. NOT the
// "menu shown" moment: hooking 0x141d6f0a8 installed fine but the game did NOT call it when the
// main menu displayed (the backdrop world was already loaded/cached). Kept for reference only.
inline constexpr std::uintptr_t kFrontEndLoadWorld = 0x141d6f0a8ull;

}  // namespace game
