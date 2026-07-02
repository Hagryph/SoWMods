#pragma once
// SoWModAPI.h - the contract between the SoWLoader mod loader and an EXTERNAL mod DLL.
//
// A mod is a DLL dropped into  <game>\x64\mods\ . The loader (steam_api64.dll) enumerates that
// folder in filename order, LoadLibrary()s each DLL, and then — if the DLL exports SoWMod_Init —
// calls it exactly once. SoWMod_Init runs on the loader's worker thread, OUTSIDE the Windows
// loader lock (LoadLibrary has already returned), so it is safe to do real work there; keep the
// mod's DllMain trivial (DisableThreadLibraryCalls + return TRUE).
//
// Only the loader core + HagUI (the "Loader UI") are compiled into steam_api64.dll. Every other
// mod — the inventory editor and friends — is a separate DLL built from its own mods/<Name>/ folder.
//
// ONE TAB PER MOD: the loader creates exactly one HagUI tab for each mod, auto-named after the mod,
// and passes that tab's page handle to SoWMod_Init. A mod does NOT call RegisterPage — it just fills
// its given page. The tab name comes from the mod's optional SoWMod_Name export, else the DLL name.
//
//   extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "My Mod"; }   // optional
//   extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
//       auto get = (HagUI_GetAPI_t)GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI");
//       HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
//       if (ui) ui->AddToggle(page, "Enable", &gEnable);   // add all widgets to `page`
//   }
//
// Load order: filename ascending. Prefix DLLs with 01_, 02_, ... to force an explicit order.
#include <cstdint>

extern "C" {

constexpr std::uint32_t SOWMOD_ABI_VERSION = 1;

// Optional: the mod's display name; its single tab is auto-named after this. If absent, the loader
// names the tab after the DLL file (minus ".dll").
typedef const char* (*SoWMod_Name_t)();

// Called once after load (worker thread, outside the loader lock). `page` is THIS mod's single,
// already-created HagUI tab — add all your widgets to it. Optional (a pure-hook mod may omit it).
typedef void (*SoWMod_Init_t)(int page);

}  // extern "C"
