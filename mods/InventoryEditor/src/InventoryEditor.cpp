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
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <new>
#include <unordered_map>

#include "GameOffsets.h"
#include "HagUIAPI.h"    // ../../shared
#include "SoWModAPI.h"   // ../../shared

namespace {
HMODULE g_self = nullptr;
bool (*g_queueGameTask)(HagUI_GameTaskFn fn, void* ctx) = nullptr;

struct Row { std::string category, slot, set, rarity, tier, record, display; };

struct GrantTask {
    std::uintptr_t itemRow = 0;
    int count = 0;
    char requested[128]{};
    char resolved[128]{};
};

std::string Hex(std::uintptr_t v) {
    char b[32]{};
    ::wsprintfA(b, "0x%p", reinterpret_cast<void*>(v));
    return b;
}

void Log(const std::string& m) {
    const std::string debugLine = "[InventoryEditor] " + m + "\n";
    ::OutputDebugStringA(debugLine.c_str());

    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring root(base, n);
    root += L"\\SoWLoader";
    ::CreateDirectoryW(root.c_str(), nullptr);
    std::wstring dir = root + L"\\logs";
    ::CreateDirectoryW(dir.c_str(), nullptr);

    HANDLE h = ::CreateFileW((dir + L"\\InventoryEditor.log").c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    char stamp[32]{};
    const int slen = ::wsprintfA(stamp, "[%02d:%02d:%02d.%03d] ",
                                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    DWORD written = 0;
    ::WriteFile(h, stamp, static_cast<DWORD>(slen), &written, nullptr);
    ::WriteFile(h, m.c_str(), static_cast<DWORD>(m.size()), &written, nullptr);
    ::WriteFile(h, "\r\n", 2, &written, nullptr);
    ::CloseHandle(h);
}

constexpr std::uintptr_t kRowSize = 0x28;
constexpr std::size_t kLootRecordSize = 0x38;
constexpr std::size_t kDropPoseSize = 0x30;
constexpr int kMaxWorldDropsPerClick = 50;

template <class T>
bool ReadMem(std::uintptr_t addr, T& out) {
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool WriteMem(std::uintptr_t addr, const T& value) {
    __try {
        *reinterpret_cast<T*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t SafeStringLen(const char* s) {
    if (!s) return {};
    __try {
        return ::strnlen_s(s, 256);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string SafeString(const char* s) {
    const size_t n = SafeStringLen(s);
    return n ? std::string(s, n) : std::string{};
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep)) out.push_back(cur);
    return out;
}

void CopyAscii(char* dst, std::size_t cap, const std::string& src) {
    if (!dst || cap == 0) return;
    const std::size_t n = (std::min)(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

const char* SlotToken(const std::string& gearType) {
    if (gearType.find("Sword") != std::string::npos) return "1Sword";
    if (gearType.find("Dagger") != std::string::npos) return "2Dagger";
    if (gearType.find("Bow") != std::string::npos) return "3Bow";
    if (gearType.find("Armor") != std::string::npos) return "4Armor";
    if (gearType.find("Cape") != std::string::npos || gearType.find("Cloak") != std::string::npos) return "5Cape";
    return "";
}

bool IsLegendaryTheme(const std::string& v) {
    static const char* kThemes[] = {
        "Celebrimbor", "Vendetta", "Dark", "Feral", "Machine", "Marauder", "Mystic",
        "Outlaw", "Ringwraith", "Slaughter", "Terror", "Warmonger", "Ringcraft"
    };
    for (const char* t : kThemes) if (v == t) return true;
    return false;
}

std::vector<std::string> CandidateItemRecords(const std::string& record) {
    std::vector<std::string> out;
    out.push_back(record);
    if (!StartsWith(record, "GearArt_")) return out;

    auto parts = Split(record.substr(8), '_');
    if (parts.empty()) return out;
    const std::string type = parts[0];
    const std::string slot = SlotToken(type);
    if (slot.empty()) return out;

    std::string theme;
    std::string tierOrRarity;
    if (parts.size() >= 2) theme = parts[1];
    if (parts.size() >= 3) tierOrRarity = parts[2];

    auto addPrimary = [&](const std::string& suffix) {
        out.push_back("Primary_" + slot + "_" + suffix);
        if (type.find("BowSnipe") != std::string::npos) out.push_back("PrimSnipe_3Bow_" + suffix);
    };

    if (theme == "1Common" || theme == "2Uncommon" || theme == "3Rare" || theme == "4Epic") {
        addPrimary(theme);
    } else if (IsLegendaryTheme(theme)) {
        addPrimary("5Lgnd_" + theme);
        addPrimary("5Lgnd_" + theme + "_DebugGrant");
    } else if (tierOrRarity == "3Rare" || tierOrRarity == "4Epic") {
        addPrimary(tierOrRarity);
    }
    return out;
}

std::unordered_map<std::string, std::uintptr_t>& ItemIndex() {
    static std::unordered_map<std::string, std::uintptr_t> index;
    static bool built = false;
    if (built) return index;
    built = true;

    std::uintptr_t desc = 0;
    if (!ReadMem(game::FromRVA(game::kInventoryItemDescriptor), desc) || !desc) {
        Log("Inventory.Item descriptor unavailable");
        return index;
    }
    std::uint32_t count = 0;
    std::uintptr_t table = 0;
    if (!ReadMem(desc + 0x28, count) || !ReadMem(desc + 0x38, table) || !table) {
        Log("Inventory.Item descriptor has no table");
        return index;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uintptr_t row = table + i * kRowSize;
        const char* name = nullptr;
        if (!ReadMem(row + 0x20, name)) continue;
        std::string key = SafeString(name);
        if (!key.empty()) index.emplace(std::move(key), row);
    }
    Log("indexed " + std::to_string(index.size()) + " Inventory.Item rows");
    return index;
}

std::uintptr_t ResolveItemRow(const std::string& record, std::string& resolvedName) {
    auto& index = ItemIndex();
    for (const auto& candidate : CandidateItemRecords(record)) {
        auto it = index.find(candidate);
        if (it != index.end()) {
            resolvedName = candidate;
            return it->second;
        }
    }
    return 0;
}

template <class T>
void PutBytes(std::uint8_t* buf, std::size_t offset, const T& value) {
    std::memcpy(buf + offset, &value, sizeof(T));
}

bool Finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

std::uintptr_t CurrentPlayer() {
    std::uintptr_t engine = 0;
    if (!ReadMem(game::FromRVA(game::kEngineSingleton), engine) || !engine) {
        Log("engine singleton unavailable for world drop");
        return 0;
    }
    std::uintptr_t player = 0;
    if (!ReadMem(engine + 0x888, player) || !player) {
        Log("current player unavailable for world drop");
        return 0;
    }
    return player;
}

bool BuildDropPose(std::uint8_t* pose) {
    std::memset(pose, 0, kDropPoseSize);
    const std::uintptr_t player = CurrentPlayer();
    if (!player) return false;

    float x = 0.0f, y = 0.0f, z = 0.0f;
    if ((!ReadMem(player + 0x2180, x) || !ReadMem(player + 0x2184, y) ||
         !ReadMem(player + 0x2188, z) || !Finite3(x, y, z))) {
        Log("player transform unavailable for world drop");
        return false;
    }

    // Matches the fallback pose used by ActionInventoryItemDropLoot: position + identity rotation.
    PutBytes(pose, 0x00, x);
    PutBytes(pose, 0x04, y);
    PutBytes(pose, 0x08, z);
    const float zero = 0.0f;
    const float one = 1.0f;
    PutBytes(pose, 0x0c, zero);
    PutBytes(pose, 0x10, zero);
    PutBytes(pose, 0x14, zero);
    PutBytes(pose, 0x18, one);
    return true;
}

bool NextDropContext(std::uint32_t& id, std::uintptr_t& worldCtx) {
    std::uintptr_t systems = 0;
    if (!ReadMem(game::FromRVA(game::kGameSystemsSingleton), systems) || !systems) {
        Log("game systems singleton unavailable for world drop");
        return false;
    }
    std::uintptr_t manager = 0;
    if (!ReadMem(systems + 0x6d28, manager) || !manager) {
        Log("world drop manager unavailable");
        return false;
    }
    if (!ReadMem(manager + 0x20, worldCtx) || !worldCtx) {
        Log("world drop context unavailable");
        return false;
    }
    std::uint32_t cur = 0;
    if (!ReadMem(manager + 0x40, cur)) {
        Log("world drop id counter unavailable");
        return false;
    }
    id = cur;
    std::uint32_t next = cur + 1;
    if (next == 0) {
        next = 1;
        id = 1;
    }
    if (!WriteMem(manager + 0x40, next)) {
        Log("world drop id counter write failed");
        return false;
    }
    return true;
}

bool CallLootRecordInit(std::uint8_t* record) {
    using InitFn = void*(__fastcall*)(void*);
    __try {
        reinterpret_cast<InitFn>(game::FromRVA(game::kLootRecordInit))(record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallLootRecordDestroy(std::uint8_t* record) {
    using DestroyFn = void(__fastcall*)(void*);
    __try {
        reinterpret_cast<DestroyFn>(game::FromRVA(game::kLootRecordDestroy))(record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallWorldDropSpawn(std::uint8_t* pose, std::uint8_t* record, std::uint32_t id,
                        std::uintptr_t worldCtx, std::uintptr_t& result) {
    using SpawnFn = std::uintptr_t(__fastcall*)(void*, void*, std::uint32_t, std::uintptr_t, std::uint8_t);
    __try {
        result = reinterpret_cast<SpawnFn>(game::FromRVA(game::kWorldDropSpawn))(
            pose, record, id, worldCtx, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
        return false;
    }
}

bool SpawnWorldDrop(std::uintptr_t itemRow) {
    alignas(8) std::uint8_t record[kLootRecordSize]{};
    alignas(8) std::uint8_t pose[kDropPoseSize]{};
    bool initialized = false;
    bool ok = false;
    std::uintptr_t result = 0;

    if (!CallLootRecordInit(record)) {
        Log("loot record init raised SEH");
    } else {
        initialized = true;
        std::uintptr_t generatedData = 0;
        std::memcpy(&generatedData, record + 0x18, sizeof(generatedData));
        if (!generatedData) {
            Log("loot record init produced no generated-data slot");
        } else if (!BuildDropPose(pose)) {
            Log("world drop pose build failed");
        } else {
            std::uint32_t id = 0;
            std::uintptr_t worldCtx = 0;
            if (NextDropContext(id, worldCtx)) {
                const std::uint32_t itemKind = 1;
                const std::uint32_t resourceKind = 0x0c;
                PutBytes(record, 0x20, itemRow);
                PutBytes(record, 0x28, itemKind);
                PutBytes(record, 0x2c, resourceKind);

                if (CallWorldDropSpawn(pose, record, id, worldCtx, result)) {
                    ok = result != 0;
                    if (!ok) {
                        Log("world drop spawn returned null id=" + std::to_string(id) +
                            " ctx=" + Hex(worldCtx));
                    }
                } else {
                    Log("world drop spawn raised SEH id=" + std::to_string(id) +
                        " ctx=" + Hex(worldCtx));
                }
            }
        }
    }

    if (initialized && !CallLootRecordDestroy(record)) Log("loot record destroy raised SEH");
    if (!ok) Log("world drop spawn failed for row=" + Hex(itemRow));
    return ok;
}

void RunGrantTask(void* ctx) {
    GrantTask* task = static_cast<GrantTask*>(ctx);
    if (!task) return;

    int spawned = 0;
    for (int i = 0; i < task->count; ++i) {
        if (!SpawnWorldDrop(task->itemRow)) break;
        ++spawned;
    }
    Log(std::string("grant world-drop ") + task->requested + " -> " + task->resolved +
        " spawned=" + std::to_string(spawned) + "/" + std::to_string(task->count));
    delete task;
}

void GrantItem(const char* id, int count) {
    if (!id || count <= 0) return;
    if (count > kMaxWorldDropsPerClick) {
        Log("grant count capped from " + std::to_string(count) + " to " +
            std::to_string(kMaxWorldDropsPerClick) + " world drops");
        count = kMaxWorldDropsPerClick;
    }

    std::string resolved;
    const std::uintptr_t itemRow = ResolveItemRow(id, resolved);
    if (!itemRow) {
        Log(std::string("no Inventory.Item mapping for ") + id);
        return;
    }

    auto* task = new (std::nothrow) GrantTask{};
    if (!task) {
        Log("grant queue allocation failed");
        return;
    }
    task->itemRow = itemRow;
    task->count = count;
    CopyAscii(task->requested, sizeof(task->requested), id);
    CopyAscii(task->resolved, sizeof(task->resolved), resolved);

    if (!g_queueGameTask || !g_queueGameTask(&RunGrantTask, task)) {
        delete task;
        Log(std::string("grant queue failed ") + id + " -> " + resolved);
        return;
    }
    Log(std::string("grant queued world-drop ") + id + " -> " + resolved +
        " count=" + std::to_string(count));
}

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
    g_queueGameTask = ui->QueueGameTask;

    const std::vector<Row> rows = LoadCatalog();
    if (rows.empty()) { ui->AddLabel(page, "Item catalog not loaded (InventoryEditor.catalog missing)."); return; }
    Log("loaded " + std::to_string(rows.size()) + " item records");

    // One searchable, multi-facet-filtered, grouped list of every item template. Facets: Category
    // (also the group header), Slot (sub-header), Set, Rarity, Tier. Rows are the human-readable
    // display names. The catalog is already sorted by category then slot, so the grouping is tidy.
    static const char* kFacets[] = { "Category", "Slot", "Set", "Rarity", "Tier" };
    const int nf = (int)(sizeof(kFacets) / sizeof(kFacets[0]));

    std::vector<std::string> displays;                 // owns the row strings
    std::vector<std::string> ids;                      // owns stable row ids passed back on click
    std::vector<std::string> values;                   // owns the facet-value strings (row-major)
    displays.reserve(rows.size());
    ids.reserve(rows.size());
    values.reserve(rows.size() * nf);
    for (const auto& r : rows) {
        displays.push_back(r.display);
        ids.push_back(r.record);
        values.push_back(r.category);                  // facet 0
        values.push_back(r.slot);                      // facet 1
        values.push_back(r.set);                       // facet 2
        values.push_back(r.rarity);                    // facet 3
        values.push_back(r.tier);                      // facet 4
    }
    std::vector<const char*> dp, ip, vp;
    dp.reserve(displays.size()); ip.reserve(ids.size()); vp.reserve(values.size());
    for (const auto& s : displays) dp.push_back(s.c_str());
    for (const auto& s : ids)      ip.push_back(s.c_str());
    for (const auto& s : values)   vp.push_back(s.c_str());
    ui->AddFacetedActionList(page, kFacets, nf, dp.data(), ip.data(), (int)displays.size(),
                             vp.data(), &GrantItem);  // HagUI copies
    Log("filled tab (page " + std::to_string(page) + ") with " + std::to_string(displays.size()) + " items");
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; ::DisableThreadLibraryCalls(h); }
    return TRUE;
}
