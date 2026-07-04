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
#include <limits>
#include <unordered_map>

#include "GameOffsets.h"
#include "HagUIAPI.h"    // ../../shared
#include "SoWModAPI.h"   // ../../shared

namespace {
HMODULE g_self = nullptr;

struct Row { std::string category, slot, set, rarity, tier, record, display; };

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
constexpr std::uintptr_t kInvVecBegin = 0x740;
constexpr std::uintptr_t kInvVecEnd = 0x748;
constexpr std::uintptr_t kInvDirty = 0x76c;

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

bool CallSetEntryCount(std::uintptr_t entry, std::uint32_t count) {
    using SetCountFn = void(__fastcall*)(std::uintptr_t, std::uint32_t);
    __try {
        reinterpret_cast<SetCountFn>(game::FromRVA(game::kInventorySetEntryCount))(entry, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallAddItem(std::uintptr_t inv, std::uintptr_t itemRow, std::uint32_t count) {
    using AddFn = bool(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uint32_t);
    __try {
        return reinterpret_cast<AddFn>(game::FromRVA(game::kInventoryAddItem))(inv, itemRow, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsReadableRange(std::uintptr_t addr, std::size_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!addr || !::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = start + mbi.RegionSize;
    return addr >= start && len <= end - addr;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, sep)) out.push_back(cur);
    return out;
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

std::string ItemRecordName(std::uintptr_t itemRow) {
    const char* name = nullptr;
    if (!ReadMem(itemRow + 0x20, name)) return {};
    return SafeString(name);
}

struct InventoryProbe {
    bool valid = false;
    std::uintptr_t addr = 0;
    std::uintptr_t owner = 0;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uint32_t count = 0;
    int score = (std::numeric_limits<int>::min)();
};

int PlayerInventoryScore(std::uintptr_t begin, std::uint32_t count) {
    int score = static_cast<int>(std::min<std::uint32_t>(count, 200));
    const std::uint32_t limit = std::min<std::uint32_t>(count, 160);
    for (std::uint32_t i = 0; i < limit; ++i) {
        std::uintptr_t entry = 0, itemRow = 0;
        if (!ReadMem(begin + static_cast<std::uintptr_t>(i) * sizeof(std::uintptr_t), entry) ||
            !entry || !ReadMem(entry, itemRow) || !itemRow) {
            continue;
        }
        const std::string name = ItemRecordName(itemRow);
        if (name.empty()) continue;

        // Talion's inventory contains player abilities/unlocks mixed with gear. Orc/NPC inventories
        // start with personality/tribe records and otherwise look structurally identical.
        if (name == "PC_Glaive") score += 1000;
        else if (name == "ScriptedUnlock_WraithVision") score += 1000;
        else if (name == "Primary_1Sword_1Default") score += 500;
        else if (name == "S_BasicWraith") score += 250;
        else if (StartsWith(name, "PC_")) score += 80;
        else if (StartsWith(name, "Personality_") || StartsWith(name, "Tribe_") ||
                 StartsWith(name, "Orc_")) {
            score -= 500;
        }
    }
    return score;
}

InventoryProbe ProbeInventoryContainer(std::uintptr_t candidate, bool scoreItems) {
    InventoryProbe p{};
    p.addr = candidate;
    std::uintptr_t vtable = 0, owner = 0, ownerVtable = 0, begin = 0, end = 0;
    if (!ReadMem(candidate, vtable) || vtable != game::FromRVA(game::kInventoryContainerVtable)) return p;
    if (!ReadMem(candidate + 0x440, owner) || !owner) return p;
    if (!ReadMem(owner, ownerVtable) || ownerVtable != game::FromRVA(game::kPlayerBaseVtable)) return p;
    if (!ReadMem(candidate + kInvVecBegin, begin) || !ReadMem(candidate + kInvVecEnd, end)) return p;
    if (end < begin || ((end - begin) & 7) != 0) return p;
    const std::uintptr_t count = (end - begin) / 8;
    if (count == 0 || count >= 5000 || !IsReadableRange(begin, static_cast<std::size_t>(end - begin))) return p;
    p.valid = true;
    p.owner = owner;
    p.begin = begin;
    p.end = end;
    p.count = static_cast<std::uint32_t>(count);
    p.score = scoreItems ? PlayerInventoryScore(begin, p.count) : 0;
    return p;
}

bool ValidateInventoryContainer(std::uintptr_t candidate) {
    const InventoryProbe p = ProbeInventoryContainer(candidate, true);
    return p.valid && p.score > 0;
}

std::uintptr_t FindInventoryContainer() {
    static std::uintptr_t cached = 0;
    if (cached && ValidateInventoryContainer(cached)) return cached;

    const std::uintptr_t target = game::FromRVA(game::kInventoryContainerVtable);
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t max = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    InventoryProbe best{};
    InventoryProbe fallback{};
    fallback.score = (std::numeric_limits<int>::min)();
    while (p < max) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!::VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) break;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t end = base + mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                              !(mbi.Protect & PAGE_NOACCESS);
        if (readable) {
            for (std::uintptr_t q = base; q + sizeof(std::uintptr_t) <= end; q += sizeof(std::uintptr_t)) {
                std::uintptr_t value = 0;
                if (ReadMem(q, value) && value == target) {
                    InventoryProbe probe = ProbeInventoryContainer(q, true);
                    if (!probe.valid) continue;
                    if (!fallback.valid || probe.count > fallback.count) fallback = probe;
                    if (!best.valid || probe.score > best.score ||
                        (probe.score == best.score && probe.count > best.count)) {
                        best = probe;
                    }
                }
            }
        }
        p = end;
    }
    const InventoryProbe chosen = (best.valid && best.score > 0) ? best : fallback;
    if (chosen.valid) {
        cached = chosen.addr;
        Log("inventory container selected addr=" + Hex(chosen.addr) +
            " owner=" + Hex(chosen.owner) +
            " count=" + std::to_string(chosen.count) +
            " score=" + std::to_string(chosen.score));
        return cached;
    }
    Log("inventory container not found");
    return 0;
}

std::uintptr_t FindEntry(std::uintptr_t container, std::uintptr_t itemRow, std::uint32_t* count = nullptr) {
    std::uintptr_t begin = 0, end = 0;
    if (!ReadMem(container + kInvVecBegin, begin) || !ReadMem(container + kInvVecEnd, end) || end < begin)
        return 0;
    for (std::uintptr_t p = begin; p < end; p += sizeof(std::uintptr_t)) {
        std::uintptr_t entry = 0, entryItem = 0;
        if (!ReadMem(p, entry) || !entry || !ReadMem(entry, entryItem)) continue;
        if (entryItem == itemRow) {
            if (count) ReadMem(entry + 0x20, *count);
            return entry;
        }
    }
    return 0;
}

void GrantItem(const char* id, int count) {
    if (!id || count <= 0) return;
    if (count > 9999) count = 9999;

    std::string resolved;
    const std::uintptr_t itemRow = ResolveItemRow(id, resolved);
    if (!itemRow) {
        Log(std::string("no Inventory.Item mapping for ") + id);
        return;
    }
    Log(std::string("grant requested ") + id + " -> " + resolved +
        " count=" + std::to_string(count));
    Log("grant blocked: direct inventory insertion is unsafe for gear until level/stat instance generation is wired");
    return;

    const std::uintptr_t inv = FindInventoryContainer();
    if (!inv) return;

    std::uint32_t before = 0;
    const std::uintptr_t existing = FindEntry(inv, itemRow, &before);
    if (existing) {
        const std::uint32_t afterCount = before + static_cast<std::uint32_t>(count);
        if (!CallSetEntryCount(existing, afterCount)) {
            WriteMem(existing + 0x20, afterCount);
        }
        const std::uint8_t dirty = 1;
        WriteMem(inv + kInvDirty, dirty);
    } else {
        if (!CallAddItem(inv, itemRow, static_cast<std::uint32_t>(count))) {
            Log("add wrapper raised SEH after call; verifying memory anyway");
        }
    }

    std::uint32_t after = 0;
    const std::uintptr_t entry = FindEntry(inv, itemRow, &after);
    Log(std::string("grant ") + id + " -> " + resolved +
        (entry ? (" count " + std::to_string(before) + " -> " + std::to_string(after)) : " failed"));
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
