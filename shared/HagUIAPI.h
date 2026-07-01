#pragma once
// HagUIAPI.h - stable, versioned, flat extern "C" cross-plugin ABI for SoW HagUI.
//
// HagUI (hosted in the SoWLoader proxy, steam_api64.dll) exports HagUI_GetAPI. Any other SoW mod
// registers its own option page WITHOUT linking HagUI: resolve the API at runtime with
//
//   auto get = (HagUI_GetAPI_t)GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI");
//   HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
//   if (ui) { int p = ui->RegisterPage("My Mod"); ui->AddToggle(p, "Enable X", &gEnableX); }
//
// No C++/STL types cross the boundary - only C primitives + function pointers.
#include <cstdint>

extern "C" {

constexpr std::uint32_t HAGUI_ABI_VERSION = 1;

struct HagUIAPI {
    std::uint32_t abiVersion;                              // == HAGUI_ABI_VERSION
    int  (*RegisterPage)(const char* title);               // -> page handle (>=0), or -1 on failure
    void (*AddLabel)(int page, const char* text);
    void (*AddToggle)(int page, const char* label, bool* value);   // HagUI reads/writes *value
    void (*AddButton)(int page, const char* label, void (*onClick)());
};

// Exported by the loader DLL. Returns null if the requested ABI version is unsupported.
typedef HagUIAPI* (*HagUI_GetAPI_t)(std::uint32_t abiVersion);

}  // extern "C"
