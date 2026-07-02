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

struct Row { std::string category, group, subgroup, tier, record, display; };

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
        // category, group, subgroup, tier, record, [display]. Old 5-col catalogs fall back to record.
        if (c.size() >= 5) rows.push_back({ c[0], c[1], c[2], c[3], c[4], c.size() >= 6 ? c[5] : c[4] });
    }
    return rows;
}
}  // namespace

// This mod's single tab is auto-named after this.
extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "Inventory Editor"; }

// The loader created this mod's ONE tab and hands us its page. We fill only that page.
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { Log("HagUI unavailable"); return; }

    const std::vector<Row> rows = LoadCatalog();
    if (rows.empty()) { ui->AddLabel(page, "Item catalog not loaded (InventoryEditor.catalog missing)."); return; }
    Log("loaded " + std::to_string(rows.size()) + " item records");

    // One searchable + filterable + scrollable list of every item template. The filter bucket is the
    // catalog's "group" (gear slot for GEAR: Sword/Dagger/Bow/Armor/Cloak/Talion; the category name
    // for RUNE/GEM/WEAPON/ARMOR/...). The row text is the record name; search narrows within a bucket.
    std::vector<std::string> items, cats;
    items.reserve(rows.size()); cats.reserve(rows.size());
    for (const auto& r : rows) {
        items.push_back(r.display);   // human-readable display name (see tools/extract_sow_items.py)
        cats.push_back(r.group);
    }
    std::vector<const char*> ip, cp;
    ip.reserve(items.size()); cp.reserve(cats.size());
    for (const auto& s : items) ip.push_back(s.c_str());
    for (const auto& s : cats)  cp.push_back(s.c_str());
    ui->AddList(page, ip.data(), cp.data(), (int)items.size());   // HagUI copies the strings
    Log("filled tab (page " + std::to_string(page) + ") with " + std::to_string(items.size()) + " items");
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; ::DisableThreadLibraryCalls(h); }
    return TRUE;
}
