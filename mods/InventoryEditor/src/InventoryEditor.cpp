// Item spawner - reads the live Inventory.Item descriptor and surfaces spawnable item rows in HagUI.
// Clicking an item spawns a world pickup, which the player collects through the game's own loot path.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <tuple>

#include "GameOffsets.h"
#include "HagUIAPI.h"    // ../../shared
#include "SoWModAPI.h"   // ../../shared

namespace {
HagUIAPI* g_ui = nullptr;
bool (*g_queueGameTask)(HagUI_GameTaskFn fn, void* ctx) = nullptr;

struct Row { std::string category, slot, set, rarity, variant, record, display; };

struct SpawnTask {
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

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool Contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

std::unordered_map<std::string, std::uintptr_t>& ItemIndex() {
    static std::unordered_map<std::string, std::uintptr_t> index;
    static bool built = false;
    if (built) return index;

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
    built = true;
    Log("indexed " + std::to_string(index.size()) + " Inventory.Item rows");
    return index;
}

std::string StripDebugGrant(std::string name) {
    static const char* kSuffix = "_DebugGrant";
    if (EndsWith(name, kSuffix)) name.resize(name.size() - std::strlen(kSuffix));
    return name;
}

bool IsRarityToken(const std::string& token) {
    static const char* kTokens[] = {
        "1Default", "1Common", "1Base",
        "2Standard", "2Uncommon", "2Forged", "2Crafted",
        "3Rare", "3Crafted", "3Forged",
        "4Epic", "4Mighty",
        "5Lgnd", "5Legendary", "5Wondrous", "5Marvel",
        "6Epic", "7Lgnd"
    };
    for (const char* t : kTokens) if (token == t) return true;
    return false;
}

std::string PrettyToken(const std::string& token) {
    if (token.empty()) return {};
    if (token == "1Sword") return "Sword";
    if (token == "2Dagger") return "Dagger";
    if (token == "3Bow") return "Bow";
    if (token == "4Armor") return "Armor";
    if (token == "5Cape") return "Cloak";
    if (token == "6Ring") return "Ring";
    if (token == "1Default" || token == "1Common") return "Default";
    if (token == "1Base") return "Base";
    if (token == "2Standard" || token == "2Uncommon") return "Standard";
    if (token == "2Crafted" || token == "3Crafted") return "Crafted";
    if (token == "2Forged" || token == "3Forged") return "Forged";
    if (token == "3Rare") return "Rare";
    if (token == "4Epic" || token == "6Epic") return "Epic";
    if (token == "4Mighty") return "Mighty";
    if (token == "5Lgnd" || token == "5Legendary" || token == "7Lgnd") return "Legendary";
    if (token == "5Wondrous") return "Wondrous";
    if (token == "5Marvel") return "Marvel";
    if (token == "Dmg") return "Damage";
    if (token == "SotP") return "SotP";

    std::string out;
    out.reserve(token.size() + 4);
    for (size_t i = 0; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        const unsigned char prev = i ? static_cast<unsigned char>(token[i - 1]) : 0;
        if (i > 0 && std::isupper(c) && (std::islower(prev) || std::isdigit(prev))) out.push_back(' ');
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string PrettyJoin(const std::vector<std::string>& parts, size_t first, size_t last) {
    std::string out;
    if (last > parts.size()) last = parts.size();
    for (size_t i = first; i < last; ++i) {
        if (parts[i].empty()) continue;
        if (!out.empty()) out.push_back(' ');
        out += PrettyToken(parts[i]);
    }
    return out;
}

std::string RarityLabel(const std::string& name) {
    const std::vector<std::string> parts = Split(StripDebugGrant(name), '_');
    for (const auto& part : parts) {
        if (IsRarityToken(part)) return PrettyToken(part);
    }
    return "-";
}

bool IsGearRecord(const std::string& name) {
    return StartsWith(name, "Primary_") || StartsWith(name, "PrimHam_") ||
           StartsWith(name, "PrimSnipe_");
}

std::string GearSlotLabel(const std::string& name) {
    if (Contains(name, "_1Sword_")) return "Sword";
    if (Contains(name, "_2Dagger_")) return "Dagger";
    if (Contains(name, "_3Bow_")) return "Bow";
    if (Contains(name, "_4Armor_")) return "Armor";
    if (Contains(name, "_5Cape_")) return "Cloak";
    if (Contains(name, "_6Ring_")) return "Ring";
    return "-";
}

std::string GearVariantLabel(const std::string& name) {
    if (StartsWith(name, "PrimHam_")) return "Hammer";
    if (StartsWith(name, "PrimSnipe_")) return "Snipe";
    return "-";
}

std::string GearSetLabel(const std::string& name) {
    const std::vector<std::string> parts = Split(StripDebugGrant(name), '_');
    for (size_t i = 0; i < parts.size(); ++i) {
        if (IsRarityToken(parts[i]) && i + 1 < parts.size()) {
            return PrettyJoin(parts, i + 1, parts.size());
        }
    }
    return "-";
}

std::string DisplayLabel(const std::string& name) {
    std::string clean = StripDebugGrant(name);
    if (StartsWith(clean, "Primary_")) clean.erase(0, std::strlen("Primary_"));
    else if (StartsWith(clean, "PrimHam_")) clean.replace(0, std::strlen("PrimHam_"), "Hammer_");
    else if (StartsWith(clean, "PrimSnipe_")) clean.replace(0, std::strlen("PrimSnipe_"), "Snipe_");
    const std::vector<std::string> parts = Split(clean, '_');
    return PrettyJoin(parts, 0, parts.size());
}

bool RuntimeRowForRecord(const std::string& name, const std::unordered_set<std::string>& names, Row& out) {
    if (name.empty() || Contains(name, "_Resell_Value")) return false;

    if (IsGearRecord(name)) {
        if (!EndsWith(name, "_DebugGrant") && names.find(name + "_DebugGrant") != names.end()) return false;
        out = { "Gear", GearSlotLabel(name), GearSetLabel(name), RarityLabel(name),
                GearVariantLabel(name), name, DisplayLabel(name) };
        return out.slot != "-";
    }

    const std::vector<std::string> parts = Split(name, '_');
    if (StartsWith(name, "Runegem_") && parts.size() >= 4 &&
        (parts[1] == "Green" || parts[1] == "Red" || parts[1] == "White")) {
        out = { "Gem", PrettyToken(parts[1]), PrettyToken(parts[2]), RarityLabel(name),
                "Runegem", name, DisplayLabel(name) };
        return true;
    }
    if (StartsWith(name, "Gem_") && parts.size() >= 3 &&
        (parts[1] == "Green" || parts[1] == "Red" || parts[1] == "White") &&
        IsRarityToken(parts[2])) {
        out = { "Gem", PrettyToken(parts[1]), "-", PrettyToken(parts[2]),
                "Gem", name, DisplayLabel(name) };
        return true;
    }
    if (StartsWith(name, "Rune_") && parts.size() >= 2 &&
        (IsRarityToken(parts[1]) || parts[1] == "Green" || parts[1] == "Red" || parts[1] == "White")) {
        out = { "Rune", parts.size() > 2 ? PrettyToken(parts[1]) : "-", "-",
                RarityLabel(name), "Rune", name, DisplayLabel(name) };
        return true;
    }
    if (StartsWith(name, "Weapon_Tint_") && parts.size() >= 3) {
        out = { "Skin", "Weapon", PrettyJoin(parts, 2, parts.size()), "-",
                "Tint", name, DisplayLabel(name) };
        return true;
    }
    return false;
}

std::vector<Row> LoadRuntimeRows() {
    auto& index = ItemIndex();
    std::vector<Row> rows;
    if (index.empty()) return rows;

    std::unordered_set<std::string> names;
    names.reserve(index.size());
    for (const auto& kv : index) names.insert(kv.first);

    rows.reserve(index.size());
    for (const auto& kv : index) {
        Row row;
        if (RuntimeRowForRecord(kv.first, names, row)) rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return std::tie(a.category, a.slot, a.rarity, a.set, a.variant, a.display, a.record) <
               std::tie(b.category, b.slot, b.rarity, b.set, b.variant, b.display, b.record);
    });
    return rows;
}

std::uintptr_t ResolveItemRow(const std::string& record, std::string& resolvedName) {
    auto& index = ItemIndex();
    auto it = index.find(record);
    if (it != index.end()) {
        resolvedName = record;
        return it->second;
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

bool CallLootRecordFromItem(std::uint8_t* record, std::uintptr_t itemRow) {
    using FromItemFn = void*(__fastcall*)(void*, std::uintptr_t);
    __try {
        reinterpret_cast<FromItemFn>(game::FromRVA(game::kLootRecordFromItem))(record, itemRow);
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

    if (!CallLootRecordFromItem(record, itemRow)) {
        Log("loot record from item raised SEH row=" + Hex(itemRow));
    } else {
        initialized = true;
        std::uintptr_t generatedData = 0;
        std::memcpy(&generatedData, record + 0x18, sizeof(generatedData));
        if (!generatedData) {
            Log("loot record from item produced no generated-data slot");
        } else if (!BuildDropPose(pose)) {
            Log("world drop pose build failed");
        } else {
            std::uint32_t id = 0;
            std::uintptr_t worldCtx = 0;
            if (NextDropContext(id, worldCtx)) {
                std::uintptr_t resolvedRow = 0;
                std::uint32_t itemKind = 0;
                std::uint32_t resourceKind = 0;
                std::memcpy(&resolvedRow, record + 0x20, sizeof(resolvedRow));
                std::memcpy(&itemKind, record + 0x28, sizeof(itemKind));
                std::memcpy(&resourceKind, record + 0x2c, sizeof(resourceKind));
                if (!resolvedRow) {
                    Log("loot record from item produced no row for row=" + Hex(itemRow));
                } else {
                    Log("loot record from item row=" + Hex(itemRow) + " -> row=" +
                        Hex(resolvedRow) + " kind=" + std::to_string(itemKind) +
                        " resource=" + std::to_string(resourceKind) +
                        " generated=" + Hex(generatedData));

                    if (CallWorldDropSpawn(pose, record, id, worldCtx, result)) {
                        ok = result != 0;
                        if (!ok) {
                            Log("world drop spawn returned null id=" + std::to_string(id) +
                                " ctx=" + Hex(worldCtx) + " row=" + Hex(resolvedRow) +
                                " kind=" + std::to_string(itemKind) +
                                " resource=" + std::to_string(resourceKind));
                        }
                    } else {
                        Log("world drop spawn raised SEH id=" + std::to_string(id) +
                            " ctx=" + Hex(worldCtx) + " row=" + Hex(resolvedRow) +
                            " kind=" + std::to_string(itemKind) +
                            " resource=" + std::to_string(resourceKind));
                    }
                }
            }
        }
    }

    if (initialized && !CallLootRecordDestroy(record)) Log("loot record destroy raised SEH");
    if (!ok) Log("world drop spawn failed for row=" + Hex(itemRow));
    return ok;
}

void RunSpawnTask(void* ctx) {
    SpawnTask* task = static_cast<SpawnTask*>(ctx);
    if (!task) return;

    int spawned = 0;
    for (int i = 0; i < task->count; ++i) {
        if (!SpawnWorldDrop(task->itemRow)) break;
        ++spawned;
    }
    Log(std::string("spawn world-drop ") + task->requested + " -> " + task->resolved +
        " spawned=" + std::to_string(spawned) + "/" + std::to_string(task->count));
    delete task;
}

void SpawnItem(const char* id, int count) {
    if (!id || count <= 0) return;
    if (count > kMaxWorldDropsPerClick) {
        Log("spawn count capped from " + std::to_string(count) + " to " +
            std::to_string(kMaxWorldDropsPerClick) + " world drops");
        count = kMaxWorldDropsPerClick;
    }

    std::string resolved;
    const std::uintptr_t itemRow = ResolveItemRow(id, resolved);
    if (!itemRow) {
        Log(std::string("no Inventory.Item mapping for ") + id);
        return;
    }

    auto* task = new (std::nothrow) SpawnTask{};
    if (!task) {
        Log("spawn queue allocation failed");
        return;
    }
    task->itemRow = itemRow;
    task->count = count;
    CopyAscii(task->requested, sizeof(task->requested), id);
    CopyAscii(task->resolved, sizeof(task->resolved), resolved);

    if (!g_queueGameTask || !g_queueGameTask(&RunSpawnTask, task)) {
        delete task;
        Log(std::string("spawn queue failed ") + id + " -> " + resolved);
        return;
    }
    Log(std::string("spawn queued world-drop ") + id + " -> " + resolved +
        " count=" + std::to_string(count));
}

void BuildPageOnFirstOpen(int page) {
    if (!g_ui || page < 0) return;
    const std::vector<Row> rows = LoadRuntimeRows();
    if (rows.empty()) {
        g_ui->AddLabel(page, "Live Inventory.Item rows are not available.");
        Log("live Inventory.Item list unavailable or empty");
        return;
    }

    static const char* kFacets[] = { "Category", "Slot", "Set", "Rarity", "Variant" };
    const int nf = (int)(sizeof(kFacets) / sizeof(kFacets[0]));

    std::vector<std::string> displays;
    std::vector<std::string> ids;
    std::vector<std::string> values;
    displays.reserve(rows.size());
    ids.reserve(rows.size());
    values.reserve(rows.size() * nf);
    for (const auto& r : rows) {
        displays.push_back(r.display);
        ids.push_back(r.record);
        values.push_back(r.category);
        values.push_back(r.slot);
        values.push_back(r.set);
        values.push_back(r.rarity);
        values.push_back(r.variant);
    }
    std::vector<const char*> dp, ip, vp;
    dp.reserve(displays.size());
    ip.reserve(ids.size());
    vp.reserve(values.size());
    for (const auto& s : displays) dp.push_back(s.c_str());
    for (const auto& s : ids) ip.push_back(s.c_str());
    for (const auto& s : values) vp.push_back(s.c_str());
    g_ui->AddFacetedActionList(page, kFacets, nf, dp.data(), ip.data(),
                               (int)displays.size(), vp.data(), &SpawnItem);
    Log("filled tab (page " + std::to_string(page) + ") with " +
        std::to_string(displays.size()) + " live Inventory.Item rows");
}
}  // namespace

// This mod's single tab is auto-named after this.
extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "Item Spawner"; }

// Save-local: an item spawner only makes sense with a loaded save, so its tab appears in the hub
// only in-game (hidden at the main menu). See shared/SoWModAPI.h.
extern "C" __declspec(dllexport) int SoWMod_Scope() { return SOWMOD_LOCAL; }

// The loader created this mod's ONE tab and hands us its page. We fill only that page.
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { Log("HagUI unavailable"); return; }
    g_ui = ui;
    g_queueGameTask = ui->QueueGameTask;
    ui->SetPageOnFirstOpenInSave(page, &BuildPageOnFirstOpen);
    Log("registered live Inventory.Item first-open builder for page " + std::to_string(page));
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { ::DisableThreadLibraryCalls(h); }
    return TRUE;
}
