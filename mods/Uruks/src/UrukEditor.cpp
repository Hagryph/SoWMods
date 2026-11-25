#include "UrukEditor.h"
#include "GameOffsets.h"
#include "Settings.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <set>

namespace uruk {
namespace {
using Ptr = std::uintptr_t;
template<class T> T Read(Ptr address) {
    __try { return address ? *reinterpret_cast<const T*>(address) : T{}; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return T{}; }
}
bool CopyText(Ptr address, char* out, size_t size) {
    __try {
        if (!address) return false;
        for (size_t i = 0; i + 1 < size; ++i) {
            char c = reinterpret_cast<const char*>(address)[i];
            out[i] = c;
            if (!c) return true;
        }
        out[size - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
std::string Text(Ptr address) { char text[512]{}; CopyText(address, text, sizeof(text)); return text; }
std::string RecordName(Ptr record) { return record ? Text(Read<Ptr>(record + 0x20)) : "None"; }
struct Record { Ptr address; std::string name; };
using Records = std::vector<Record>;
using Catalog = std::map<std::string, Records>;
struct Key {
    Ptr army = 0;
    uint32_t index = 0;
    uint64_t seed = 0;
    bool operator==(const Key&) const = default;
};
struct Target { Key key; Ptr member; std::string label; };
enum class Kind { View, Target, Level, Personality, Role, Tribe, Picker, AddPicker, Design, Equipment };
struct Choice { uint64_t value; std::string label; bool selected = false; };
struct Field {
    uint64_t id = 0;
    Kind kind = Kind::View;
    std::string label, value, category;
    Ptr expected = 0;
    uint32_t slot = 0;
    std::vector<Choice> choices;
};
struct Snapshot {
    uint64_t session = 0;
    Key selected;
    std::vector<Target> targets;
    std::vector<Field> fields;
};
HagUIAPI* g_ui = nullptr;
std::mutex g_mutex;
Snapshot g_snapshot;
std::atomic<bool> g_pending{false};
std::atomic<uint64_t> g_nextField{1};

int VectorSize(Ptr vector, size_t stride, int limit = 4096) {
    Ptr begin = Read<Ptr>(vector), end = Read<Ptr>(vector + 8);
    if (end < begin || (end - begin) % stride || (end - begin) / stride > static_cast<size_t>(limit)) return 0;
    return static_cast<int>((end - begin) / stride);
}
Catalog LoadCatalog(Ptr database = 0) {
    Catalog catalog;
    if (!database) database = Read<Ptr>(game::FromRVA(game::kGameDatabase));
    const uint32_t count = Read<uint32_t>(database + 0x48);
    const Ptr categories = Read<Ptr>(database + 0x50);
    if (!database || !categories || count > 10000) return catalog;
    for (uint32_t i = 0; i < count; ++i) {
        const Ptr category = categories + i * 0x58;
        const auto name = Text(Read<Ptr>(category));
        if (!(name.starts_with("Faction/") || name.starts_with("Model/SimpleModel") ||
            name == "Combat/Tree" || name == "Character/Models" || name == "Inventory/Items" ||
            name == "Inventory/ArmorData" || name == "Inventory/EquippedWeaponData")) continue;
        const uint32_t rows = Read<uint32_t>(category + 0x28);
        const Ptr table = Read<Ptr>(category + 0x38);
        if (rows > 100000 || !table) continue;
        auto& records = catalog[name];
        for (uint32_t j = 0; j < rows; ++j) {
            const Ptr record = table + j * 0x28;
            if (Read<Ptr>(record + 0x18) != category) continue;
            auto recordName = RecordName(record);
            if (!recordName.empty()) records.push_back({record, std::move(recordName)});
        }
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    }
    return catalog;
}
bool InCatalog(const Catalog& catalog, const std::string& category, Ptr record) {
    auto it = catalog.find(category);
    return it != catalog.end() && std::any_of(it->second.begin(), it->second.end(),
        [record](const auto& entry) { return entry.address == record; });
}
uint32_t FieldHash(const char* text) {
    uint32_t hash = 0;
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        uint32_t value = c >= 'a' && c <= 'z' ? c - 'a' + 1 :
            c >= '0' && c <= '9' ? c - '0' + 27 : c == '_' ? 38 : 0;
        hash = hash * 919 + value;
    }
    return hash;
}
uint64_t Cell(Ptr record, const char* name, uint32_t index = 0, uint8_t expectedType = 10, uint64_t fallback = 0) {
    if (!record || (Read<uint8_t>(record + 1) & 0x10)) return fallback;
    const auto hash = FieldHash(name);
    const auto count = Read<uint8_t>(record);
    const Ptr fields = Read<Ptr>(record + 8);
    const Ptr cells = Read<Ptr>(record + 0x10);
    if (!fields || !cells) return fallback;
    for (uint32_t i = 0; i < count; ++i) {
        const Ptr field = fields + i * 16;
        if (Read<uint32_t>(field) != hash) continue;
        if ((Read<uint8_t>(field + 0xa) & 0x3f) != expectedType || index >= Read<uint16_t>(field + 0xc)) return fallback;
        return Read<uint64_t>(cells + (Read<uint16_t>(field + 0xe) + index) * 8);
    }
    return fallback;
}
Ptr PieceForDesign(Ptr design, uint32_t body) {
    for (uint32_t i = 0; i < 1024; ++i) {
        Ptr entry = Cell(design, "ModelPicker", i);
        if (!entry) break;
        uint32_t required = static_cast<uint32_t>(Cell(entry, "RequiredBodyType", 0, 3, UINT32_MAX));
        if (required == 0 || required == body) return Cell(entry, "SimpleModelPiece");
    }
    return 0;
}
bool ItemFits(Ptr item, uint32_t slot) {
    Ptr builder = Cell(item, "ItemBuilder");
    if (!builder) return false;
    const char* list = slot < 10 ? "WeaponData" : "ArmorData";
    const char* field = slot < 10 ? "ItemSlot" : "ArmorSlot";
    const uint32_t target = slot < 10 ? slot : slot - 10;
    bool found = false;
    for (uint32_t i = 0; i < 1024; ++i) {
        Ptr data = Cell(builder, list, i);
        if (!data) break;
        if (Cell(data, field, 0, 3, UINT32_MAX) != target) return false;
        found = true;
    }
    return found;
}
bool PickerAllowed(Ptr member, Ptr candidate, Ptr replacing) {
    if (!candidate || (Read<uint64_t>(member + 0x508) & (1ull << 38))) return false;
    const int count = VectorSize(member + 0x1e0, 0x20, 1024);
    const Ptr begin = Read<Ptr>(member + 0x1e0);
    for (int i = 0; i < count; ++i) {
        const Ptr held = Read<Ptr>(begin + i * 0x20);
        if (held == replacing) continue;
        if (held == candidate) return false;
        for (uint32_t a = 0; a < 1024; ++a) {
            const Ptr group = Cell(candidate, "ExclusiveGroup", a);
            if (!group) break;
            for (uint32_t b = 0; b < 1024; ++b) {
                Ptr other = Cell(held, "ExclusiveGroup", b);
                if (!other) break;
                if (group == other) return false;
            }
        }
    }
    return true;
}
std::vector<Target> Targets(Ptr faction = 0) {
    std::vector<Target> result;
    const Ptr systems = Read<Ptr>(game::FromRVA(game::kGameSystemsSingleton));
    if (!faction) faction = Read<Ptr>(systems + 0x6c68);
    const Ptr pools = Read<Ptr>(faction + 0x68);
    const uint32_t count = Read<uint32_t>(faction + 0x70);
    if (!faction || !pools || count > 32) return result;
    for (uint32_t i = 0; i < count; ++i) {
        const Ptr pool = pools + i * 0xc0 + 8;
        const int members = VectorSize(pool + 0x18, 8, 4096);
        const Ptr list = Read<Ptr>(pool + 0x18);
        for (int j = 0; j < members; ++j) {
            const Ptr member = Read<Ptr>(list + j * 8);
            if (!member || Read<Ptr>(member) != game::FromRVA(game::kFactionMemberVtable) ||
                Read<uint32_t>(member + 0x174) != static_cast<uint32_t>(j)) continue;
            if (!Read<Ptr>(member + 0x268) && !Read<Ptr>(member + 0x190)) continue;
            Key key{Read<Ptr>(member + 0x178), static_cast<uint32_t>(j), Read<uint64_t>(member + 0x168)};
            if (!key.army) continue;
            char name[33]{};
            CopyText(member + 0x498, name, sizeof(name));
            std::string label = name[0] ? name : "Unnamed";
            label += " | " + RecordName(key.army) + " | " + std::to_string(j);
            if (Read<uint64_t>(member + 0x508) & 4) label += " | Dead";
            result.push_back({key, member, std::move(label)});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
    return result;
}

Field& Add(Snapshot& snapshot, std::string label, std::string value, Kind kind = Kind::View) {
    snapshot.fields.push_back({});
    auto& field = snapshot.fields.back();
    field.id = g_nextField.fetch_add(1);
    field.kind = kind;
    field.label = std::move(label);
    field.value = std::move(value);
    return field;
}
void RecordField(Snapshot& snapshot, const Catalog& catalog, const char* label, Ptr current,
                 const char* category, Kind kind = Kind::View) {
    auto& field = Add(snapshot, label, RecordName(current), kind);
    field.category = category;
    field.expected = current;
    if (kind == Kind::View) return;
    if (auto it = catalog.find(category); it != catalog.end()) for (const auto& record : it->second)
        field.choices.push_back({record.address, record.name, record.address == current});
}

void Capture(Snapshot& snapshot, const Catalog& catalog) {
    auto& selector = Add(snapshot, "Uruk", "Select a captain", Kind::Target);
    for (size_t i = 0; i < snapshot.targets.size(); ++i) selector.choices.push_back({i, snapshot.targets[i].label, snapshot.targets[i].key == snapshot.selected});
    auto selected = std::find_if(snapshot.targets.begin(), snapshot.targets.end(), [&](const auto& target) { return target.key == snapshot.selected; });
    if (selected == snapshot.targets.end()) {
        Add(snapshot, "State", "No captain selected");
        return;
    }
    selector.value = selected->label;
    const Ptr member = selected->member;
    const uint64_t flags = Read<uint64_t>(member + 0x508);
    const bool spawned = Read<Ptr>(member + 0x48) != 0;
    std::map<Ptr, std::vector<Ptr>> groups;
    std::set<Ptr> heldPickers;
    const int heldCount = VectorSize(member + 0x1e0, 0x20, 1024);
    for (int i = 0; i < heldCount; ++i) heldPickers.insert(Read<Ptr>(Read<Ptr>(member + 0x1e0) + i * 0x20));
    if (auto it = catalog.find("Faction/Traits/Picker"); it != catalog.end()) for (const auto& record : it->second) {
        for (uint32_t i = 0; i < 1024; ++i) {
            Ptr group = Cell(record.address, "ExclusiveGroup", i);
            if (!group) break;
            groups[record.address].push_back(group);
        }
    }
    auto allowed = [&](Ptr picker, Ptr replacing) {
        if ((flags & (1ull << 38)) || (picker != replacing && heldPickers.count(picker))) return false;
        for (Ptr held : heldPickers) if (held != replacing)
            for (Ptr group : groups[picker]) if (std::find(groups[held].begin(), groups[held].end(), group) != groups[held].end()) return false;
        return true;
    };
    std::map<uint32_t, Records> equipment;
    if (!spawned) if (auto it = catalog.find("Inventory/Items"); it != catalog.end()) for (const auto& record : it->second) {
        Ptr builder = Cell(record.address, "ItemBuilder");
        if (!builder) continue;
        Ptr weapon = Cell(builder, "WeaponData"), armor = Cell(builder, "ArmorData");
        uint32_t slot = weapon ? static_cast<uint32_t>(Cell(weapon, "ItemSlot", 0, 3, 25)) :
            armor ? static_cast<uint32_t>(Cell(armor, "ArmorSlot", 0, 3, 25)) + 10 : 25;
        if (slot < 25 && ItemFits(record.address, slot)) equipment[slot].push_back(record);
    }
    Add(snapshot, "State", (flags & 4) ? "Dead" : "Alive");
    auto& level = Add(snapshot, "Level", std::to_string(Read<uint32_t>(member + 0x4b8) + 1), Kind::Level);
    for (uint32_t i = 0; i < 85; ++i) level.choices.push_back({i, std::to_string(i + 1), i == Read<uint32_t>(member + 0x4b8)});
    RecordField(snapshot, catalog, "Personality / voice", Read<Ptr>(member + 0x270), "Faction/Personalities", Kind::Personality);
    RecordField(snapshot, catalog, "Role / title", Read<Ptr>(member + 0x318), "Faction/Roles/Definitions", Kind::Role);
    RecordField(snapshot, catalog, "Tribe", Read<Ptr>(member + 0x328), "Faction/Tribes/Definitions", Kind::Tribe);
    RecordField(snapshot, catalog, "Class model", Read<Ptr>(member + 0x278), "Character/Models");
    RecordField(snapshot, catalog, "Combat tree", Read<Ptr>(member + 0x188), "Combat/Tree");
    RecordField(snapshot, catalog, "Army", Read<Ptr>(member + 0x178), "Faction/Definition");
    RecordField(snapshot, catalog, "Army row", Read<Ptr>(member + 0x268), "Faction/Rows");
    RecordField(snapshot, catalog, "Location", Read<Ptr>(member + 0x330), "Faction/Locations");
    RecordField(snapshot, catalog, "Rarity", Read<Ptr>(member + 0x390), "Faction/OrcRarity");
    RecordField(snapshot, catalog, "Preset", Read<Ptr>(member + 0x190), "Faction/PresetMembers");
    RecordField(snapshot, catalog, "Trait level remap", Read<Ptr>(member + 0x280), "Faction/Traits/PickerLevelRemap");
    RecordField(snapshot, catalog, "Purpose", Read<Ptr>(member + 0x290), "Faction/Purpose");
    RecordField(snapshot, catalog, "Last death type", Read<Ptr>(member + 0x2c0), "Faction/Resurrection");
    RecordField(snapshot, catalog, "Applied scar", Read<Ptr>(member + 0x370), "Combat/Scar/Type");
    Add(snapshot, "Live character", Read<Ptr>(member + 0x48) ? "Spawned" : "Not spawned");
    Add(snapshot, "Follower", (flags & (1ull << 27)) ? "Yes" : "No");
    Add(snapshot, "Intel level", std::to_string(Read<uint8_t>(member + 0x51a)));
    Add(snapshot, "Locked XP level", std::to_string(Read<uint32_t>(member + 0x4bc)));
    Add(snapshot, "Player kills", std::to_string(Read<uint32_t>(member + 0x4c4)));
    Add(snapshot, "Player flee count", std::to_string(Read<uint32_t>(member + 0x4c8)));
    Add(snapshot, "Resurrection count", std::to_string(Read<uint32_t>(member + 0x500)));
    Add(snapshot, "Random seed", std::to_string(snapshot.selected.seed));
    char flagText[32]; std::snprintf(flagText, sizeof(flagText), "0x%016llX", flags);
    Add(snapshot, "Nemesis flags", flagText);

    static const char* weaponSlots[] = {"Melee weapon", "Secondary weapon", "Ranged weapon", "Ranged auxiliary", "Weapon slot 4", "Weapon slot 5", "Utility weapon", "Tribe weapon", "Weapon slot 8", "Offhand weapon"};
    for (uint32_t i = 0; i < 25; ++i) {
        const Ptr instance = Read<Ptr>(member + 0x50 + i * 8);
        const Ptr item = Read<Ptr>(instance + 0x38);
        auto& field = Add(snapshot, i < 10 ? weaponSlots[i] : "Armor item " + std::to_string(i - 10), RecordName(item), Kind::Equipment);
        field.expected = item;
        field.slot = i;
        field.category = "Inventory/Items";
        for (const auto& record : equipment[i]) field.choices.push_back({record.address, record.name, record.address == item});
        if (instance) {
            Add(snapshot, i < 10 ? std::string(weaponSlots[i]) + " look" : "Armor item look " + std::to_string(i - 10), RecordName(Read<Ptr>(instance + 0x40)));
        }
    }

    const int slots = VectorSize(member + 0x1e0, 0x20, 1024);
    const Ptr start = Read<Ptr>(member + 0x1e0);
    for (int i = 0; i < slots; ++i) {
        const Ptr slot = start + i * 0x20;
        const Ptr picker = Read<Ptr>(slot);
        const Ptr resolved = Read<Ptr>(slot + 8);
        const std::string name = RecordName(picker);
        auto& field = Add(snapshot, "Trait / perk " + std::to_string(i + 1), name + " -> " + RecordName(resolved), Kind::Picker);
        field.expected = picker;
        field.slot = i;
        field.category = "Faction/Traits/Picker";
        if (!(flags & (1ull << 38))) field.choices.push_back({0, "Remove picker", false});
        if (auto it = catalog.find(field.category); it != catalog.end()) for (const auto& record : it->second)
            if (allowed(record.address, picker)) field.choices.push_back({record.address, record.name, record.address == picker});
        if (name.find("Mount") != std::string::npos || name.find("Caragor") != std::string::npos)
            Add(snapshot, "Mount ability", RecordName(resolved));
        if (name.find("Weapon") != std::string::npos) Add(snapshot, "Weapon ability", RecordName(resolved));
    }
    auto& add = Add(snapshot, "Add trait / perk / mount", "Choose an engine trait picker", Kind::AddPicker);
    add.category = "Faction/Traits/Picker";
    if (auto it = catalog.find(add.category); it != catalog.end()) for (const auto& record : it->second)
        if (allowed(record.address, 0)) add.choices.push_back({record.address, record.name});
    for (auto [offset, label] : {std::pair{0x198, "Active ability "}, {0x1b0, "Marker "}}) {
        const int n = VectorSize(member + offset, 8);
        const Ptr begin = Read<Ptr>(member + offset);
        for (int i = 0; i < n; ++i) Add(snapshot, std::string(label) + std::to_string(i + 1), RecordName(Read<Ptr>(begin + i * 8)));
    }
    const Ptr appearance = Read<Ptr>(member + 0x2d8);
    if (appearance) {
        static const char* names[] = {"Body", "Head", "Hair", "Accessory 1", "Accessory 2", "Body flair", "Body scar", "Helmet", "Shoulders", "Chest", "Arms", "Pants", "Legs", "Low LOD", "Head scar", "Head accessory", "Accessory 3", "Accessory 4", "FX 1", "FX 2", "FX 3", "FX 4", "FX 5"};
        Add(snapshot, "Body type", std::to_string(Read<uint32_t>(appearance)));
        RecordField(snapshot, catalog, "Base model", Read<Ptr>(appearance + 0x20), "Model");
        RecordField(snapshot, catalog, "Head record", Read<Ptr>(appearance + 0x170), "Model/SimpleModelHeads");
        for (uint32_t i = 0; i < 23; ++i) {
            RecordField(snapshot, catalog, names[i], Read<Ptr>(appearance + 0x28 + i * 8), "Model/SimpleModelPieces");
        }
        static constexpr uint32_t pieceSlots[] = {7,8,9,10,11,12,3,4,13,15,0,1,23,16,17};
        for (uint32_t i = 0; i < 15; ++i) {
            RecordField(snapshot, catalog, ("Armor design " + std::to_string(i)).c_str(), Read<Ptr>(appearance + 0xe0 + i * 8), "Inventory/ArmorData");
            auto& field = snapshot.fields.back();
            field.kind = Kind::Design;
            field.slot = i;
            if (!spawned && pieceSlots[i] < 23) if (auto it = catalog.find(field.category); it != catalog.end())
                for (const auto& record : it->second) if (Cell(record.address, "ArmorSlot", 0, 3, UINT32_MAX) == i && PieceForDesign(record.address, Read<uint32_t>(appearance)))
                    field.choices.push_back({record.address, record.name, record.address == field.expected});
        }
        for (int i = 0; i < 17; ++i) {
            char color[16]; std::snprintf(color, sizeof(color), "#%08X", Read<uint32_t>(appearance + 0x178 + i * 4));
            Add(snapshot, "Palette " + std::to_string(i), color);
        }
    }
}

bool Signature(Ptr address, const char* bytes, size_t count) {
    __try { return std::memcmp(reinterpret_cast<const void*>(game::FromRVA(address)), bytes, count) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Only POD lives in the SEH boundary; no UI callbacks touch native objects.
bool ApplyNative(Ptr member, Kind kind, Ptr expected, uint32_t slot, Ptr value) {
    __try {
        using Two = void(__fastcall*)(Ptr, Ptr);
        using Three = void(__fastcall*)(Ptr, Ptr, Ptr);
        if (kind == Kind::Level) {
            if (value > 84) return false;
            reinterpret_cast<void(__fastcall*)(Ptr, uint32_t)>(game::FromRVA(game::kFactionMemberSetXPLevel))(member, static_cast<uint32_t>(value));
            return Read<uint32_t>(member + 0x4b8) == value;
        }
        if (kind == Kind::Personality) {
            if (!Signature(game::kFactionMemberSetPersonality, "\x48\x89\x5c\x24\x18", 5)) return false;
            reinterpret_cast<Two>(game::FromRVA(game::kFactionMemberSetPersonality))(member, value);
            return Read<Ptr>(member + 0x270) == value;
        }
        if (kind == Kind::Role) {
            if (!Signature(game::kFactionMemberSetRole, "\x48\x89\x5c\x24\x08", 5)) return false;
            reinterpret_cast<Three>(game::FromRVA(game::kFactionMemberSetRole))(member, value, 0);
            return Read<Ptr>(member + 0x318) == value;
        }
        if (kind == Kind::Tribe) {
            if (!Signature(game::kFactionMemberSetTribe, "\x48\x89\x5c\x24\x10", 5)) return false;
            reinterpret_cast<Three>(game::FromRVA(game::kFactionMemberSetTribe))(member, value, reinterpret_cast<Ptr>("Uruks editor"));
            return Read<Ptr>(member + 0x328) == value;
        }
        if (kind == Kind::Equipment) {
            if (Read<Ptr>(member + 0x48) || slot >= 25 || !ItemFits(value, slot) ||
                !Signature(game::kFactionMemberGiveItem, "\x48\x8b\xc4", 3)) return false;
            Ptr old = Read<Ptr>(member + 0x50 + slot * 8);
            if (Read<Ptr>(old + 0x38) != expected) return false;
            reinterpret_cast<bool(__fastcall*)(Ptr, Ptr, Ptr, uint8_t, Ptr)>(game::FromRVA(game::kFactionMemberGiveItem))(
                member + 0x50, member, value, 8, Read<Ptr>(member + 0x2d8));
            Ptr instance = Read<Ptr>(member + 0x50 + slot * 8);
            return Read<Ptr>(instance + 0x38) == value;
        }
        if (kind == Kind::Design) {
            // A spawned body's residency and rig cannot safely be changed by a storage edit.
            const uint32_t pieceSlots[] = {7,8,9,10,11,12,3,4,13,15,0,1,23,16,17};
            if (Read<Ptr>(member + 0x48) || slot >= 15 || pieceSlots[slot] >= 23 ||
                Cell(value, "ArmorSlot", 0, 3, UINT32_MAX) != slot ||
                !Signature(game::kAppearanceSetPiece, "\x48\x8b\xc4", 3)) return false;
            Ptr appearance = Read<Ptr>(member + 0x2d8);
            if (!appearance || Read<Ptr>(appearance + 0xe0 + slot * 8) != expected) return false;
            Ptr piece = PieceForDesign(value, Read<uint32_t>(appearance));
            if (!piece) return false;
            reinterpret_cast<bool(__fastcall*)(Ptr, uint32_t, Ptr, Ptr, Ptr)>(game::FromRVA(game::kAppearanceSetPiece))(appearance, pieceSlots[slot], piece, value, 0);
            return Read<Ptr>(appearance + 0x28 + pieceSlots[slot] * 8) == piece;
        }
        if (kind == Kind::Picker || kind == Kind::AddPicker) {
            if (Read<uint64_t>(member + 0x508) & (1ull << 38)) return false;
            if (!Signature(game::kFactionMemberAddPicker, "\x48\x89\x5c\x24\x08", 5) ||
                !Signature(game::kFactionMemberRemovePickerSlot, "\x48\x89\x5c\x24\x08", 5) ||
                !Signature(game::kFactionMemberDeactivateTrait, "\x48\x89\x5c\x24\x10", 5) ||
                !Signature(game::kFactionMemberSetFlag, "\x48\x81\xc1\x08\x05\x00\x00", 7) ||
                !Signature(game::kFactionMemberUpdateTraits, "\x40\x53\x48\x83\xec\x20", 6)) return false;
            int count = VectorSize(member + 0x1e0, 0x20, 1024);
            Ptr begin = Read<Ptr>(member + 0x1e0);
            int index = -1;
            for (int i = 0; i < count; ++i) {
                Ptr picker = Read<Ptr>(begin + i * 0x20);
                if (kind == Kind::Picker && picker == expected) index = i;
                if (value && picker == value) return picker == expected;
            }
            if (kind == Kind::Picker && index < 0) return false;
            if (value && (!PickerAllowed(member, value, expected) || !reinterpret_cast<bool(__fastcall*)(Ptr, Ptr)>(game::FromRVA(game::kFactionMemberAddPicker))(member, value))) return false;
            if (index >= 0) {
                begin = Read<Ptr>(member + 0x1e0);
                Ptr trait = Read<Ptr>(begin + index * 0x20 + 8);
                if (trait) reinterpret_cast<Two>(game::FromRVA(game::kFactionMemberDeactivateTrait))(member, trait);
                reinterpret_cast<void(__fastcall*)(Ptr, int)>(game::FromRVA(game::kFactionMemberRemovePickerSlot))(member + 0x1e0, index);
            }
            reinterpret_cast<void(__fastcall*)(Ptr, uint32_t, bool)>(game::FromRVA(game::kFactionMemberSetFlag))(member, 34, true);
            reinterpret_cast<void(__fastcall*)(Ptr, uint32_t)>(game::FromRVA(game::kFactionMemberUpdateTraits))(member, 0);
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Task { uint64_t session; Key selected; bool edit = false; Field field; Choice choice; };
void Run(void* context) {
    std::unique_ptr<Task> task(static_cast<Task*>(context));
    auto finish = [] { g_pending.store(false); };
    if (!g_ui || !task->session || g_ui->SaveSession() != task->session) { finish(); return; }
    try {
        Snapshot snapshot;
        snapshot.session = task->session;
        snapshot.selected = task->selected;
        snapshot.targets = Targets();
        const Catalog catalog = LoadCatalog();
        std::string status;
        if (task->edit) {
            auto member = std::find_if(snapshot.targets.begin(), snapshot.targets.end(), [&](const auto& t) { return t.key == task->selected; });
            const bool recordValid = task->field.kind == Kind::Level || task->choice.value == 0 ||
                InCatalog(catalog, task->field.category, task->choice.value);
            const bool ok = member != snapshot.targets.end() && recordValid &&
                ApplyNative(member->member, task->field.kind, task->field.expected, task->field.slot, task->choice.value);
            status = ok ? "Applied: " : "Rejected or constrained by engine: ";
            status += task->field.label + " -> " + task->choice.label;
            Log(status.c_str());
        }
        Capture(snapshot, catalog);
        if (!status.empty()) Add(snapshot, "Last edit", status);
        if (g_ui->SaveSession() == task->session) {
            std::lock_guard lock(g_mutex);
            g_snapshot = std::move(snapshot);
        }
    } catch (...) { Log("Uruk editor snapshot failed"); }
    finish();
}
bool Queue(std::unique_ptr<Task> task) {
    if (g_pending.exchange(true)) return false;
    if (!g_ui->QueueGameTask(Run, task.get())) { g_pending.store(false); return false; }
    task.release();
    return true;
}
void Refresh(void*) {
    auto task = std::make_unique<Task>();
    task->session = g_ui->SaveSession();
    { std::lock_guard lock(g_mutex); if (g_snapshot.session == task->session) task->selected = g_snapshot.selected; }
    if (task->session) Queue(std::move(task));
}
bool Current() { return g_snapshot.session != 0 && g_snapshot.session == g_ui->SaveSession(); }
int Count(void*) { std::lock_guard lock(g_mutex); return Current() ? static_cast<int>(g_snapshot.fields.size()) : 1; }
bool ReadField(void*, int index, HagUI_Property* property) {
    if (!property || index < 0) return false;
    std::lock_guard lock(g_mutex);
    if (!Current()) {
        if (index) return false;
        *property = {};
        strcpy_s(property->label, "State");
        strcpy_s(property->value, g_pending.load() ? "Loading captain state..." : "No loaded world. Refresh after loading a save.");
        return true;
    }
    if (index >= static_cast<int>(g_snapshot.fields.size())) return false;
    const auto& field = g_snapshot.fields[index];
    *property = {};
    property->id = field.id;
    strncpy_s(property->label, field.label.c_str(), _TRUNCATE);
    strncpy_s(property->value, field.value.c_str(), _TRUNCATE);
    property->editable = !g_pending.load() && !field.choices.empty();
    if (!property->editable) strncpy_s(property->readOnlyReason, g_pending.load() ? "Pending game-thread operation" :
        field.kind == Kind::View ? "Engine-managed field: native setter not yet verified" :
        field.kind == Kind::Equipment || field.kind == Kind::Design ? "No compatible records, or captain currently spawned" :
        "No compatible records or trait changes locked by the engine", _TRUNCATE);
    return true;
}
const Field* Find(uint64_t id) {
    if (!Current()) return nullptr;
    auto it = std::find_if(g_snapshot.fields.begin(), g_snapshot.fields.end(), [id](const auto& f) { return f.id == id; });
    return it == g_snapshot.fields.end() ? nullptr : &*it;
}
int ChoiceCount(void*, uint64_t id) { std::lock_guard lock(g_mutex); auto field = Find(id); return field ? static_cast<int>(field->choices.size()) : 0; }
bool ReadChoice(void*, uint64_t id, int index, HagUI_Choice* out) {
    std::lock_guard lock(g_mutex);
    auto field = Find(id);
    if (!out || !field || index < 0 || index >= static_cast<int>(field->choices.size())) return false;
    *out = {};
    out->id = index + 1;
    out->selected = field->choices[index].selected;
    strncpy_s(out->label, field->choices[index].label.c_str(), _TRUNCATE);
    return true;
}
bool Apply(void*, uint64_t id, uint64_t choiceId) {
    auto task = std::make_unique<Task>();
    {
        std::lock_guard lock(g_mutex);
        const Field* field = Find(id);
        if (!field || choiceId == 0 || choiceId > field->choices.size()) return false;
        const Choice& choice = field->choices[choiceId - 1];
        task->session = g_snapshot.session;
        task->selected = g_snapshot.selected;
        if (field->kind == Kind::Target) {
            if (choice.value >= g_snapshot.targets.size()) return false;
            task->selected = g_snapshot.targets[choice.value].key;
        } else {
            task->edit = true;
            task->field = *field;
            task->choice = choice;
        }
    }
    return Queue(std::move(task));
}
}
void AddUrukEditor(int parent, HagUIAPI* ui) {
    if (!ui || ui->abiVersion < 10) return;
    g_ui = ui;
    const int page = ui->RegisterSubPage(parent, "Uruk Editor");
    if (page < 0) return;
    const HagUI_PropertySource source{Refresh, Count, ReadField, ChoiceCount, ReadChoice, Apply};
    ui->AddPropertyEditor(page, nullptr, &source);
    ui->AddSubPageButton(parent, "Uruk Editor", page);
}
}
