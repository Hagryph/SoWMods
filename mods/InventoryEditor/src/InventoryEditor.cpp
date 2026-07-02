// InventoryEditor - reads the offline-extracted SoW item catalog (tools/extract_sow_items.py ->
// InventoryEditor.catalog, deployed next to this DLL) and surfaces it, grouped + sorted, in HagUI.
// The catalog is the full set of item templates from game.gamedb (750 gear pieces + runes/gems/
// weapons/armor) - see memory reference_sow-data-archives. This is the data pipeline into the editor;
// per-item "add" wiring comes once HagUI has scrolling + the runtime grant call.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "HagUIAPI.h"    // ../../shared
#include "SoWModAPI.h"   // ../../shared

namespace {
HMODULE g_self = nullptr;

struct Row { std::string category, slot, set, rarity, tier, record, display; };

void Log(const std::string& m) { ::OutputDebugStringA(("[InventoryEditor] " + m + "\n").c_str()); }

std::wstring CatalogPath() {
    wchar_t p[MAX_PATH]{};
    ::GetModuleFileNameW(g_self, p, MAX_PATH);           // ...\mods\InventoryEditor.dll
    std::wstring s = p;
    const size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos) s.resize(dot);
    return s + L".catalog";                              // ...\mods\InventoryEditor.catalog
}

std::vector<Row> LoadCatalog() {
    std::vector<Row> rows;
    std::ifstream f(CatalogPath());
    if (!f) { Log("catalog file not found next to the DLL"); return rows; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c; std::string tok; std::istringstream is(line);
        while (std::getline(is, tok, '\t')) c.push_back(tok);
        // category, slot, set, rarity, tier, record, display  (7 cols)
        if (c.size() >= 7) rows.push_back({ c[0], c[1], c[2], c[3], c[4], c[5], c[6] });
    }
    return rows;
}
}  // namespace

// This mod's single tab is auto-named after this.
extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "Inventory Editor"; }

// Save-local: an inventory editor only makes sense with a loaded save, so its tab appears in the hub
// only in-game (hidden at the main menu). See shared/SoWModAPI.h.
extern "C" __declspec(dllexport) int SoWMod_Scope() { return SOWMOD_LOCAL; }

// The loader created this mod's ONE tab and hands us its page. We fill only that page.
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { Log("HagUI unavailable"); return; }

    const std::vector<Row> rows = LoadCatalog();
    if (rows.empty()) { ui->AddLabel(page, "Item catalog not loaded (InventoryEditor.catalog missing)."); return; }
    Log("loaded " + std::to_string(rows.size()) + " item records");

    // One searchable, multi-facet-filtered, grouped list of every item template. Facets: Category
    // (also the group header), Slot (sub-header), Set, Rarity, Tier. Rows are the human-readable
    // display names. The catalog is already sorted by category then slot, so the grouping is tidy.
    static const char* kFacets[] = { "Category", "Slot", "Set", "Rarity", "Tier" };
    const int nf = (int)(sizeof(kFacets) / sizeof(kFacets[0]));

    std::vector<std::string> displays;                 // owns the row strings
    std::vector<std::string> values;                   // owns the facet-value strings (row-major)
    displays.reserve(rows.size());
    values.reserve(rows.size() * nf);
    for (const auto& r : rows) {
        displays.push_back(r.display);
        values.push_back(r.category);                  // facet 0
        values.push_back(r.slot);                      // facet 1
        values.push_back(r.set);                       // facet 2
        values.push_back(r.rarity);                    // facet 3
        values.push_back(r.tier);                      // facet 4
    }
    std::vector<const char*> dp, vp;
    dp.reserve(displays.size()); vp.reserve(values.size());
    for (const auto& s : displays) dp.push_back(s.c_str());
    for (const auto& s : values)   vp.push_back(s.c_str());
    ui->AddFacetedList(page, kFacets, nf, dp.data(), (int)displays.size(), vp.data());  // HagUI copies
    Log("filled tab (page " + std::to_string(page) + ") with " + std::to_string(displays.size()) + " items");
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; ::DisableThreadLibraryCalls(h); }
    return TRUE;
}
