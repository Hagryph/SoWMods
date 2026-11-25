#include "UrukEditor.h"
#include "GameOffsets.h"
#include "Settings.h"
#include "GeneratorNative.h"
#include "CaptainRecipe.h"
#include <chrono>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
enum class Kind { View, Target, Level, Role, Picker, Equipment, Name, Appearance, Traits, Advance,
    Class, Base, Tribe, Rarity, Immunities, Hates, Classes, Bonuses, Mortal, Vulnerable, Dazed, Hints, Regenerate };
bool Selector(Kind k) { return k>=Kind::Class && k<=Kind::Rarity; }
bool RecipeField(Kind k) { return k>=Kind::Class && k<=Kind::Regenerate; }
bool Bucket(Kind k) { return k>=Kind::Immunities && k<=Kind::Hints; }
bool Identity(Kind k) { return k==Kind::Name || k==Kind::Role || k==Kind::Appearance; }
bool Generator(Kind k) { return Identity(k) || k==Kind::Advance || k==Kind::Regenerate; }
struct Choice {
    uint64_t value; std::string label; bool selected = false;
    std::string preview;
    engine::Look look{};
    std::vector<Ptr> pickers;
    uint64_t ticket=0;
    engine::Advancement advance{};
    const char* disabled=nullptr;
    std::shared_ptr<const engine::GenerationPlan> generation;
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
    engine::TraitRules traitRules;
    recipe::Catalog recipeCatalog;
    std::vector<Field> fields;
    bool supported=false;
    bool mayGenerate=false, mayAdvance=false;
    const char* traitSaveBlock=nullptr;
    uint64_t baseline=0;
    std::string status;
};
struct Draft {
    bool fromTemplate=false;
    std::set<Ptr> importedHints;
    std::map<Kind,std::pair<uint64_t,Choice>> edits;
    bool dirty() const { return !edits.empty(); }
    bool permits(Kind k,uint64_t field) const {
        if (!dirty()) return true;
        if (RecipeField(k)) return RecipeField(edits.begin()->first);
        if (Identity(k)) return Identity(edits.begin()->first);
        return edits.size()==1 && edits.begin()->second.first==field;
    }
    bool missing(Kind k) const {
        return edits.count(Kind::Role) && (k==Kind::Name || k==Kind::Appearance) && !edits.count(k);
    }
    bool ready() const {
        if (dirty() && RecipeField(edits.begin()->first)) return edits.contains(Kind::Regenerate);
        return dirty() && !missing(Kind::Name) && !missing(Kind::Appearance);
    }
    void stage(const Field& f,const Choice& c) {
        if (RecipeField(f.kind) && f.kind!=Kind::Regenerate) edits.erase(Kind::Regenerate);
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
std::atomic<bool> g_loadFailed{false};
std::atomic<uint64_t> g_nextField{1};

int VectorSize(Ptr vector, size_t stride, int limit = 4096) {
    Ptr begin = Read<Ptr>(vector), end = Read<Ptr>(vector + 8);
    if (end < begin || (end - begin) % stride || (end - begin) / stride > static_cast<size_t>(limit)) return 0;
    return static_cast<int>((end - begin) / stride);
}
bool DemoTraitId(std::string_view id) {
    while (!id.empty()) {
        const auto end=id.find('_');
        const auto token=id.substr(0,end);
        if (token.size()==4 && _strnicmp(token.data(),"DEMO",4)==0) return true;
        if (end==std::string_view::npos) break;
        id.remove_prefix(end+1);
    }
    return false;
}
bool UrukTraitDefinition(std::string_view id) {
    return !id.starts_with("Easterling_") && !DemoTraitId(id);
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
        if (name!="Faction/Rewards/Definitions" && name!="Faction/Tribes/Definitions" &&
            name!="Faction/Traits/Picker" && name!="Faction/Traits/Definitions" && name!="Faction/Traits/PickerListRaw" &&
            name!="Interface/Menu/MenuTreeNode/TraitGroup" && name!="Faction/Traits/Marker" && name!="Faction/Roles/Definitions" &&
            name!="Faction/Contracts/Contract" && name!="Inventory/Items") continue;
        const uint32_t rows = Read<uint32_t>(category + 0x28);
        const Ptr table = Read<Ptr>(category + 0x38);
        if (rows > 100000 || !table) continue;
        auto& records = catalog[name];
        for (uint32_t j = 0; j < rows; ++j) {
            const Ptr record = table + j * 0x28;
            if (Read<Ptr>(record + 0x18) != category) continue;
            auto recordName = RecordName(record);
            if (name=="Faction/Traits/Picker" && DemoTraitId(recordName)) continue;
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
uint64_t CellHash(Ptr record, uint32_t hash, uint32_t index, uint8_t expectedType=10, uint64_t fallback=0) {
    if (!record || (Read<uint8_t>(record + 1) & 0x10)) return fallback;
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
uint64_t Cell(Ptr record, const char* name, uint32_t index = 0, uint8_t expectedType = 10, uint64_t fallback = 0) {
    return CellHash(record,FieldHash(name),index,expectedType,fallback);
}
std::vector<Ptr> Links(Ptr record,uint32_t hash) {
    std::vector<Ptr> links;
    if (!record || (Read<uint8_t>(record+1)&0x10)) return links;
    for (unsigned i=0;i<Read<uint8_t>(record);++i) {
        Ptr f=Read<Ptr>(record+8)+16*i;
        if (Read<uint32_t>(f)!=hash || (Read<uint8_t>(f+10)&63)!=10) continue;
        Ptr data=Read<Ptr>(record+16)+8ull*Read<uint16_t>(f+14);
        for (unsigned j=0;j<Read<uint16_t>(f+12);++j)
            if (auto p=Read<Ptr>(data+8*j)) links.push_back(p);
    }
    return links;
}
using LookupText=const wchar_t* (*)(const char*);
const wchar_t* GameText(const char* key) {
    const Ptr address=game::FromRVA(game::kLocalize);
    if (Read<uint64_t>(address)!=0xc9158b4c28ec8348ULL) return nullptr;
    return reinterpret_cast<LookupText>(address)(key);
}
bool CopyLabel(const char* key,LookupText lookup,char (&label)[1024]) {
    __try {
        const wchar_t* value=lookup(key);
        return value && *value && wcsncmp(value,L"[ERROR:",7)!=0 &&
            WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,label,sizeof label,nullptr,nullptr)>0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
std::string ReadableId(const std::string& id) {
    std::string out;
    for (size_t i=0;i<id.size();++i) {
        char c=id[i];
        if (c=='_') {if (!out.empty() && out.back()!=' ') out+=' ';continue;}
        if (i && id[i-1]>='a' && id[i-1]<='z' && c>='A' && c<='Z') out+=' ';
        out+=c;
    }
    return out;
}
std::string TraitLabel(Ptr trait,LookupText lookup=GameText) {
    auto id=RecordName(trait);
    if (!UrukTraitDefinition(id)) return {};
    // Type-4 Army display key, distinct from the adjacent description and hint keys.
    auto key=Text(CellHash(trait,0xf51aff36,0,4));
    char label[1024]{};
    if (!key.empty() && CopyLabel(key.c_str(),lookup,label) && key!=label && ("["+key+"]")!=label)
    {
        std::string text=label;
        for (size_t i=0;(i=text.find("{Color_",i))!=std::string::npos;) {
            auto end=text.find('}',i);
            if (end==std::string::npos) break;
            text.erase(i,end-i+1);
        }
        for (size_t i;(i=text.find("</font>"))!=std::string::npos;) text.erase(i,7);
        return text.empty()?ReadableId(id):text;
    }
    return ReadableId(id);
}
std::string PickerLabel(Ptr picker) {
    auto id=RecordName(picker);
    if (DemoTraitId(id)) return {};
    std::vector<std::string> labels;
    bool hasOutput=false;
    for (auto entry:Links(picker,0x6bd57b1d)) for (auto trait:Links(entry,0xbcb47686)) {
        hasOutput=true;
        auto label=TraitLabel(trait);
        if (!label.empty() && std::ranges::find(labels,label)==labels.end()) labels.push_back(std::move(label));
    }
    if (labels.empty()) return hasOutput ? std::string{} : ReadableId(id);
    auto result=labels.front();
    for (size_t i=1;i<labels.size();++i) result+=" / "+labels[i];
    return result;
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

// Class progression uses XP reward tracks, separately from the ordinary trait draw lists.
void LoadClassRewards(recipe::Catalog& db,Ptr reward,const std::vector<Ptr>& owners,bool base) {
    if (owners.empty()) return;
    for (Ptr setup:Links(reward,0xcca3a85c)) for (Ptr track:Links(setup,0x11407c64))
        for (Ptr tier:Links(track,0x31775628)) {
            const float chance=std::bit_cast<float>(uint32_t(CellHash(tier,0x5cd1372c,0,2)));
            if (!(chance>0)) continue;
            for (Ptr list:Links(tier,0x4c13006d)) {
                const auto rewards=Links(list,0x0a535c57);
                for (Ptr item:rewards) for (Ptr group:Links(item,0x5b7f9956)) {
                    const auto picks=Links(group,0x7abf91f5);
                    if (chance>=1 && rewards.size()==1 && picks.size()==1) continue;
                    for (Ptr p:picks) {
                        for (Ptr owner:owners) db.classRewards[owner][track].insert(p);
                        if (!base || DemoTraitId(RecordName(p))) continue;
                        for (Ptr e:Links(p,0x6bd57b1d)) for (Ptr d:Links(e,0xbcb47686))
                            if (db.definitions.contains(d) && UrukTraitDefinition(RecordName(d)))
                                db.categories.try_emplace(d,2);
                    }
                }
            }
        }
}
bool LoadRecipeCatalog(const Catalog& catalog,recipe::Catalog& db,Ptr army) {
    const auto nodes=catalog.find("Interface/Menu/MenuTreeNode/TraitGroup"),
        defs=catalog.find("Faction/Traits/Definitions"), pickers=catalog.find("Faction/Traits/Picker"),
        markers=catalog.find("Faction/Traits/Marker");
    if (nodes==catalog.end() || defs==catalog.end() || pickers==catalog.end() || markers==catalog.end()) return false;
    std::map<Ptr,unsigned> groups;
    if (auto items=catalog.find("Inventory/Items");items!=catalog.end()) {
        for (const auto& [id,name]:items->second) {
            for (Ptr look:Links(CellHash(id,0x25399090,0),0x798691ce)) {
                auto slot=CellHash(look,0x765efa31,0,3,10);
                if (slot<10) db.weaponSlots[id].insert(unsigned(slot));
            }
            if (db.weaponSlots.contains(id)) for (Ptr grip:Links(id,0xdb3cd9ad))
                if (RecordName(grip)=="HandGrip_Both") db.bothHands.insert(id);
        }
        for (const auto& [id,name]:items->second) for (Ptr other:Links(id,0xe512b887))
            if (db.weaponSlots.contains(other)) db.pairedWeapons[id].push_back(other);
    }
    auto weapons=[&](Ptr source) {
        std::vector<Ptr> result;
        for (Ptr item:Links(source,0xa15e0193))
            if (db.weaponSlots.contains(item)) result.push_back(item);
        return result;
    };
    if (auto roles=catalog.find("Faction/Roles/Definitions");roles!=catalog.end())
        for (const auto& [id,name]:roles->second) if (name.starts_with("Role_") && !DemoTraitId(name)) {
            auto grants=Links(id,0x66fc592b);
            if (!grants.empty()) db.roles.emplace(id,recipe::RoleKit{std::move(grants),Links(id,0xe0f81767),weapons(id)});
        }
    for (unsigned i=0;i<recipe::nodes.size();++i) {
        auto row=std::ranges::find(nodes->second,recipe::nodes[i],&Record::name);
        if (row==nodes->second.end()) return false;
        auto links=Links(row->address,0x71a4b48a);
        if (links.empty()) return false;
        for (auto g:links) if (!groups.emplace(g,i).second) return false;
    }
    for (const auto& [id,name]:defs->second) {
        db.definitions.insert(id);
        auto items=weapons(id);
        if (!items.empty()) db.traitWeapons.emplace(id,std::move(items));
        // Keep native records recognizable even when they aren't Uruk editor choices.
        if (!UrukTraitDefinition(name)) continue;
        for (auto g:Links(id,0xc402530b)) if (auto it=groups.find(g);it!=groups.end()) {
            if (RecordName(g)=="UIGroup_Epics") db.epics.insert(id);
            auto [entry,added]=db.categories.emplace(id,it->second);
            if (!added && entry->second!=it->second) return false;
        }
    }
    std::map<std::string,Ptr> hints;
    for (const auto& [id,name]:defs->second)
        if (name.starts_with("Resistant_") && db.categories.contains(id) && db.categories.at(id)==7)
            hints.emplace(name.substr(sizeof("Resistant_")-1),id);
    for (const auto& [id,name]:defs->second)
        if (name.starts_with("Immune_") && db.categories.contains(id) && db.categories.at(id)==0)
            if (auto it=hints.find(name.substr(sizeof("Immune_")-1));it!=hints.end()) db.resistanceHint[id]=it->second;
    auto raw=catalog.find("Faction/Traits/PickerListRaw");
    for (const auto& [id,name]:markers->second) {
        auto bind=[&](const std::string& suffix){
            auto base=std::ranges::find(pickers->second,"Picker_BaseStrength_"+suffix,&Record::name);
            if (base!=pickers->second.end()) {
                db.classOnlyPickers[base->address]=id; db.baseGrants[id]=base->address;
                db.managedPickers.insert(base->address);
            }
        };
        if (name.starts_with("ClassMarker_")) {
            db.combatClasses.insert(id);bind(name.substr(sizeof("ClassMarker_")-1));
        }
        if (name.starts_with("Marker_Specialization_")) db.allSpecializations.insert(id);
        if (auto rarity=CellHash(id,0xd76e357b,0)) db.rarityRecords[id]=rarity;
        if (name.starts_with("Marker_Specialization_") && name!="Marker_Specialization_Necromancer") {
            db.classes.insert(id);
            auto suffix=name.substr(sizeof("Marker_Specialization_")-1);
            bind(suffix);
            if (raw!=catalog.end()) {
                auto champions=std::ranges::find(raw->second,"PickerListRaw_Champion"+suffix,&Record::name);
                if (champions!=raw->second.end()) for (auto picker:Links(champions->address,0x7abf91f5)) {
                    db.classOnlyPickers[picker]=id;
                }
            }
            auto block=std::ranges::find(markers->second,"RoleMarker_SpecializationNever_"+suffix,&Record::name);
            if (block!=markers->second.end()) db.neverClass[id]=block->address;
        }
        if (name=="Marker_OrcRarity_Common" || name=="Marker_OrcRarity_Epic" || name=="Marker_OrcRarity_Legendary") {
            db.rarities.insert(id);
            db.epicCaps[id]=name.ends_with("Common")?0:name.ends_with("Epic")?1:2;
        }
    }
    // Only ordinary captain trees present in this faction's class setup.
    const std::set<std::string> captainTrees{"AI_Infantry","AI_Berserker","AI_Defender","AI_Spearman","AI_Archer","AI_Olog_Infantry"};
    for (auto map:Links(CellHash(army,0x5d67200b,0),0x31775628)) for (auto tree:Links(map,0x5210ee19)) {
        if (!captainTrees.contains(RecordName(tree))) continue;
        for (auto marker:Links(tree,0xe0f81767)) if (db.combatClasses.contains(marker) && db.baseGrants.contains(marker)) {
            db.trees[marker]=tree; db.types[marker]=CellHash(tree,0xd990f5e3,0);
        }
    }
    if (auto it=catalog.find("Faction/Tribes/Definitions");it!=catalog.end()) {
        const std::set<std::string> standard{"Dark","Feral","Machine","Marauder","Outlaw","Slaughter","Sorcerous","Terror","Warmonger"};
        for (const auto& [id,name]:it->second) {
            auto grants=Links(id,0x66fc592b);
            db.managedPickers.insert(grants.begin(),grants.end());
            if (name.starts_with("TribeDef_") && standard.contains(name.substr(9)) && engine::TribeAvailable(id))
                db.tribes[id]=std::move(grants);
        }
    }
    if (auto it=catalog.find("Faction/Rewards/Definitions");it!=catalog.end()) for (const auto& [id,name]:it->second) {
        if (name.starts_with("BaseClass_")) {
            std::vector<Ptr> owners;
            for (Ptr predicate:Links(id,0xdd4ea5f7)) for (Ptr type:Links(predicate,0x014ba459))
                for (auto [marker,baseType]:db.types) if (baseType==type) owners.push_back(marker);
            LoadClassRewards(db,id,owners,true);
        }
        if (!name.starts_with("AdvancedClass_")) continue;
        LoadClassRewards(db,id,Links(id,0x53373e4d),false);
        for (auto spec:Links(id,0x53373e4d)) if (db.classes.contains(spec)) {
            auto& kit=db.kits[spec].emplace_back();
            kit.drawLists=Links(id,0xe4d587ea);
            auto gather=[&](auto& into,Ptr record,unsigned hash){
                for (auto v:Links(record,hash)) into.insert(v);
            };
            for (auto predicate:Links(id,0xdd4ea5f7)) gather(kit.allowedTypes,predicate,0x014ba459);
            for (auto predicate:Links(id,0xf15ea6b9)) gather(kit.blockedTypes,predicate,0x014ba459);
            for (auto group:Links(id,0xf273ff9a)) kit.requiredAny.push_back(Links(group,0xff778c99));
            for (auto group:Links(id,0x8148127c)) gather(kit.forbiddenMarkers,group,0xff778c99);
            kit.forbiddenMarkers.erase(spec); // The reward refuses to award the same class twice.
            for (auto group:Links(id,0xa22cf1b9)) gather(kit.removed,group,0x7abf91f5);
            for (auto group:Links(id,0x5b7f9956)) {
                auto grants=Links(group,0x7abf91f5);
                if (!grants.empty()) kit.grants.push_back(std::move(grants));
            }
        }
    }
    // Some class-restricted combat abilities have no Army-screen UI group.
    // ScopeClassDraws still has to find an optional source for the selected kit.
    for (const auto& [id,name]:pickers->second) {
        if (!std::ranges::any_of(Links(id,0x4cf9fd36),[&](Ptr m){return db.combatClasses.contains(m);})) continue;
        for (Ptr e:Links(id,0x6bd57b1d)) for (Ptr d:Links(e,0xbcb47686))
            if (db.definitions.contains(d) && UrukTraitDefinition(RecordName(d)) && CellHash(d,0xa15e0193,0))
                db.categories.try_emplace(d,2);
    }
    for (const auto& [id,name]:pickers->second) {
        recipe::Picker route{id,Links(id,0xb2e40fc1),Links(id,0x4cf9fd36),Links(id,0x0fedbe24)};
        if (auto binding=db.classOnlyPickers.find(id);binding!=db.classOnlyPickers.end()) route.required.push_back(binding->second);
        db.routes.emplace(id,route);
        std::set<Ptr> outputs;
        bool hiddenOutput=false;
        for (auto e:Links(id,0x6bd57b1d)) {
            auto traits=Links(e,0xbcb47686);
            auto requiresAny=Links(e,0xb05e2ca4);
            recipe::indexWeaknessGate(db,e,traits,requiresAny);
            for (auto dep:requiresAny) if (!db.categories.contains(dep) && db.definitions.contains(dep) &&
                UrukTraitDefinition(RecordName(dep))) db.hiddenDependencies[id].insert(dep);
            for (auto trait:traits) {
                if (db.categories.contains(trait)) outputs.insert(trait);
                else {
                    hiddenOutput=true;
                    if (db.definitions.contains(trait) && UrukTraitDefinition(RecordName(trait)))
                        db.hiddenProducers[trait].insert(id);
                }
            }
        }
        if (!hiddenOutput && !outputs.empty()) db.optionalOnly.insert(id);
        for (auto trait:outputs) db.producers[trait].push_back(route);
        for (auto trait:outputs) if (db.managedPickers.contains(id))
            db.managedTraits.insert(trait);
    }
    return !db.producers.empty() && !db.classes.empty() && db.rarities.size()==3;
}
void RecipeFields(Snapshot& snap,const Catalog& catalog) {
    recipe::Catalog db;
    if (!LoadRecipeCatalog(catalog,db,snap.selected.army)) return;
    auto& base=Add(snap,"Class","Select a class",Kind::Base);
    for (auto [id,tree]:db.trees) base.choices.push_back({id,RecordName(id).substr(sizeof("ClassMarker_")-1)});
    std::ranges::sort(base.choices,{},&Choice::label);
    auto& spec=Add(snap,"Advanced Class","Select a class",Kind::Class);
    for (auto id:db.classes) {
        auto name=RecordName(id).substr(sizeof("Marker_Specialization_")-1);
        if (name=="CombatMaster") name="Slayer";
        if (name=="BeastMaster") name="Beastmaster";
        spec.choices.push_back({id,name});
    }
    std::ranges::sort(spec.choices,{},&Choice::label);
    auto& tribe=Add(snap,"Tribe","Select a tribe",Kind::Tribe);
    for (const auto& [id,grants]:db.tribes) {
        auto label=RecordName(id).substr(9);
        tribe.choices.push_back({id,label=="Sorcerous"?"Mystic":label});
    }
    std::ranges::sort(tribe.choices,{},&Choice::label);
    auto& rarity=Add(snap,"Rarity","Select a rarity",Kind::Rarity);
    for (const auto& r:catalog.at("Faction/Traits/Marker"))
        if (db.rarities.contains(r.address)) rarity.choices.push_back({r.address,r.name.substr(sizeof("Marker_OrcRarity_")-1)});
    for (unsigned i=0;i<recipe::nodes.size();++i) {
        auto& field=Add(snap,recipe::labels[i],"0 selected",Kind(int(Kind::Immunities)+i));
        for (const auto& r:catalog.at("Faction/Traits/Definitions"))
            if (db.producers.contains(r.address) && !db.managedTraits.contains(r.address) && db.categories.at(r.address)==i)
                field.choices.push_back({r.address,TraitLabel(r.address)});
        std::stable_sort(field.choices.begin(),field.choices.end(),[](const Choice& a,const Choice& b){return a.label<b.label;});
    }
    Add(snap,"Replacement","Generate a captain",Kind::Regenerate);
    snap.recipeCatalog=std::move(db);
}
recipe::Recipe BuildRecipe(const Draft& draft) {
    recipe::Recipe r;
    for (const auto& [kind,edit]:draft.edits) {
        if (kind==Kind::Class) r.specialization=edit.second.value;
        else if (kind==Kind::Base) r.base=edit.second.value;
        else if (kind==Kind::Tribe) r.tribe=edit.second.value;
        else if (kind==Kind::Rarity) r.rarity=edit.second.value;
        else if (Bucket(kind)) r.traits[int(kind)-int(Kind::Immunities)]=edit.second.pickers;
    }
    return r;
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
    RecipeFields(snapshot,catalog);
    const Ptr member = selected->member;
    const uint64_t flags = Read<uint64_t>(member + 0x508);
    const bool spawned = Read<Ptr>(member + 0x48) != 0;
    const bool writable = snapshot.supported && !spawned && !(flags & ((1ull<<37)|4)) && !Read<Ptr>(member+0x190);
    snapshot.mayGenerate=writable;
    snapshot.traitSaveBlock=!writable ? "This captain is not writable." :
        flags&(1ull<<38) ? "The engine has locked this captain's traits." :
        VectorSize(member+0x1c8,8) ? "Replacing traits with recorded reward history is not supported." : nullptr;
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
    auto& traits=Add(snapshot,"Trait list","0 selected",Kind::Traits);
    if (snapshot.supported && engine::ReadTraitRules(member,snapshot.traitRules)) {
        for (const auto& [p,groups]:snapshot.traitRules.groups) if (InCatalog(catalog,"Faction/Traits/Picker",p)) {
            auto label=PickerLabel(p);
            if (!label.empty()) traits.choices.push_back({p,std::move(label)});
        }
        std::sort(traits.choices.begin(),traits.choices.end(),[](const Choice& a,const Choice& b){return a.label<b.label;});
    } else {
        snapshot.traitRules={};
        snapshot.traitSaveBlock="Native picker rules unavailable.";
    }
    Add(snapshot,"Level-up reward","Advance one level",Kind::Advance);

    auto& level = Add(snapshot, "Level", std::to_string(Read<uint32_t>(member + 0x4b8) + 1), Kind::Level);
    level.expected=Read<unsigned>(member+0x4b8);
    const int cap=writable?engine::LevelCap(member):-1;
    snapshot.mayAdvance=int(level.expected)<cap && !(flags&(1ull<<38)) && !Read<Ptr>(member+0x4f0);
    if (writable && cap>=0) for (int i=0;i<85;++i) {
        Choice c{unsigned(i),std::to_string(i+1),unsigned(i)==level.expected};
        if (i>cap) c.disabled="Above this captain's native level cap";
        level.choices.push_back(std::move(c));
    }
    static const char* weaponSlots[] = {"Melee weapon", "Secondary weapon", "Ranged weapon", "Ranged auxiliary", "Weapon slot 4", "Weapon slot 5", "Utility weapon", "Tribe weapon", "Weapon slot 8", "Offhand weapon"};
    for (uint32_t i = 0; i < 10; ++i) {
        if (equipment[i].empty()) continue;
        const Ptr instance = Read<Ptr>(member + 0x50 + i * 8);
        const Ptr item = Read<Ptr>(instance + 0x38);
        auto& field = Add(snapshot, i < 10 ? weaponSlots[i] : "Armor item " + std::to_string(i - 10), RecordName(item), Kind::Equipment);
        field.expected = item;
        field.slot = i;
        field.category = "Inventory/Items";
        field.choices=std::move(equipment[i]);
        for (auto& c:field.choices) c.selected=c.value==item;
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
                !engine::EquipmentGrantReady()) return false;
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
    if (k==Kind::Advance) {
        if (!engine::RollAdvancement(m,c.advance)) return false;
        c.label="Level "+std::to_string(c.advance.level+1)+": "+RecordName(c.advance.reward);
        c.preview=c.label+"\nReward list: "+RecordName(c.advance.list)+"\nThe game rolls the final outcome on Save:";
        for (bool remove:{false,true}) {
            Ptr possible[128]{};
            int n=engine::RewardPickers(m,c.advance.reward,remove,possible);
            for (int i=0;i<n;++i) {
                auto label=PickerLabel(possible[i]);
                if (!label.empty()) c.preview+=(remove?"\nMay remove: ":"\nMay gain: ")+label;
            }
        }
        c.value=c.ticket; return true;
    }
    return false;
}
bool SaveTraitsImpl(Ptr m,const std::vector<Ptr>& wanted) {
    if (VectorSize(m+0x1c8,8) || Read<uint64_t>(m+0x508)&(1ull<<38)) return false;
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
enum class Op { Refresh, Generate, Save, Template, RetuneTemplate };
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
bool MemberRecords(Ptr vector,std::vector<Ptr>& out) {
    const Ptr begin=Read<Ptr>(vector),end=Read<Ptr>(vector+8);
    if (end<begin || (end-begin)%8 || (end-begin)/8>4096 || (!begin && end)) return false;
    for (Ptr at=begin;at<end;at+=8) {
        Ptr record=Read<Ptr>(at);
        if (!record) return false;
        out.push_back(record);
    }
    return true;
}
recipe::Recipe MemberKit(Ptr m,const recipe::Catalog& db) {
    recipe::Recipe r;
    std::vector<Ptr> markers;
    if (!MemberRecords(m+0x1b0,markers)) return r;
    for (auto [id,tree]:db.trees) if (tree==Read<Ptr>(m+0x188)) r.base=id;
    r.tribe=Read<Ptr>(m+0x328);
    for (auto id:markers) {
        if (db.classes.contains(id)) r.specialization=id;
        if (db.rarities.contains(id)) r.rarity=id;
    }
    if (!r.rarity) for (auto [id,record]:db.rarityRecords)
        if (db.rarities.contains(id) && record==Read<Ptr>(m+0x390)) r.rarity=id;
    return r;
}
bool StageTemplate(const Snapshot& snap,const recipe::Recipe& r,Draft& draft) {
    Draft next;
    for (const auto& field:snap.fields) {
        if (Selector(field.kind)) {
            Ptr value=field.kind==Kind::Base?r.base:field.kind==Kind::Class?r.specialization:
                      field.kind==Kind::Tribe?r.tribe:r.rarity;
            auto it=std::ranges::find(field.choices,value,&Choice::value);
            if (it==field.choices.end()) return false;
            next.stage(field,*it);
        } else if (Bucket(field.kind)) {
            const auto& picks=r.traits[int(field.kind)-int(Kind::Immunities)];
            if (!picks.empty()) {
                for (Ptr id:picks) if (std::ranges::find(field.choices,id,&Choice::value)==field.choices.end()) return false;
                Choice c{0,std::to_string(picks.size())+" selected"};c.pickers=picks;
                next.stage(field,c);
            }
        }
    }
    if (BuildRecipe(next)!=r || !r.complete()) return false;
    next.fromTemplate=true;
    next.importedHints.insert(r.traits[7].begin(),r.traits[7].end());
    draft=std::move(next);return true;
}
bool TemplateFromMember(Ptr m,Snapshot& snap,Draft& draft,const Draft* retune) {
    auto db=snap.recipeCatalog;
    auto r=retune?BuildRecipe(*retune):MemberKit(m,db);
    if (!r.complete() || !db.tribes.contains(r.tribe) || !engine::ScopeClassDraws(db,r,snap.selected.army)) return false;
    std::vector<Ptr> active;
    if (!MemberRecords(m+0x198,active)) return false;
    auto fixed=recipe::coreInputs(db,r);
    const Ptr slots=Read<Ptr>(m+0x1e0);
    std::set<Ptr> automatic=db.managedTraits;
    for (int i=0;i<VectorSize(m+0x1e0,32);++i) {
        Ptr picker=Read<Ptr>(slots+32*i);
        if (db.managedPickers.contains(picker) || std::ranges::find(fixed,picker)!=fixed.end())
            automatic.insert(Read<Ptr>(slots+32*i+8));
    }
    if (!retune) for (auto id:active) if (auto bucket=db.categories.find(id);bucket!=db.categories.end() &&
        db.producers.contains(id) && !automatic.contains(id)) r.traits[bucket->second].push_back(id);
    const Ptr rank=CellHash(Read<Ptr>(m+0x268),0x40c579dc,0);
    auto valid=[&]{engine::GenerationPlan plan;std::string error;return engine::PlanGeneration(m,rank,db,r,plan,error,snap.session);};
    size_t skipped=0;
    if (!valid()) {
        auto pending=r.traits;r.traits={};
        if (!valid()) return false;
        bool progress;
        do {
            progress=false;
            for (size_t i=0;i<pending.size();++i) std::erase_if(pending[i],[&](Ptr id){
                r.traits[i].push_back(id);
                if (valid()) {progress=true;return true;}
                r.traits[i].pop_back();return false;
            });
        } while (progress);
        for (auto& row:pending) skipped+=row.size();
    }
    snap.status="Template settings staged. No captain changed.";
    if (skipped) snap.status+=" Skipped "+std::to_string(skipped)+" incompatible choices.";
    if (!StageTemplate(snap,r,draft)) return false;
    if (retune) std::erase_if(draft.importedHints,[&](Ptr id){return !retune->importedHints.contains(id);});
    return true;
}
std::string KitText(const recipe::Recipe& r) {
    return "base="+RecordName(r.base)+", advanced="+RecordName(r.specialization)+", tribe="+RecordName(r.tribe)+
        ", rarity="+RecordName(r.rarity);
}
bool GenerationReport(const Task& task,const Catalog& catalog,Ptr m,const std::string& outcome) {
    const auto wanted=BuildRecipe(task.draft);
    std::string report="Seat: "+RecordName(task.selected.army)+" / "+std::to_string(task.selected.index)+
        "\r\nResult: "+outcome+"\r\nPicked kit: "+KitText(wanted)+"\r\n";
    recipe::Catalog db;
    bool catalogOK=m && LoadRecipeCatalog(catalog,db,task.selected.army);
    auto action=task.draft.edits.find(Kind::Regenerate);
    if (action!=task.draft.edits.end()) if (auto plan=action->second.second.generation) {
        report+="Role: "+(plan->role?RecordName(plan->role):std::string("automatic"))+"\r\n";
        for (auto [slot,item]:plan->weapons) {
            Ptr instance=m?Read<Ptr>(m+0x50+8*slot):0;
            report+="Weapon slot "+std::to_string(slot)+": reserved "+(item?RecordName(item):"empty")+
                ", observed "+(!m?"unknown":instance?RecordName(Read<Ptr>(instance+0x38)):"empty")+"\r\n";
        }
    }
    if (catalogOK) report+="Observed kit: "+KitText(MemberKit(m,db))+", level="+std::to_string(Read<unsigned>(m+0x4b8)+1)+"\r\n";
    std::vector<Ptr> active;
    bool observed=m && MemberRecords(m+0x198,active);
    auto line=[&](Ptr id){
        std::string label=TraitLabel(id)+" ["+RecordName(id)+"]";
        if (!observed) return label+" - state unavailable";
        if (std::ranges::find(active,id)==active.end()) return label+" - NOT ACTIVE";
        if (Links(id,0xc402530b).empty()) return label+" - active; hidden (no Army group)";
        auto visible=engine::ArmyTraitVisible(m,id);
        return label+(visible?(*visible?" - active; Army display eligible":" - active; hidden by Army display rules"):
            " - active; visibility unknown");
    };
    std::set<Ptr> requested;
    for (size_t i=0;i<wanted.traits.size();++i) {
        report+=std::string(recipe::labels[i])+": "+std::to_string(wanted.traits[i].size())+" picked\r\n";
        for (Ptr id:wanted.traits[i]) {requested.insert(id);report+="  "+line(id)+"\r\n";}
    }
    report+="Other active traits (not picked):\r\n";
    if (observed) {for (Ptr id:active) if (!requested.contains(id)) report+="  "+line(id)+"\r\n";}
    else report+="  State unavailable\r\n";
    return WriteGenerationLog(report.c_str());
}
bool Commit(Ptr m,const Task& task,const Catalog& catalog,std::string& status) {
    const auto& d=task.draft;
    if (!d.ready() || !Writable(m)) return false;
    auto get=[&](Kind k)->const Choice* {auto i=d.edits.find(k); return i==d.edits.end()?nullptr:&i->second.second;};
    if (RecipeField(d.edits.begin()->first)) {
        auto c=get(Kind::Regenerate);
        recipe::Catalog db;
        const auto r=BuildRecipe(d);
        if (!c || !c->generation || !LoadRecipeCatalog(catalog,db,task.selected.army) ||
            !engine::ScopeClassDraws(db,r,task.selected.army)) return false;
        auto result=engine::ReplaceCaptain(m,*c->generation,db,r,SaveTraits);
        if (result==engine::ReplaceResult::ChangedUnexpectedly)
            status="Replacement ran, but the engine state did not match the preview. Inspect the captain before continuing.";
        else if (result==engine::ReplaceResult::Replaced)
            status="Replaced the selected captain at his original level and Army seat.";
        return result==engine::ReplaceResult::Replaced;
    }
    if (auto l=get(Kind::Level))
        return int(l->value)<=engine::LevelCap(m) && ApplyNative(m,Kind::Level,0,0,l->value);
    if (auto w=get(Kind::Equipment)) {
        Ptr kit[512]{},look=0; int n=engine::KitItems(m,kit); unsigned slot=0;
        return std::find(kit,kit+n,w->value)!=kit+n && engine::WeaponLook(m,w->value,slot,look) && slot==task.slot
            && look==w->look.base && ApplyNative(m,Kind::Equipment,task.expected,slot,w->value);
    }
    if (auto t=get(Kind::Traits)) {
        for (auto p:t->pickers) if (!InCatalog(catalog,"Faction/Traits/Picker",p)) return false;
        engine::TraitRules now;
        if (!engine::ReadTraitRules(m,now) || !now.permits(t->pickers)) {
            status="The captain's picker rules changed. No traits saved."; return false;
        }
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
bool Current();
void RunTask(Task* task) {
    if (!g_ui || !task->session || g_ui->SaveSession()!=task->session) return;
    try {
        Snapshot snapshot;
        snapshot.session=task->session; snapshot.selected=task->selected;
        snapshot.targets=Targets(); snapshot.supported=engine::Supported();
        if (task->op==Op::Refresh && !task->selected.army) {
            Ptr army; unsigned index;
            if (engine::StageSelection(army,index)) for (const auto& t:snapshot.targets)
                if (t.key.army==army && t.key.index==index) { snapshot.selected=t.key; break; }
        }
        auto it=std::find_if(snapshot.targets.begin(),snapshot.targets.end(),[&](const auto& t){return t.key==snapshot.selected;});
        Ptr m=it==snapshot.targets.end()?0:it->member;
        const Catalog catalog=LoadCatalog();
        Draft imported;
        if (task->op!=Op::Refresh && (!m || Stamp(m)!=task->baseline)) {
            snapshot.status="Captain changed. Draft discarded; no edits saved.";
        } else if (task->op==Op::Generate) {
            Choice candidate;
            bool ok=false;
            if (task->kind==Kind::Regenerate) {
                auto plan=std::make_shared<engine::GenerationPlan>();
                recipe::Catalog db;
                const auto r=BuildRecipe(task->draft);
                ok=Writable(m) && LoadRecipeCatalog(catalog,db,task->selected.army) &&
                    engine::ScopeClassDraws(db,r,task->selected.army) &&
                    engine::PlanGeneration(m,CellHash(Read<Ptr>(m+0x268),0x40c579dc,0),db,r,*plan,snapshot.status,task->session);
                if (ok) {
                    candidate.ticket=g_nextField.fetch_add(1);
                    candidate.label="Generation settings";
                    candidate.preview=it->label+"\nLevel "+std::to_string(plan->level+1);
                    if (plan->role) candidate.preview+="\nRequired role: "+RecordName(plan->role);
                    for (auto [slot,item]:plan->weapons)
                        candidate.preview+="\nWeapon slot "+std::to_string(slot)+": "+(item?RecordName(item):"reserved empty");
                    for (const auto& [kind,edit]:task->draft.edits) if (Selector(kind))
                        candidate.preview+="\n"+edit.second.label;
                    for (const auto& [kind,edit]:task->draft.edits) if (Bucket(kind)) for (auto id:edit.second.pickers)
                        candidate.preview+="\n"+TraitLabel(id);
                    candidate.generation=std::move(plan);
                }
            } else {
                ok=Roll(m,task->kind,DraftRole(m,task->draft),catalog,candidate);
                if (ok) AppendOrderReport(candidate,task->kind,m,task->draft,catalog);
            }
            std::lock_guard lock(g_mutex);
            if (task->request!=g_generationRequest || g_snapshot.session!=g_ui->SaveSession() || g_snapshot.session!=task->session) return;
            for (auto& f:g_snapshot.fields) if (f.id==task->field) {
                f.choices.clear();
                if (ok) {
                    f.choices.push_back(std::move(candidate));
                }
                g_snapshot.status=ok?"Apply to draft or cancel.":snapshot.status.empty()?"No compatible profile. Try generating again.":snapshot.status;
            }
            return;
        } else if (task->op==Op::Save) {
            bool saved=Commit(m,*task,catalog,snapshot.status);
            if (m && task->draft.edits.contains(Kind::Regenerate)) {
                snapshot.targets=Targets();
                m=0;
                for (const auto& target:snapshot.targets) if (target.key.army==task->selected.army && target.key.index==task->selected.index) {
                    m=target.member; snapshot.selected=target.key; break;
                }
            }
            if (snapshot.status.empty()) snapshot.status=saved?"Saved to the captain.":"Save rejected by the engine. Captain state refreshed.";
            Log(snapshot.status.c_str());
        }
        if (task->op==Op::Save && task->draft.edits.contains(Kind::Regenerate) &&
            !GenerationReport(*task,catalog,m,snapshot.status)) snapshot.status+=" Could not write generation log.";
        Capture(snapshot,catalog);
        if (m && Stamp(m)==task->baseline && (task->op==Op::Template || task->op==Op::RetuneTemplate) &&
            !TemplateFromMember(m,snapshot,imported,task->op==Op::RetuneTemplate?&task->draft:nullptr)) {
            std::lock_guard lock(g_mutex);
            g_snapshot.status="Cannot import this kit. Previous settings kept.";
            return;
        }
        snapshot.baseline=m?Stamp(m):0;
        if (!snapshot.status.empty()) Add(snapshot,"Last operation",snapshot.status);
        if (g_ui->SaveSession()==task->session) {
            std::lock_guard lock(g_mutex);
            g_snapshot=std::move(snapshot); g_draft=std::move(imported);
        }
    } catch (...) { g_loadFailed=true; Log("Uruk editor operation failed"); }
}
void GuardedRun(Task* task) {
    __try { RunTask(task); }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {
        g_loadFailed=true;
        Log("Uruk editor native access failed; refresh can be retried");
    }
}
void Run(void* context) {
    std::unique_ptr<Task> task(static_cast<Task*>(context));
    GuardedRun(task.get());
    g_pending=false;
}
bool Queue(std::unique_ptr<Task> task) {
    if (g_pending.exchange(true)) return false;
    g_loadFailed=false;
    if (!g_ui->QueueGameTask(Run, task.get())) { g_loadFailed=true; g_pending.store(false); return false; }
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
    if (f.kind==Kind::Traits) return true;
    if (Selector(f.kind)) return g_snapshot.mayGenerate && !f.choices.empty() &&
        (f.kind!=Kind::Class || g_draft.edits.contains(Kind::Base));
    if (Bucket(f.kind)) return g_snapshot.mayGenerate && BuildRecipe(g_draft).complete();
    if (f.kind==Kind::Regenerate) return g_snapshot.mayGenerate && BuildRecipe(g_draft).complete();
    if (Generator(f.kind)) return g_snapshot.mayGenerate &&
        (f.kind!=Kind::Advance || g_snapshot.mayAdvance);
    return !f.choices.empty();
}
int recipePage;
bool PageField(void* page,const Field& f) {
    if (f.kind==Kind::Regenerate && g_ui->abiVersion>=14 && g_ui->SetEditorFooterGenerator) return false;
    return f.kind!=Kind::Traits && (f.kind==Kind::Target || f.kind==Kind::View || (RecipeField(f.kind)==(page==&recipePage)));
}
int Count(void* page) { std::lock_guard lock(g_mutex); return Current() ? int(std::ranges::count_if(g_snapshot.fields,[&](const Field& f){return PageField(page,f);})) : 1; }
bool ReadField(void* page, int index, HagUI_Property* property) {
    if (!property || index < 0) return false;
    std::lock_guard lock(g_mutex);
    if (!Current()) {
        if (index) return false;
        *property = {};
        strcpy_s(property->label, "State");
        strcpy_s(property->value, g_pending.load() ? "Loading captain state..." : g_loadFailed.load() ?
            "Captain refresh failed. Press Refresh to retry." : "Refresh after loading a save.");
        return true;
    }
    auto found=std::ranges::find_if(g_snapshot.fields,[&](const Field& f){return PageField(page,f) && index--==0;});
    if (found==g_snapshot.fields.end()) return false;
    const auto& field = *found;
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
        field.kind==Kind::Class && !g_draft.edits.contains(Kind::Base) ? "Select a base class first." :
        field.kind==Kind::Regenerate || Bucket(field.kind) ? "Select class, advanced class, tribe and rarity to prepare the automatic kit." :
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
bool FitsSelector(const Field& f,const Choice& c) {
    const auto& db=g_snapshot.recipeCatalog;
    if (f.kind==Kind::Base) return db.trees.contains(c.value);
    if (f.kind==Kind::Tribe) return db.tribes.contains(c.value);
    if (f.kind==Kind::Rarity) return db.rarities.contains(c.value);
    auto base=g_draft.edits.find(Kind::Base);
    return f.kind==Kind::Class && base!=g_draft.edits.end() &&
        db.classes.contains(c.value) && recipe::fitsClass(db,base->second.second.value,c.value);
}
bool ReadChoice(void*, uint64_t id, int index, HagUI_Choice* out) {
    std::lock_guard lock(g_mutex);
    auto field = Find(id);
    if (!out || !field || index < 0 || index >= static_cast<int>(field->choices.size())) return false;
    *out = {};
    out->id = field->choices[index].ticket ? field->choices[index].ticket : index + 1;
    out->selected = field->choices[index].selected;
    if (field->kind==Kind::Traits || Bucket(field->kind)) if (auto it=g_draft.edits.find(field->kind);it!=g_draft.edits.end()) {
        const auto& picks=it->second.second.pickers;
        out->selected=std::find(picks.begin(),picks.end(),field->choices[index].value)!=picks.end();
    }
    if (Selector(field->kind) && !FitsSelector(*field,field->choices[index])) return false;
    if (Selector(field->kind))
        if (auto it=g_draft.edits.find(field->kind);it!=g_draft.edits.end())
            out->selected=field->choices[index].value==it->second.second.value;
    std::string label=field->choices[index].label;
    if (field->kind==Kind::Immunities) {
        const auto& hints=g_snapshot.recipeCatalog.resistanceHint;
        auto it=hints.find(field->choices[index].value);
        if (it!=hints.end() && g_draft.importedHints.contains(it->second)) label+=" (replaces imported resistance)";
    }
    strncpy_s(out->label,label.c_str(),_TRUNCATE);
    return true;
}
const Choice* ChoiceById(const Field& f,uint64_t id) {
    if (!id) return nullptr;
    if (Generator(f.kind)) return !f.choices.empty() && f.choices.front().ticket==id?&f.choices.front():nullptr;
    return id<=f.choices.size()?&f.choices[size_t(id-1)]:nullptr;
}
std::unique_ptr<Task> DraftTask(Op op);
bool Apply(void*,uint64_t id,uint64_t choiceId) {
    std::lock_guard lock(g_mutex);
    const Field* f=Find(id);
    if (!f || !Enabled(*f)) return false;
    const Choice* c=ChoiceById(*f,choiceId);
    if (!c || c->disabled) return false;
    if (f->kind==Kind::Traits || Bucket(f->kind)) return false;
    if (Selector(f->kind) && !FitsSelector(*f,*c)) return false;
    if (RecipeField(f->kind) && !Selector(f->kind)) {
        Draft next=g_draft;
        next.stage(*f,*c);
        if (!recipe::resolve(g_snapshot.recipeCatalog,BuildRecipe(next),[](const auto&){return true;})) return false;
    }
    if (f->kind==Kind::Target) {
        if (c->value>=g_snapshot.targets.size()) return false;
        auto t=std::make_unique<Task>();
        t->session=g_snapshot.session; t->selected=g_snapshot.targets[c->value].key;
        return Queue(std::move(t));
    }
    if (Selector(f->kind) && g_draft.fromTemplate) {
        auto t=DraftTask(Op::RetuneTemplate);
        t->draft.stage(*f,*c);
        const auto r=BuildRecipe(t->draft);
        if (f->kind==Kind::Base && !recipe::fitsClass(g_snapshot.recipeCatalog,r.base,r.specialization)) {
            t->draft.edits.erase(Kind::Class);
            g_draft=std::move(t->draft);
            g_snapshot.status="Select an advanced class to check the remaining template choices.";
            return true;
        }
        if (r.complete()) return Queue(std::move(t));
        g_draft=std::move(t->draft);return true;
    }
    g_draft.stage(*f,*c);
    if (Selector(f->kind)) {
        std::erase_if(g_draft.edits,[](const auto& e){return Bucket(e.first);});
        auto spec=g_draft.edits.find(Kind::Class);
        if (f->kind==Kind::Base && spec!=g_draft.edits.end() &&
            !recipe::fitsClass(g_snapshot.recipeCatalog,c->value,spec->second.second.value)) g_draft.edits.erase(spec);
        g_snapshot.status.clear();
    }
    if (RecipeField(f->kind) && f->kind!=Kind::Regenerate)
        for (auto& field:g_snapshot.fields) if (field.kind==Kind::Regenerate) field.choices.clear();
    if (f->kind==Kind::Role) for (auto& other:g_snapshot.fields)
        if (other.kind==Kind::Name || other.kind==Kind::Appearance) other.choices.clear();
    return true;
}
void State(void* page,uint64_t id,uint64_t choice,HagUI_PopupState* out) {
    *out={};
    std::lock_guard lock(g_mutex);
    out->busy=g_pending.load();
    if (!Current()) return;
    out->dirty=g_draft.dirty(); out->busy=g_pending;
    out->canSave=g_draft.ready() && !out->busy;
    bool composing=g_draft.dirty() && RecipeField(g_draft.edits.begin()->first);
    if (g_draft.dirty() && composing!=(page==&recipePage)) out->canSave=false;
    strncpy_s(out->status,composing && !id && !g_draft.ready() && g_snapshot.status.empty()?"Generate and apply a replacement first.":
        !composing && g_draft.dirty()&&!g_draft.ready()?"Regenerate and apply the marked fields.":g_snapshot.status.c_str(),_TRUNCATE);
    if (g_draft.edits.count(Kind::Traits) && g_snapshot.traitSaveBlock) {
        out->canSave=false;
        strncpy_s(out->status,g_snapshot.traitSaveBlock,_TRUNCATE);
    }
    if (auto f=Find(id)) {
        out->generator=Generator(f->kind); out->needsRepair=g_draft.missing(f->kind);
        if (auto c=ChoiceById(*f,choice)) {
            if (Selector(f->kind)) {
                if (!FitsSelector(*f,*c)) {out->unavailable=true; strcpy_s(out->status,"Conflicts with the selected kit.");}
                else if (BuildRecipe(g_draft).complete()) strcpy_s(out->status,g_draft.fromTemplate?
                    "Keeps compatible template choices.":"Applying a selector clears optional picks.");
            }
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
bool CanTakeTemplate() {
    std::lock_guard lock(g_mutex);
    return Current() && !g_pending && g_snapshot.selected.army && g_snapshot.mayGenerate;
}
void TakeTemplate() {
    std::lock_guard lock(g_mutex);
    if (Current() && !g_pending && g_snapshot.selected.army && g_snapshot.mayGenerate)
        Queue(DraftTask(Op::Template));
}
std::uint64_t FooterGenerator(void*) {
    std::lock_guard lock(g_mutex);
    if (Current()) for (auto& f:g_snapshot.fields) if (f.kind==Kind::Regenerate && Enabled(f)) return f.id;
    return 0;
}
void Save(void* page) {
    std::lock_guard lock(g_mutex);
    if (!Current() || !g_draft.ready() || g_pending) return;
    if (RecipeField(g_draft.edits.begin()->first)!=(page==&recipePage)) return;
    if (g_draft.edits.count(Kind::Traits) && g_snapshot.traitSaveBlock) return;
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
bool HandlesSet(void*,uint64_t id) {
    std::lock_guard lock(g_mutex); auto f=Find(id); return f && (f->kind==Kind::Traits || Bucket(f->kind));
}
struct InputCheck {
    Key target;
    uint64_t session{},baseline{},field{};
    recipe::Recipe staged;
    std::set<Ptr> importedHints;
    std::set<Ptr> allowed;
    bool valid=false,ready=false;
};
std::shared_ptr<InputCheck> inputCheck;
// Evaluate native picker records on the game thread, without running the captain builder.
void CheckInputs(void* arg) {
    std::unique_ptr<std::shared_ptr<InputCheck>> job(static_cast<std::shared_ptr<InputCheck>*>(arg));
    const auto check=*job;
    recipe::Catalog db; std::vector<Ptr> choices; size_t bucket;
    {
        std::lock_guard lock(g_mutex);
        auto f=Find(check->field);
        if (!f || inputCheck!=check || g_snapshot.session!=check->session || !Bucket(f->kind)) return;
        db=g_snapshot.recipeCatalog;
        bucket=int(f->kind)-int(Kind::Immunities);
        for (const auto& c:f->choices) choices.push_back(c.value);
    }
    std::set<Ptr> allowed;
    bool valid=false;
    for (const auto& t:Targets()) if (t.key==check->target && Stamp(t.member)==check->baseline) {
        auto r=check->staged;
        if (!engine::ScopeClassDraws(db,r,t.key.army)) break;
        auto rank=CellHash(Read<Ptr>(t.member+0x268),0x40c579dc,0);
        engine::GenerationPlan plan;
        std::string error;
        auto validSelection=[&] {
            auto effective=r;
            if (bucket==0) recipe::upgradeImportedHints(db,effective,check->importedHints);
            return engine::PlanGeneration(t.member,rank,db,effective,plan,error,check->session);
        };
        valid=validSelection();
        for (Ptr id:choices) {
            r.traits[bucket].push_back(id);
            if (validSelection()) allowed.insert(id);
            r.traits[bucket].pop_back();
        }
        break;
    }
    std::lock_guard lock(g_mutex);
    if (inputCheck==check && Current() && g_snapshot.session==check->session) {
        check->ready=true;check->valid=valid;check->allowed=std::move(allowed);
    }
}
bool Compatible(const Field& f,uint64_t candidate,const uint64_t* picks,int n) {
    if (n<0 || n>512 || (n && !picks) || (candidate && candidate>f.choices.size())) return false;
    std::vector<Ptr> set;
    for (int i=0;i<n;++i) {
        if (!picks[i] || picks[i]>f.choices.size()) return false;
        set.push_back(f.choices[picks[i]-1].value);
    }
    if (candidate) set.push_back(f.choices[candidate-1].value);
    if (Bucket(f.kind)) {
        auto r=BuildRecipe(g_draft);
        if (!r.complete()) return false;
        r.traits[int(f.kind)-int(Kind::Immunities)]=std::move(set);
        const auto& db=g_snapshot.recipeCatalog;
        auto effective=r;
        if (f.kind==Kind::Immunities) recipe::upgradeImportedHints(db,effective,g_draft.importedHints);
        auto fixed=recipe::coreInputs(db,r);
        unsigned budget=8192;
        if (!recipe::resolve(db,effective,[](const auto&){return true;},[&](const auto& picked){
            return recipe::fitsCore(db,fixed,picked);
        },&budget)) return false;
        auto& row=r.traits[int(f.kind)-int(Kind::Immunities)];
        if (candidate) row.pop_back();
        auto same=[&]{return inputCheck && inputCheck->field==f.id && inputCheck->session==g_snapshot.session &&
            inputCheck->baseline==g_snapshot.baseline && inputCheck->target==g_snapshot.selected &&
            inputCheck->importedHints==g_draft.importedHints;};
        if (!candidate && same() && inputCheck->ready && !row.empty()) {
            auto last=row.back();row.pop_back();
            if (r==inputCheck->staged && inputCheck->allowed.contains(last)) return true;
            row.push_back(last);
        }
        if (same() && inputCheck->staged==r) return inputCheck->ready &&
            (candidate?inputCheck->allowed.contains(f.choices[candidate-1].value):inputCheck->valid);
        auto check=std::make_shared<InputCheck>();
        check->field=f.id;check->target=g_snapshot.selected;check->baseline=g_snapshot.baseline;
        check->session=g_snapshot.session;check->staged=std::move(r);
        check->importedHints=g_draft.importedHints;
        auto job=std::make_unique<std::shared_ptr<InputCheck>>(check);
        inputCheck=check;
        if (g_ui->QueueGameTask(CheckInputs,job.get())) job.release();
        else inputCheck.reset();
        return false;
    }
    return g_snapshot.traitRules.permits(set);
}
bool SetOption(void*,uint64_t id,uint64_t candidate,const uint64_t* picks,int n) {
    std::lock_guard lock(g_mutex); auto f=Find(id);
    return f && (f->kind==Kind::Traits || Bucket(f->kind)) && Compatible(*f,candidate,picks,n);
}
bool SetApply(void*,uint64_t id,const uint64_t* picks,int n) {
    std::lock_guard lock(g_mutex); auto f=Find(id);
    if (!f || (f->kind!=Kind::Traits && !Bucket(f->kind)) || !Enabled(*f) || !Compatible(*f,0,picks,n)) return false;
    Choice c{0,std::to_string(n)+" selected"};
    for (int i=0;i<n;++i) {
        if (std::find(picks,picks+i,picks[i])!=picks+i) return false;
        c.pickers.push_back(f->choices[picks[i]-1].value);
    }
    g_draft.stage(*f,c);
    if (f->kind==Kind::Hints) g_draft.importedHints.clear(); // the player explicitly accepted this list
    if (f->kind==Kind::Immunities && !g_draft.importedHints.empty()) {
        auto r=BuildRecipe(g_draft);
        auto& hints=r.traits[7];
        const auto count=hints.size();
        recipe::upgradeImportedHints(g_snapshot.recipeCatalog,r,g_draft.importedHints);
        if (count!=hints.size()) {
            auto& hintChoice=g_draft.edits.at(Kind::Hints).second;
            hintChoice.pickers=hints;hintChoice.label=std::to_string(hints.size())+" selected";
            std::erase_if(g_draft.importedHints,[&](Ptr id){return std::ranges::find(hints,id)==hints.end();});
            g_snapshot.status="Replaced "+std::to_string(count-hints.size())+" imported resistance hint(s) with immunity.";
        }
    }
    if (Bucket(f->kind)) for (auto& field:g_snapshot.fields) if (field.kind==Kind::Regenerate) field.choices.clear();
    return true;
}
struct SceneId {
    uint64_t session{},field{},choice{};
    bool operator==(const SceneId&) const = default;
};
SceneId wantedScene{};
bool sceneQueued=false;
int sceneState=0;
std::chrono::steady_clock::time_point lastProbe{};
// Only the game thread touches the lease. Identity, never a pointer alone,
// determines whether a later task is allowed to restore the Army character.
engine::StageLease lease{};
SceneId leasedScene{};
void ClearLease() {
    if (leasedScene.session && leasedScene.session==g_ui->SaveSession()) engine::RestoreStage(lease);
    lease={}; leasedScene={};
}
void RunScene(void*) {
    SceneId id;
    Key target{};
    uint64_t baseline=0;
    Kind kind=Kind::View;
    Choice candidate{};
    Ptr role=0;
    unsigned slot=0;
    try {
        {
            std::lock_guard lock(g_mutex);
            id=wantedScene;
            if (id.session && id.session==g_ui->SaveSession()) if (auto f=Find(id.field)) {
                if (auto c=ChoiceById(*f,id.choice); c && !c->disabled) {
                    target=g_snapshot.selected; baseline=g_snapshot.baseline; kind=f->kind; slot=f->slot;
                    candidate=*c;
                    if (auto r=g_draft.edits.find(Kind::Role); r!=g_draft.edits.end()) role=r->second.second.value;
                }
            }
        }
        bool ready=target.army && leasedScene==id && engine::StageCurrent(lease);
        if (!ready) {
            ClearLease();
            Ptr selectedArmy; unsigned selectedIndex;
            if (target.army && id.session==g_ui->SaveSession() && engine::StageSelection(selectedArmy,selectedIndex)
                    && selectedArmy==target.army && selectedIndex==target.index) {
                for (const auto& t:Targets()) if (t.key==target && Stamp(t.member)==baseline) {
                    if (!role) role=Read<Ptr>(t.member+0x318);
                    ready=engine::PreviewStage(t.member,role,kind==Kind::Appearance?candidate.value:0,
                            candidate.look,kind==Kind::Equipment?candidate.value:0,slot,lease);
                    if (ready) leasedScene=id;
                    break;
                }
            }
        }
        bool stale;
        {
            std::lock_guard lock(g_mutex);
            stale=id!=wantedScene || (id.session && id.session!=g_ui->SaveSession());
            sceneState=stale?0:(ready?2:1);
            sceneQueued=false;
        }
        if (stale) ClearLease();
    } catch (...) {
        ClearLease();
        std::lock_guard lock(g_mutex);
        sceneQueued=false; sceneState=1;
    }
}
int Scene(void*,uint64_t field,uint64_t choice,char (&message)[192]) {
    std::lock_guard lock(g_mutex);
    SceneId id{};
    auto f=field?Find(field):nullptr;
    const bool visual=f && (f->kind==Kind::Appearance || f->kind==Kind::Equipment);
    if (visual && !g_pending) if (auto c=ChoiceById(*f,choice); c && !c->disabled)
        id={g_snapshot.session,field,choice};
    const auto now=std::chrono::steady_clock::now();
    const bool changed=id!=wantedScene;
    if (changed) { wantedScene=id; sceneState=0; }
    if (!sceneQueued && (changed || (id.session && now-lastProbe>std::chrono::milliseconds(sceneState==2?250:1500)))) {
        sceneQueued=g_ui->QueueGameTask(RunScene,nullptr);
        lastProbe=now;
        if (!sceneQueued) { wantedScene={~uint64_t(0),0,0}; return 1; }
    }
    if (!visual) return 0;
    if (!id.choice) strcpy_s(message,f->kind==Kind::Appearance?"Generate an appearance to preview.":"Select a weapon to preview.");
    else if (!sceneState) strcpy_s(message,"Preparing Army model...");
    else if (sceneState!=2) strcpy_s(message,"Open this captain's detail view in the Army screen before opening the editor.");
    return id.choice && sceneState==2?2:1;
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
    const HagUI_SetEditor set{HandlesSet,SetOption,SetApply};
    if (ui->abiVersion>=13 && ui->AddSetEditor) ui->AddSetEditor(page,nullptr,&source,&popup,Scene,&set);
    else if (ui->abiVersion>=12 && ui->AddSceneEditor) ui->AddSceneEditor(page,nullptr,&source,&popup,Scene);
    else ui->AddPopupEditor(page,nullptr,&source,&popup);
    ui->AddSubPageButton(parent, "Uruk Editor", page);
    if (ui->abiVersion>=13 && ui->AddSetEditor) {
        const int regen=ui->RegisterSubPage(parent,"Regenerate Captain");
        if (regen>=0) {
            if (ui->AddHeaderButton) ui->AddHeaderButton(regen,"Take Over Template",TakeTemplate,CanTakeTemplate);
            ui->AddSetEditor(regen,&recipePage,&source,&popup,Scene,&set);
            if (ui->abiVersion>=14 && ui->SetEditorFooterGenerator) ui->SetEditorFooterGenerator(regen,"Generate",FooterGenerator);
            ui->AddSubPageButton(parent,"Regenerate Captain",regen);
        }
    }
}
}
