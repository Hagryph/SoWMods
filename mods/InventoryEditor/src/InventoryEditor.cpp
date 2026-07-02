// InventoryEditor - reads the offline-extracted SoW item catalog (tools/extract_sow_items.py ->
// InventoryEditor.catalog, deployed next to this DLL) and surfaces it, grouped + sorted, in HagUI.
// The catalog is the full set of item templates from game.gamedb (750 gear pieces + runes/gems/
// weapons/armor) - see memory reference_sow-data-archives. This is the data pipeline into the editor;
// per-item "add" wiring comes once HagUI has scrolling + the runtime grant call.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "HagUIAPI.h"    // ../../shared
#include "SoWModAPI.h"   // ../../shared

namespace {
HMODULE g_self = nullptr;

struct Row { std::string category, group, subgroup, tier, record; };

// Keep label strings alive for HagUI (it stores const char*).
std::vector<std::string> g_strings;
const char* keep(const std::string& s) { g_strings.push_back(s); return g_strings.back().c_str(); }

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
        if (c.size() >= 5) rows.push_back({ c[0], c[1], c[2], c[3], c[4] });
    }
    return rows;
}
}  // namespace

extern "C" __declspec(dllexport) void SoWMod_Init() {
    auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { Log("HagUI unavailable"); return; }

    const std::vector<Row> rows = LoadCatalog();
    if (rows.empty()) {
        int p = ui->RegisterPage("Inventory");
        ui->AddLabel(p, "Item catalog not loaded (InventoryEditor.catalog missing).");
        return;
    }
    Log("loaded " + std::to_string(rows.size()) + " item records");

    // group: category -> group -> count, and category -> set-of-subgroups, category -> total
    std::map<std::string, std::map<std::string, int>> byGroup;   // cat -> group -> n
    std::map<std::string, std::map<std::string, int>> setsOf;    // cat -> subgroup -> n
    std::map<std::string, int> total;
    // preserve first-seen category order
    std::vector<std::string> catOrder;
    for (const auto& r : rows) {
        if (!byGroup.count(r.category)) catOrder.push_back(r.category);
        byGroup[r.category][r.group]++;
        if (r.subgroup != "-") setsOf[r.category][r.subgroup]++;
        total[r.category]++;
    }

    // one HagUI page per category, showing the grouped breakdown (fits the fixed card)
    for (const auto& cat : catOrder) {
        std::string title = cat;                                   // GEAR -> Gear, RUNE -> Rune, ...
        std::transform(title.begin() + 1, title.end(), title.begin() + 1, ::tolower);
        int page = ui->RegisterPage(keep(title));
        std::string hdr = std::to_string(total[cat]) + " records";
        if (!setsOf[cat].empty()) hdr += "  \xE2\x80\xA2  " + std::to_string(setsOf[cat].size()) + " sets/themes";
        ui->AddLabel(page, keep(hdr));
        for (const auto& [grp, n] : byGroup[cat])
            ui->AddLabel(page, keep("  " + grp + "  \xE2\x80\x94  " + std::to_string(n)));
    }
    Log("registered " + std::to_string(catOrder.size()) + " catalog pages");
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; ::DisableThreadLibraryCalls(h); }
    return TRUE;
}
