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

constexpr std::uint32_t HAGUI_ABI_VERSION = 3;

struct HagUIAPI {
    std::uint32_t abiVersion;                              // == HAGUI_ABI_VERSION
    int  (*RegisterPage)(const char* title);               // -> page handle (>=0), or -1 on failure
    void (*AddLabel)(int page, const char* text);
    void (*AddToggle)(int page, const char* label, bool* value);   // HagUI reads/writes *value
    void (*AddButton)(int page, const char* label, void (*onClick)());
    // v2: a searchable + filterable scrolling list. items[i] is the row text; cats[i] its filter
    // bucket (nullptr/"" = none). Implemented as a one-facet AddFacetedList. Kept for simple mods.
    void (*AddList)(int page, const char* const* items, const char* const* cats, int count);
    // v3: a searchable, MULTI-FACET-filtered, grouped list. facetNames[0..facetCount) name the filter
    // facets; facet 0 is the group header, facet 1 the sub-group header. displays[0..itemCount) are the
    // row labels. facetValues is ROW-MAJOR itemCount x facetCount (item i, facet f => [i*facetCount+f]);
    // a ""/"-" value means the item has no value for that facet (it isn't offered as an option and is
    // excluded only when that facet has an active selection). Filtering is OR within a facet, AND across
    // facets, plus a case-insensitive substring search over the display text. HagUI COPIES everything;
    // the caller's arrays need not outlive the call. Feed items pre-sorted by facet 0 then 1 for tidy
    // group headers.
    void (*AddFacetedList)(int page, const char* const* facetNames, int facetCount,
                           const char* const* displays, int itemCount,
                           const char* const* facetValues);
};

// Exported by the loader DLL. Returns null if the requested ABI version is unsupported.
typedef HagUIAPI* (*HagUI_GetAPI_t)(std::uint32_t abiVersion);

}  // extern "C"
