#include "UrukEditor.h"
#include "GameOffsets.h"
#include "Settings.h"
#include "GeneratorNative.h"
#include <chrono>
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
enum class Kind { View, Target, Level, Role, Picker, Equipment, Name, Appearance, Traits, Advance };
bool Identity(Kind k) { return k==Kind::Name || k==Kind::Role || k==Kind::Appearance; }
bool Generator(Kind k) { return Identity(k) || k==Kind::Traits || k==Kind::Advance; }
struct Choice {
    uint64_t value; std::string label; bool selected = false;
    std::string preview;
    engine::Look look{};
    std::vector<Ptr> pickers;
    uint64_t ticket=0;
    engine::Advancement advance{};
    const char* disabled=nullptr;
};
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
    bool supported=false;
    bool mayGenerate=false, mayRollTraits=false, mayAdvance=false;
    uint64_t baseline=0;
    std::string status;
};
struct Draft {
    std::map<Kind,std::pair<uint64_t,Choice>> edits;
    bool dirty() const { return !edits.empty(); }
    bool permits(Kind k,uint64_t field) const {
        if (!dirty()) return true;
        if (Identity(k)) return Identity(edits.begin()->first);
        return edits.size()==1 && edits.begin()->second.first==field;
    }
    bool missing(Kind k) const {
        return edits.count(Kind::Role) && (k==Kind::Name || k==Kind::Appearance) && !edits.count(k);
    }
    bool ready() const { return dirty() && !missing(Kind::Name) && !missing(Kind::Appearance); }
    void stage(const Field& f,const Choice& c) {
        if (f.kind==Kind::Role) { edits.erase(Kind::Name); edits.erase(Kind::Appearance); }
        edits[f.kind]={f.id,c};
    }
};
Draft g_draft;
uint64_t g_generationRequest=0;
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
        if (name!="Faction/Traits/Picker" && name!="Faction/Roles/Definitions" &&
            name!="Faction/Contracts/Contract" && name!="Inventory/Items") continue;
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
std::vector<Target> Targets(Ptr faction = 0) {
    std::vector<Target> result;
    const Ptr systems = Read<Ptr>(game::FromRVA(game::kGameSystemsSingleton));
    if (!faction) faction = Read<Ptr>(systems + 0x6c68);
    const Ptr pools = Read<Ptr>(faction + 0x68);
    const uint32_t count = Read<uint32_t>(faction + 0x70);
    if (!faction || !pools || count > 32) return result;
    for (uint32_t i = 0; i < count; ++i) {
        const Ptr pool = pools + i * 0xc0 + 8;
        auto poolName=RecordName(Read<Ptr>(pool-8));
        if (poolName!="MainGame" && poolName!="DLC3" && poolName!="DLC4") continue;
        const int members = VectorSize(pool + 0x18, 8, 4096);
        const Ptr list = Read<Ptr>(pool + 0x18);
        for (int j = 0; j < members; ++j) {
            const Ptr member = Read<Ptr>(list + j * 8);
            if (!member || Read<Ptr>(member) != game::FromRVA(game::kFactionMemberVtable) ||
                Read<uint32_t>(member + 0x174) != static_cast<uint32_t>(j)) continue;
            if (!Read<Ptr>(member + 0x268) || Read<Ptr>(member + 0x190)) continue;
            const auto base=RecordName(Read<Ptr>(Read<Ptr>(member+0x2d8)+0x20));
            if (_stricmp(base.c_str(),"orc_anim_base") && _stricmp(base.c_str(),"olog_anim_base")) continue;
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
    const bool writable = snapshot.supported && !spawned && !(flags & ((1ull<<37)|4)) && !Read<Ptr>(member+0x190);
    snapshot.mayGenerate=writable;
    snapshot.mayRollTraits=writable && !(flags&(1ull<<38)) && !Read<Ptr>(member+0x3a0)
        && !VectorSize(member+0x1c8,8);
    std::map<unsigned,std::vector<Choice>> equipment;
    if (writable) {
        Ptr kit[512]{};
        int n=engine::KitItems(member,kit);
        for (int j=0;j<n;++j) {
            unsigned slot=0; Ptr look=0;
            if (!InCatalog(catalog,"Inventory/Items",kit[j]) || !engine::WeaponLook(member,kit[j],slot,look)) continue;
            Choice c{kit[j],RecordName(kit[j])};
            c.preview="Look: "+RecordName(look);
            Ptr model=Cell(look,"Model");
            for (unsigned i=0;i<32;++i) {
                Ptr variant=Cell(model,"Models",i);
                if (!variant) break;
                c.preview+="\nModel: "+RecordName(Cell(variant,"ModelRecord"));
            }
            c.look.base=look;
            equipment[slot].push_back(std::move(c));
        }
    }
    auto& name=Add(snapshot,"Name generator",Text(member+0x498),Kind::Name);
    name.expected=Read<unsigned>(member+0x170);
    auto& title=Add(snapshot,"Title generator",RecordName(Read<Ptr>(member+0x318)),Kind::Role);
    title.expected=Read<Ptr>(member+0x318);
    Add(snapshot,"Appearance generator",RecordName(Read<Ptr>(Read<Ptr>(member+0x2d8)+0x170)),Kind::Appearance);
    Add(snapshot,"Trait generator","Natural picker set",Kind::Traits);
    Add(snapshot,"Level-up reward","Advance one level",Kind::Advance);
    Add(snapshot, "State", (flags & 4) ? "Dead" : "Alive");
    auto& level = Add(snapshot, "Level", std::to_string(Read<uint32_t>(member + 0x4b8) + 1), Kind::Level);
    level.expected=Read<unsigned>(member+0x4b8);
    const int cap=writable?engine::LevelCap(member):-1;
    snapshot.mayAdvance=int(level.expected)<cap && !(flags&(1ull<<38)) && !Read<Ptr>(member+0x4f0);
    if (writable && cap>=0) for (int i=0;i<85;++i) {
        Choice c{unsigned(i),std::to_string(i+1),unsigned(i)==level.expected};
        if (i>cap) c.disabled="Above this captain's native level cap";
        level.choices.push_back(std::move(c));
    }
    RecordField(snapshot,catalog,"Personality / voice",Read<Ptr>(member+0x270),"Faction/Personalities");
    RecordField(snapshot,catalog,"Tribe",Read<Ptr>(member+0x328),"Faction/Tribes/Definitions");
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
        field.choices=std::move(equipment[i]);
        for (auto& c:field.choices) c.selected=c.value==item;
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
        // Never let a user append an arbitrary picker outside its generation budget.
        if (name.find("Mount") != std::string::npos || name.find("Caragor") != std::string::npos)
            Add(snapshot, "Mount ability", RecordName(resolved));
        if (name.find("Weapon") != std::string::npos) Add(snapshot, "Weapon ability", RecordName(resolved));
    }
    // Only complete sets from the native picker phase can be staged.
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
        for (uint32_t i=0;i<15;++i)
            RecordField(snapshot,catalog,("Armor design "+std::to_string(i)).c_str(),Read<Ptr>(appearance+0xe0+i*8),"Inventory/ArmorData");
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
        if (kind == Kind::Role) {
            if (!Signature(game::kFactionMemberSetRole, "\x48\x89\x5c\x24\x08", 5)) return false;
            reinterpret_cast<Three>(game::FromRVA(game::kFactionMemberSetRole))(member, value, 0);
            return Read<Ptr>(member + 0x318) == value;
        }
        if (kind == Kind::Equipment) {
            if (Read<Ptr>(member + 0x48) || slot >= 10 || !engine::WeaponAllowed(member,value) ||
                !Signature(game::kFactionMemberGiveItem, "\x48\x8b\xc4", 3)) return false;
            Ptr old = Read<Ptr>(member + 0x50 + slot * 8);
            if (Read<Ptr>(old + 0x38) != expected) return false;
            reinterpret_cast<bool(__fastcall*)(Ptr, Ptr, Ptr, uint8_t, Ptr)>(game::FromRVA(game::kFactionMemberGiveItem))(
                member + 0x50, member, value, 8, Read<Ptr>(member + 0x2d8));
            Ptr instance = Read<Ptr>(member + 0x50 + slot * 8);
            return Read<Ptr>(instance + 0x38) == value;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint64_t Stamp(Ptr m) {
    uint64_t h=1469598103934665603ull;
    auto mix=[&](uint64_t v){ h=(h^v)*1099511628211ull; };
    for (int o : {0x48,0x168,0x178,0x188,0x190,0x268,0x270,0x278,0x280,0x318,0x328,0x370,0x390,0x3a0,0x4b8,0x508,0x510,0x518}) mix(Read<Ptr>(m+o));
    mix(Read<unsigned>(m+0x170));
    for (int i=0;i<25;++i) {
        Ptr item=Read<Ptr>(m+0x50+i*8);
        if (item) for (int o:{0x38,0x40,0x48}) mix(Read<Ptr>(item+o));
        mix(item);
    }
    auto vector=[&](Ptr v,unsigned stride) {
        int n=VectorSize(v,stride); mix(n);
        Ptr p=Read<Ptr>(v);
        for (int i=0;i<n;++i) for (unsigned o=0;o<stride;o+=8) mix(Read<Ptr>(p+i*stride+o));
    };
    vector(m+0x1e0,32); vector(m+0x198,8); vector(m+0x1b0,8);
    vector(m+0x1c8,8); vector(m+0x1f8,8);
    for (int o:{0x470,0x4c0,0x4f0}) mix(Read<Ptr>(m+o));
    Ptr a=Read<Ptr>(m+0x2d8);
    if (a) {
        mix(Read<unsigned>(a)); vector(a+8,16); vector(a+0x158,8);
        for (unsigned o=0x20;o<0x158;o+=8) mix(Read<Ptr>(a+o));
        mix(Read<Ptr>(a+0x170));
        for (unsigned i=0;i<17;++i) mix(Read<unsigned>(a+0x178+i*4));
    }
    return h;
}
bool Writable(Ptr m) {
    return m && engine::Supported() && Read<Ptr>(m+0x268) && !Read<Ptr>(m+0x48) && !Read<Ptr>(m+0x190)
        && !(Read<uint64_t>(m+0x508)&(4|(1ull<<37)));
}
bool Roll(Ptr m, Kind k, Ptr role, const Catalog& catalog, Choice& c) {
    if (!Writable(m)) return false;
    c.ticket=g_nextField.fetch_add(1);
    if (k==Kind::Name) {
        unsigned id=0; char label[256]{};
        if (!engine::GenerateName(m,id,label) || !engine::NameAvailable(m,id)) return false;
        c.value=id; c.label=label; c.preview=c.label; return true;
    }
    if (k==Kind::Role) {
        for (int i=0;i<8;++i) {
            Ptr r=engine::GenerateTitle(m);
            if (!r || r==Read<Ptr>(m+0x318) || !InCatalog(catalog,"Faction/Roles/Definitions",r)) continue;
            c.value=r; c.label=RecordName(r); c.preview=c.label; return true;
        }
        return false;
    }
    if (k==Kind::Appearance) {
        for (int i=0;i<8;++i) {
            c.value=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())*0x9e3779b97f4a7c15ull+c.ticket+i;
            if (!engine::Appearance(m,role,c.value,c.look,nullptr)) continue;
            c.label=RecordName(c.look.head);
            c.preview="Title: "+RecordName(role)+"\nBody: "+RecordName(c.look.pieces[0])+"\nHead: "+c.label;
            for (unsigned j=2;j<23;++j) if (c.look.pieces[j]) c.preview+="\n"+RecordName(c.look.pieces[j]);
            return true;
        }
        return false;
    }
    if (k==Kind::Traits) {
        Ptr roll[512]{};
        const int n=engine::TraitRoll(m,roll);
        if (!n) return false;
        std::set<Ptr> unique;
        for (int i=0;i<n;++i) {
            if (!InCatalog(catalog,"Faction/Traits/Picker",roll[i]) || !unique.insert(roll[i]).second) return false;
            c.pickers.push_back(roll[i]);
        }
        c.label=std::to_string(n)+" generated pickers";
        c.preview="Replacement set ("+std::to_string(n)+"):";
        for (auto p:c.pickers) c.preview+="\n"+RecordName(p);
        c.value=c.ticket;
        return true;
    }
    if (k==Kind::Advance) {
        if (!engine::RollAdvancement(m,c.advance)) return false;
        c.label="Level "+std::to_string(c.advance.level+1)+": "+RecordName(c.advance.reward);
        c.preview=c.label+"\nReward list: "+RecordName(c.advance.list)+"\nThe game rolls the final outcome on Save:";
        for (bool remove:{false,true}) {
            Ptr possible[128]{};
            int n=engine::RewardPickers(m,c.advance.reward,remove,possible);
            for (int i=0;i<n;++i) c.preview+=(remove?"\nMay remove: ":"\nMay gain: ")+RecordName(possible[i]);
        }
        c.value=c.ticket; return true;
    }
    return false;
}
bool SaveTraitsImpl(Ptr m,const std::vector<Ptr>& wanted) {
    if (wanted.empty() || Read<Ptr>(m+0x3a0) || VectorSize(m+0x1c8,8) || Read<uint64_t>(m+0x508)&(1ull<<38)) return false;
    if (!Signature(game::kFactionMemberAddPicker,"\x48\x89\x5c\x24\x08",5) ||
        !Signature(game::kFactionMemberRemovePickerSlot,"\x48\x89\x5c\x24\x08",5) ||
        !Signature(game::kFactionMemberDeactivateTrait,"\x48\x89\x5c\x24\x10",5) ||
        !Signature(game::kFactionMemberUpdateTraits,"\x40\x53\x48\x83\xec\x20",6) ||
        !Signature(game::kFactionMemberSetFlag,"\x48\x81\xc1\x08\x05\x00\x00",7)) return false;
    const int old=VectorSize(m+0x1e0,32,1024);
    auto slot=[&](int i){ return Read<Ptr>(m+0x1e0)+32*i; };
    auto erase=reinterpret_cast<void(*)(Ptr,int)>(game::FromRVA(game::kFactionMemberRemovePickerSlot));
    // Keep existing matching slots, including their mission state. Append first; until
    // the rebuild, failed additions can be removed without touching active abilities.
    for (auto p:wanted) {
        bool held=false;
        for (int i=0;i<old;++i) if (Read<Ptr>(slot(i))==p) held=true;
        if (!held && !reinterpret_cast<bool(*)(Ptr,Ptr)>(game::FromRVA(game::kFactionMemberAddPicker))(m,p)) {
            for (int i=VectorSize(m+0x1e0,32,2048)-1;i>=old;--i) erase(m+0x1e0,i);
            return false;
        }
    }
    for (int i=old-1;i>=0;--i) {
        if (std::find(wanted.begin(),wanted.end(),Read<Ptr>(slot(i)))!=wanted.end()) continue;
        Ptr trait=Read<Ptr>(slot(i)+8);
        if (trait) reinterpret_cast<void(*)(Ptr,Ptr)>(game::FromRVA(game::kFactionMemberDeactivateTrait))(m,trait);
        erase(m+0x1e0,i);
    }
    reinterpret_cast<void(*)(Ptr,unsigned,bool)>(game::FromRVA(game::kFactionMemberSetFlag))(m,34,true);
    reinterpret_cast<void(*)(Ptr,unsigned)>(game::FromRVA(game::kFactionMemberUpdateTraits))(m,0);
    return VectorSize(m+0x1e0,32)==int(wanted.size());
}
bool SaveTraits(Ptr m,const std::vector<Ptr>& wanted) {
    __try { return SaveTraitsImpl(m,wanted); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
enum class Op { Refresh, Generate, Save };
struct Task { uint64_t session=0, baseline=0, field=0, request=0; Key selected; Op op=Op::Refresh; Kind kind=Kind::View; Draft draft; unsigned slot=0; Ptr expected=0; };
Ptr DraftRole(Ptr m,const Draft& d) {
    auto it=d.edits.find(Kind::Role);
    return it==d.edits.end()?Read<Ptr>(m+0x318):it->second.second.value;
}
void AppendOrderReport(Choice& c,Kind k,Ptr m,const Draft& draft,const Catalog& catalog) {
    auto it=catalog.find("Faction/Contracts/Contract");
    if (it==catalog.end()) { c.preview+="\nTraining order data unavailable."; return; }
    const auto level=k==Kind::Advance?c.advance.level:Read<unsigned>(m+0x4b8);
    Ptr role=k==Kind::Role?c.value:DraftRole(m,draft);
    unsigned name=Read<unsigned>(m+0x170);
    if (auto n=draft.edits.find(Kind::Name);n!=draft.edits.end()) name=unsigned(n->second.second.value);
    if (k==Kind::Name) name=unsigned(c.value);
    Ptr active[512]{};
    int count=0;
    if (k==Kind::Traits) count=engine::ProjectTraits(m,c.pickers.data(),int(c.pickers.size()),level,
        Cell(Read<Ptr>(m+0x268),"Rank"),active);
    unsigned passed=0,blocked=0,unknown=0;
    std::string examples;
    for (const auto& order:it->second) {
        Ptr reason=0;
        int r=engine::OrderCheck(m,order.address,level,role,name,k==Kind::Traits?active:nullptr,count,reason);
        if (r==0) ++passed;
        else if (r<0) ++unknown;
        else {
            ++blocked;
            if (blocked<=3) examples+="\n"+order.name+": "+RecordName(reason);
        }
    }
    c.preview="Training orders: "+std::to_string(passed)+" without a failure, "+std::to_string(blocked)+
        " blocked, "+std::to_string(unknown)+" unknown.\n\n"+c.preview+examples;
    if (k==Kind::Advance) c.preview+="\nLevel projected; final random reward not yet applied.";
}
bool Commit(Ptr m,const Task& task,const Catalog& catalog,std::string& status) {
    const auto& d=task.draft;
    if (!d.ready() || !Writable(m)) return false;
    auto get=[&](Kind k)->const Choice* {auto i=d.edits.find(k); return i==d.edits.end()?nullptr:&i->second.second;};
    if (auto l=get(Kind::Level))
        return int(l->value)<=engine::LevelCap(m) && ApplyNative(m,Kind::Level,0,0,l->value);
    if (auto w=get(Kind::Equipment)) {
        Ptr kit[512]{},look=0; int n=engine::KitItems(m,kit); unsigned slot=0;
        return std::find(kit,kit+n,w->value)!=kit+n && engine::WeaponLook(m,w->value,slot,look) && slot==task.slot
            && look==w->look.base && ApplyNative(m,Kind::Equipment,task.expected,slot,w->value);
    }
    if (auto t=get(Kind::Traits)) {
        for (auto p:t->pickers) if (!InCatalog(catalog,"Faction/Traits/Picker",p)) return false;
        return SaveTraits(m,t->pickers);
    }
    if (auto a=get(Kind::Advance)) {
        int result=engine::Advance(m,a->advance);
        if (result==2) status="Advanced one level. The normal engine roll replaced the previewed reward.";
        return result!=0;
    }
    const auto* name=get(Kind::Name); const auto* a=get(Kind::Appearance); const auto* title=get(Kind::Role);
    Ptr role=DraftRole(m,d);
    if (name && !engine::NameAvailable(m,unsigned(name->value))) return false;
    if (title && (!InCatalog(catalog,"Faction/Roles/Definitions",role) ||
        !Signature(game::kFactionMemberSetRole,"\x48\x89\x5c\x24\x08",5))) return false;
    if (a) {
        engine::Look result{};
        if (!engine::Appearance(m,role,a->value,result,&a->look)) return false;
    }
    if (title && !ApplyNative(m,Kind::Role,0,0,role)) return false;
    return !name || engine::SetName(m,unsigned(name->value));
}
void Run(void* context) {
    std::unique_ptr<Task> task(static_cast<Task*>(context));
    struct Finish { ~Finish(){ g_pending.store(false); } } finish;
    if (!g_ui || !task->session || g_ui->SaveSession()!=task->session) return;
    try {
        Snapshot snapshot;
        snapshot.session=task->session; snapshot.selected=task->selected;
        snapshot.targets=Targets(); snapshot.supported=engine::Supported();
        auto it=std::find_if(snapshot.targets.begin(),snapshot.targets.end(),[&](const auto& t){return t.key==task->selected;});
        Ptr m=it==snapshot.targets.end()?0:it->member;
        const Catalog catalog=LoadCatalog();
        if (task->op!=Op::Refresh && (!m || Stamp(m)!=task->baseline)) {
            snapshot.status="Captain changed. Draft discarded; no edits saved.";
        } else if (task->op==Op::Generate) {
            Choice candidate;
            bool ok=Roll(m,task->kind,DraftRole(m,task->draft),catalog,candidate);
            if (ok) AppendOrderReport(candidate,task->kind,m,task->draft,catalog);
            std::lock_guard lock(g_mutex);
            if (task->request!=g_generationRequest || g_snapshot.session!=g_ui->SaveSession() || g_snapshot.session!=task->session) return;
            for (auto& f:g_snapshot.fields) if (f.id==task->field) {
                f.choices.clear();
                if (ok) f.choices.push_back(std::move(candidate));
                g_snapshot.status=ok?"Apply to draft or cancel.":"No compatible roll. Try generating again.";
            }
            return;
        } else if (task->op==Op::Save) {
            bool saved=Commit(m,*task,catalog,snapshot.status);
            if (snapshot.status.empty()) snapshot.status=saved?"Saved to the captain.":"Save rejected by the engine. Captain state refreshed.";
            Log(snapshot.status.c_str());
        }
        Capture(snapshot,catalog);
        snapshot.baseline=m?Stamp(m):0;
        if (!snapshot.status.empty()) Add(snapshot,"Last operation",snapshot.status);
        if (g_ui->SaveSession()==task->session) {
            std::lock_guard lock(g_mutex);
            g_snapshot=std::move(snapshot); g_draft={};
        }
    } catch (...) { Log("Uruk editor operation failed"); }
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
    { std::lock_guard lock(g_mutex); if (g_snapshot.session == task->session) {
        if (g_draft.dirty()) return;
        task->selected = g_snapshot.selected;
    } }
    if (task->session) Queue(std::move(task));
}
bool Current() { return g_snapshot.session != 0 && g_snapshot.session == g_ui->SaveSession(); }
bool Enabled(const Field& f) {
    if (g_pending || !g_draft.permits(f.kind,f.id)) return false;
    if (Generator(f.kind)) return g_snapshot.mayGenerate && (f.kind!=Kind::Traits || g_snapshot.mayRollTraits) &&
        (f.kind!=Kind::Advance || g_snapshot.mayAdvance);
    return !f.choices.empty();
}
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
    if (auto d=g_draft.edits.find(field.kind); d!=g_draft.edits.end() && d->second.first==field.id)
        strncpy_s(property->value,(d->second.second.label+" (unsaved)").c_str(),_TRUNCATE);
    if (g_draft.missing(field.kind)) strcpy_s(property->value,"Regeneration required for the new title");
    property->editable = Enabled(field);
    if (!property->editable) strncpy_s(property->readOnlyReason, g_pending.load() ? "Pending game-thread operation" :
        !g_draft.permits(field.kind,field.id) ? "Save or discard the current draft first" :
        field.kind == Kind::Picker ? "Part of a generated set. Use Trait generator." :
        Generator(field.kind) ? "Requires a supported, living, unspawned procedural captain" :
        field.kind == Kind::View ? "Engine-managed field" :
        field.kind == Kind::Equipment ? "No compatible records from this captain's kit, or captain currently spawned" :
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
    out->id = field->choices[index].ticket ? field->choices[index].ticket : index + 1;
    out->selected = field->choices[index].selected;
    strncpy_s(out->label, field->choices[index].label.c_str(), _TRUNCATE);
    return true;
}
const Choice* ChoiceById(const Field& f,uint64_t id) {
    if (!id) return nullptr;
    if (Generator(f.kind)) return !f.choices.empty() && f.choices.front().ticket==id?&f.choices.front():nullptr;
    return id<=f.choices.size()?&f.choices[size_t(id-1)]:nullptr;
}
bool Apply(void*,uint64_t id,uint64_t choiceId) {
    std::lock_guard lock(g_mutex);
    const Field* f=Find(id);
    if (!f || !Enabled(*f)) return false;
    const Choice* c=ChoiceById(*f,choiceId);
    if (!c || c->disabled) return false;
    if (f->kind==Kind::Target) {
        if (c->value>=g_snapshot.targets.size()) return false;
        auto t=std::make_unique<Task>();
        t->session=g_snapshot.session; t->selected=g_snapshot.targets[c->value].key;
        return Queue(std::move(t));
    }
    g_draft.stage(*f,*c);
    if (f->kind==Kind::Role) for (auto& other:g_snapshot.fields)
        if (other.kind==Kind::Name || other.kind==Kind::Appearance) other.choices.clear();
    return true;
}
void State(void*,uint64_t id,uint64_t choice,HagUI_PopupState* out) {
    *out={};
    std::lock_guard lock(g_mutex);
    if (!Current()) return;
    out->dirty=g_draft.dirty(); out->busy=g_pending;
    out->canSave=g_draft.ready() && !out->busy;
    strncpy_s(out->status,g_draft.dirty()&&!g_draft.ready()?"Regenerate and apply the marked fields.":g_snapshot.status.c_str(),_TRUNCATE);
    if (auto f=Find(id)) {
        out->generator=Generator(f->kind); out->needsRepair=g_draft.missing(f->kind);
        if (auto c=ChoiceById(*f,choice)) {
            if (c->disabled) {out->unavailable=true; strncpy_s(out->status,c->disabled,_TRUNCATE);}
            strncpy_s(out->preview,c->preview.c_str(),_TRUNCATE);
            std::copy_n(c->look.palette,17,out->palette);
        }
    }
}
std::unique_ptr<Task> DraftTask(Op op) {
    auto t=std::make_unique<Task>();
    t->op=op; t->session=g_snapshot.session; t->selected=g_snapshot.selected;
    t->baseline=g_snapshot.baseline; t->draft=g_draft;
    return t;
}
bool Generate(void*,uint64_t id) {
    std::lock_guard lock(g_mutex);
    auto f=Find(id);
    if (!f || !Generator(f->kind) || !Enabled(*f)) return false;
    auto t=DraftTask(Op::Generate); t->kind=f->kind; t->field=id; t->request=++g_generationRequest;
    return Queue(std::move(t));
}
void Save(void*) {
    std::lock_guard lock(g_mutex);
    if (!Current() || !g_draft.ready() || g_pending) return;
    auto t=DraftTask(Op::Save);
    if (auto e=g_draft.edits.find(Kind::Equipment); e!=g_draft.edits.end()) {
        auto f=Find(e->second.first);
        if (!f) return;
        t->expected=f->expected; t->slot=f->slot;
    }
    Queue(std::move(t));
}
void Discard(void*) {
    std::lock_guard lock(g_mutex);
    if (g_pending) return;
    g_draft={};
    for (auto& f:g_snapshot.fields) if (Generator(f.kind)) f.choices.clear();
    g_snapshot.status="Draft discarded.";
}
void Close(void*,uint64_t field) {
    std::lock_guard lock(g_mutex);
    ++g_generationRequest;
    for (auto& f:g_snapshot.fields) if (f.id==field && Generator(f.kind)) f.choices.clear();
}
}
void AddUrukEditor(int parent, HagUIAPI* ui) {
    if (!ui || ui->abiVersion < 11 || !ui->AddPopupEditor) return;
    g_ui = ui;
    const int page = ui->RegisterSubPage(parent, "Uruk Editor");
    if (page < 0) return;
    const HagUI_PropertySource source{Refresh, Count, ReadField, ChoiceCount, ReadChoice, Apply};
    const HagUI_PopupActions popup{"Save Uruk",State,Generate,Save,Discard,Close};
    ui->AddPopupEditor(page,nullptr,&source,&popup);
    ui->AddSubPageButton(parent, "Uruk Editor", page);
}
}
