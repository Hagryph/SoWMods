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
// A mod that wants UI resolves HagUI at runtime (no link dependency) — see shared/HagUIAPI.h:
//
//   extern "C" __declspec(dllexport) void SoWMod_Init() {
//       auto get = (HagUI_GetAPI_t)GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI");
//       HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
//       if (ui) { int p = ui->RegisterPage("My Mod"); ui->AddToggle(p, "Enable", &gEnable); }
//   }
//
// Load order: filename ascending. Prefix DLLs with 01_, 02_, ... to force an explicit order.
#include <cstdint>

extern "C" {

constexpr std::uint32_t SOWMOD_ABI_VERSION = 1;

// The loader looks this export up by name and calls it once after loading the mod. Optional:
// a mod with no UI/registration work may omit it (it will simply be loaded).
typedef void (*SoWMod_Init_t)();

}  // extern "C"
