#pragma once
// SoWModAPI.h - the contract between the SoWLoader mod loader and an EXTERNAL mod DLL.
//
// Drop submod DLLs under <game>/mods. The loader registers their V2 action
// schemas before calling the optional initializer on its worker thread.
// Menu structure, dynamic field data and IPC are declared in ModActions.h.
// The initializer receives an owner ID for config registration, not a UI API.
// Keep DllMain trivial.
//
// SCOPE (Skyrim-style global vs. save-local mods): a mod declares, via the optional SoWMod_Scope
// export, WHEN its tab is usable. Every mod DLL is still LoadLibrary'd once at startup and its
// SoWMod_Init runs once; scope only gates when the tab is shown in the HagUI hub:
//   SOWMOD_GLOBAL — usable in the main menu AND in-game (settings, loaders, always-on tools).
//   SOWMOD_LOCAL  — usable only once a SAVE is loaded in-game (world/character/inventory editors).
// If SoWMod_Scope is absent the mod is treated as GLOBAL.
//
// Load order: filename ascending. Prefix DLLs with 01_, 02_, ... to force an explicit order.
#include <cstdint>

extern "C" {

constexpr std::uint32_t SOWMOD_ABI_VERSION = 1;

// Mod scope values returned by SoWMod_Scope (see the SCOPE note above).
enum SoWModScope { SOWMOD_GLOBAL = 0, SOWMOD_LOCAL = 1 };

// Optional: the mod's display name; its single tab is auto-named after this. If absent, the loader
// names the tab after the DLL file (minus ".dll").
typedef const char* (*SoWMod_Name_t)();
typedef int (*SoWMod_Scope_t)();
// Optional worker-thread initialization after successful action registration.
typedef void (*SoWMod_Init_t)(int owner);
// Optional game-thread callback after the native campaign world is ready.
typedef void (*SoWMod_WorldLoaded_t)();

}  // extern "C"
