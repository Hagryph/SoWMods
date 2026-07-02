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

constexpr std::uint32_t HAGUI_ABI_VERSION = 2;

struct HagUIAPI {
    std::uint32_t abiVersion;                              // == HAGUI_ABI_VERSION
    int  (*RegisterPage)(const char* title);               // -> page handle (>=0), or -1 on failure
    void (*AddLabel)(int page, const char* text);
    void (*AddToggle)(int page, const char* label, bool* value);   // HagUI reads/writes *value
    void (*AddButton)(int page, const char* label, void (*onClick)());
    // v2: a searchable + filterable scrolling list that fills the page's content area. items[i] is the
    // row text; cats[i] is its filter bucket (nullptr/"" = no bucket). HagUI COPIES all strings, then
    // renders a search box, a filter dropdown (the distinct buckets, first-seen order, "All" first),
    // and a scrollbar-backed list. The caller's arrays need not outlive the call.
    void (*AddList)(int page, const char* const* items, const char* const* cats, int count);
};

// Exported by the loader DLL. Returns null if the requested ABI version is unsupported.
typedef HagUIAPI* (*HagUI_GetAPI_t)(std::uint32_t abiVersion);

}  // extern "C"
