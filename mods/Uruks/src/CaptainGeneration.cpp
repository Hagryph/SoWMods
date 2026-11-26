#include "ModRuntime.h"
#include "CaptainGeneration.h"
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
enum class Kind { Target,
    Class, Base, Tribe, Rarity, Immunities, Hates, Classes, Bonuses, Mortal, Vulnerable, Dazed, Hints, Regenerate };
bool Selector(Kind k) { return k>=Kind::Class && k<=Kind::Rarity; }
bool Bucket(Kind k) { return k>=Kind::Immunities && k<=Kind::Hints; }
struct Choice {
    uint64_t value{}; std::string label;
    std::vector<Ptr> pickers;
    std::shared_ptr<const engine::GenerationPlan> generation;
};
struct Field {
    uint64_t id = 0;
    Kind kind = Kind::Target;
    std::string label, value;
    std::vector<Choice> choices;
};
struct Snapshot {
    uint64_t session = 0;
    Key selected;
    recipe::Catalog recipeCatalog;
    std::vector<Field> fields;
    std::string status;
};
struct Draft {
    bool fromTemplate=false;
    std::map<Kind,std::pair<uint64_t,Choice>> edits;
    bool ready() const { return edits.contains(Kind::Regenerate); }
    void stage(const Field& f,const Choice& c) {
        if (f.kind!=Kind::Regenerate) edits.erase(Kind::Regenerate);
        edits[f.kind]={f.id,c};
    }
};
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
        if (name!="Faction/RowRanks" && name!="Faction/FactionMemberBuilder" && name!="Faction/Rewards/Definitions" && name!="Faction/Tribes/Definitions" &&
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

Field& Add(Snapshot& snapshot, std::string label, std::string value, Kind kind) {
    snapshot.fields.push_back({});
    auto& field = snapshot.fields.back();
    field.id = g_nextField.fetch_add(1);
    field.kind = kind;
    field.label = std::move(label);
    field.value = std::move(value);
    return field;
}
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
// Interpret only deterministic trait grants/removals. Never dispatch a story,
// inventory-changing or delayed reward just because it also grants a trait.
bool EncounterRecord(Ptr p,std::initializer_list<unsigned> allowed,Ptr defaults=0) {
    if (!p || (Read<uint8_t>(p+1)&16)) return false;
    for (unsigned i=0;i<Read<uint8_t>(p);++i) {
        Ptr f=Read<Ptr>(p+8)+16*i;
        unsigned key=Read<uint32_t>(f),type=Read<uint8_t>(f+10)&63;
        if (type==10) {
            if (std::find(allowed.begin(),allowed.end(),key)==allowed.end() && !Links(p,key).empty()) return false;
        } else if (defaults) {
            for (unsigned j=0;j<Read<uint16_t>(f+12);++j) {
                auto mask=type==1?255ull:0xffffffffull;
                if ((CellHash(p,key,j,type)&mask)!=(CellHash(defaults,key,j,type,~0ull)&mask)) return false;
            }
        }
    }
    return true;
}
void LoadEncounters(const Catalog& catalog,recipe::Catalog& db) {
    auto builders=catalog.find("Faction/FactionMemberBuilder"),defs=catalog.find("Faction/Rewards/Definitions");
    if (builders==catalog.end() || defs==catalog.end()) return;
    auto ref=std::ranges::find(defs->second,"DLC5_Y3_VO_SummonSpiders",&Record::name);
    if (ref==defs->second.end()) return;
    for (const auto& [builder,name]:builders->second) {
        if (DemoTraitId(name)) continue;
        auto lists=Links(builder,0x1836875e);
        if (lists.size()!=1 || !EncounterRecord(lists[0],{0x0a535c57}) ||
            CellHash(lists[0],0xeffb4687,0,3)!=1 ||
            !EncounterRecord(builder,{0xef0db9af,0xbd1b75de,0x85aefa2f,0x54172756,0x1fe6b183,0x1836875e,0x77837511,0xc7a758d})) continue;
        if (Ptr instance=CellHash(builder,0xef0db9af,0);instance && Read<uint8_t>(instance)!=0) continue;
        recipe::Encounter origin;
        origin.builder=builder;origin.list=lists[0];origin.tribe=CellHash(builder,0xbd1b75de,0);
        origin.rank=CellHash(builder,0x85aefa2f,0);origin.tree=CellHash(builder,0x77837511,0);
        origin.markers=Links(builder,0x1fe6b183);
        for (Ptr list:Links(builder,0xc7a758d)) {
            auto roles=Links(list,0xc7a758d);origin.roles.insert(origin.roles.end(),roles.begin(),roles.end());
        }
        if (CellHash(builder,0xc7a758d,0) && origin.roles.empty()) continue;
        for (Ptr reward:Links(origin.list,0x0a535c57)) {
            if (!std::ranges::all_of(Links(reward,0x9bf805e3),[&](Ptr p){return db.campaignPredicates.contains(p);})) continue;
            if (!EncounterRecord(reward,{0x5b7f9956,0xa22cf1b9,0xf273ff9a,0x8148127c,
                0xdd4ea5f7,0xf15ea6b9,0x79f22fe8,0x99b5808a,0x9bf805e3},ref->address)) continue;
            auto e=origin;e.reward=reward;
            auto predicates=[&](unsigned field,unsigned link,auto add) {
                for (Ptr group:Links(reward,field)) {
                    auto values=Links(group,link);
                    if (values.empty() || !EncounterRecord(group,{link})) return false;
                    add(values);
                }
                return true;
            };
            if (!predicates(0xf273ff9a,0xff778c99,[&](auto& v){e.needsMarker.push_back(v);}) ||
                !predicates(0x8148127c,0xff778c99,[&](auto& v){e.blocksMarker.insert(v.begin(),v.end());}) ||
                !predicates(0x79f22fe8,0x7abf91f5,[&](auto& v){e.needsPicker.push_back(v);}) ||
                !predicates(0x99b5808a,0x7abf91f5,[&](auto& v){e.blocksPicker.insert(v.begin(),v.end());}) ||
                !predicates(0xdd4ea5f7,0x014ba459,[&](auto& v){e.types.insert(v.begin(),v.end());}) ||
                !predicates(0xf15ea6b9,0x014ba459,[&](auto& v){e.blockedTypes.insert(v.begin(),v.end());})) continue;
            auto read=[&](unsigned key,auto& out) {
                for (Ptr group:Links(reward,key)) {
                    auto members=Links(group,0x7abf91f5);
                    if (members.size()!=1 || !db.routes.contains(members.front())) return false;
                    out.push_back(members.front());
                }
                return true;
            };
            if (!read(0x5b7f9956,e.grants) || e.grants.empty() || !read(0xa22cf1b9,e.removes)) continue;
            db.encounterPickers.insert(e.grants.begin(),e.grants.end());
            db.encounters.push_back(std::move(e));
        }
    }
}
bool LoadRecipeCatalog(const Catalog& catalog,recipe::Catalog& db,Ptr army) {
    const auto nodes=catalog.find("Interface/Menu/MenuTreeNode/TraitGroup"),
        defs=catalog.find("Faction/Traits/Definitions"), pickers=catalog.find("Faction/Traits/Picker"),
        markers=catalog.find("Faction/Traits/Marker");
    if (nodes==catalog.end() || defs==catalog.end() || pickers==catalog.end() || markers==catalog.end()) return false;
    auto campaign=[&](Ptr record,unsigned field){
        for(Ptr p:Links(record,field))if(!db.campaignPredicates.contains(p)){
            const Ptr category=Read<Ptr>(p+0x18);
            if(category && Text(Read<Ptr>(category))=="Quest/GlobalVarPredicates")db.campaignPredicates.insert(p);
        }
    };
    for(auto [category,field]:{std::pair{"Faction/Roles/Definitions",0xc941b8e0u},
                               std::pair{"Faction/Rewards/Definitions",0x9bf805e3u}})
        if(auto it=catalog.find(category);it!=catalog.end())for(const auto& rec:it->second)campaign(rec.address,field);
    if (auto it=catalog.find("Faction/RowRanks");it!=catalog.end())
        for (const auto& rank:it->second) db.ranks.insert(rank.address);
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
            db.rolePickers.insert(grants.begin(),grants.end());
            db.roles.emplace(id,recipe::RoleKit{std::move(grants),Links(id,0xe0f81767),weapons(id)});
            for (Ptr mark:db.roles.at(id).markers) if (RecordName(mark).starts_with("RoleMarker_"))
                db.roleMarkers.insert(mark);
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
    auto raw=catalog.find("Faction/Traits/PickerListRaw");
    for (const auto& [id,name]:markers->second) {
        db.markerGroups[id]=Links(id,0xb2e40fc1);
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
            kit.reward=id;
            kit.addsMarkers=Links(id,0x53373e4d);
            kit.removesMarkers=Links(id,0xa839eb20);
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
            campaign(e,0xaf84910a);
            if (!Links(e,0xfa3aba01).empty()) db.positionPickers.insert(id);
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
    LoadEncounters(catalog,db);
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

bool Signature(Ptr address, const char* bytes, size_t count) {
    __try { return std::memcmp(reinterpret_cast<const void*>(game::FromRVA(address)), bytes, count) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Only POD lives in the SEH boundary; no UI callbacks touch native objects.
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
struct Task { uint64_t session=0, baseline=0; Key selected; Draft draft; };
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
    draft=std::move(next);return true;
}
bool TemplateFromMember(Ptr m,Snapshot& snap,Draft& draft,const Draft* retune) {
    auto db=snap.recipeCatalog;
    auto r=retune?BuildRecipe(*retune):MemberKit(m,db);
    if (!r.complete() || !db.tribes.contains(r.tribe) || !engine::ScopeClassDraws(db,r,snap.selected.army)) return false;
    if (!retune){
        std::vector<Ptr> active;
        if (!MemberRecords(m+0x198,active)) return false;
        auto fixed=recipe::coreInputs(db,r);
        const Ptr slots=Read<Ptr>(m+0x1e0);
        std::vector<recipe::Slot> granted;
        for(int i=0,n=VectorSize(m+0x1e0,32);i<n;++i)
            granted.push_back({Read<Ptr>(slots+32*i),Read<Ptr>(slots+32*i+8)});
        for(auto id:recipe::templateTraits(db,active,granted,fixed))r.traits[db.categories.at(id)].push_back(id);
    }
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
    return StageTemplate(snap,r,draft);
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
        if (auto& e=plan->encounter;e.reward) report+="Encounter: "+RecordName(e.builder)+" -> "+
            RecordName(e.list)+" -> "+RecordName(e.reward)+"\r\n";
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
    const auto* c=&d.edits.at(Kind::Regenerate).second;
    recipe::Catalog db;
    const auto r=BuildRecipe(d);
    if (!c->generation || !LoadRecipeCatalog(catalog,db,task.selected.army) ||
        !engine::ScopeClassDraws(db,r,task.selected.army)) return false;
    std::string detail;
    auto result=engine::ReplaceCaptain(m,*c->generation,db,r,SaveTraits,detail);
    if (result==engine::ReplaceResult::SeatUnavailable)
        status="The selected Army seat is not available for an in-place build. No build applied.";
    else if (result==engine::ReplaceResult::RollbackFailed)
        status="Build verification failed and the original could not be fully restored. Reload before continuing.";
    else if (result==engine::ReplaceResult::CleanupFailed)
        status="Build applied, but the engine failed to release temporary data. Restart before continuing.";
    else if (result==engine::ReplaceResult::Replaced)
        status="Applied the generated build to the original captain at his original level and Army seat.";
    else status=detail.empty()?"Candidate failed validation. No build applied.":detail;
    return result==engine::ReplaceResult::Replaced || result==engine::ReplaceResult::CleanupFailed;

}
}
namespace {
namespace act=sowmod::actions;
constexpr act::Input traitInput(const char* key,const char* label){
    return {key,label,act::MultiChoice,"",0,0,nullptr,{nullptr,act::OnOpen|act::OnInputs|act::OnDraft,0x1f}};
}
constexpr act::Input recipeInputs[]={
    {"captain","Captain",act::Choice},{"base","Base class",act::Choice},
    {"advanced","Advanced class",act::Choice,nullptr,0,0,nullptr,{nullptr,act::OnOpen|act::OnInputs,1u<<1}},{"tribe","Tribe",act::Choice},{"rarity","Rarity",act::Choice},
    traitInput("immunities","Immunities"),traitInput("hates","Hates"),
    traitInput("class_traits","Class traits"),traitInput("bonuses","Bonuses"),
    traitInput("mortal","Mortal weaknesses"),traitInput("vulnerabilities","Vulnerabilities"),
    traitInput("dazed","Dazed by"),traitInput("hints","Hints")};
constexpr Kind actionKinds[]={Kind::Target,Kind::Base,Kind::Class,Kind::Tribe,Kind::Rarity,Kind::Immunities,
    Kind::Hates,Kind::Classes,Kind::Bonuses,Kind::Mortal,Kind::Vulnerable,Kind::Dazed,Kind::Hints};
std::string seatId(const Target& t){return RecordName(t.key.army)+":"+std::to_string(t.key.index);}
bool findCaptain(const char* id,Target& target){
    for(auto& t:Targets())if(seatId(t)==id){target=t;return true;}
    return false;
}
bool matches(std::string_view s,std::string_view filter){
    return filter.empty() || std::search(s.begin(),s.end(),filter.begin(),filter.end(),[](unsigned char a,unsigned char b){
        return std::tolower(a)==std::tolower(b);})!=s.end();
}
bool ReadRecipeDraft(const Snapshot& s,const char* const* v,Draft& draft,act::Emit emit,void* ctx){
    for(unsigned i=1;i<std::size(actionKinds);++i){
        auto f=std::ranges::find(s.fields,actionKinds[i],&Field::kind);
        if(f==s.fields.end()){emit(ctx,"recipe catalog unavailable");return false;}
        std::string_view parts=v[i];Choice selected{};
        while(!parts.empty()){
            auto end=parts.find(',');auto id=parts.substr(0,end);
            auto c=std::ranges::find_if(f->choices,[&](auto& ch){return RecordName(ch.value)==id;});
            if(c==f->choices.end()){emit(ctx,("unknown "+std::string(recipeInputs[i].key)+": "+std::string(id)).c_str());return false;}
            if(Bucket(f->kind))selected.pickers.push_back(c->value);else selected=*c;
            if(end==parts.npos)break;parts.remove_prefix(end+1);
        }
        draft.stage(*f,selected);
    }
    return true;
}
bool actionChoices(unsigned n,const char* const* v,const char* search,act::Emit emit,void* ctx){
    if(!n){
        for(auto& t:Targets()){auto id=seatId(t);if(matches(id,search) || matches(t.label,search))emit(ctx,(id+"\t"+t.label).c_str());}
        return true;
    }
    Target t;if(n>=std::size(actionKinds) || !findCaptain(v[0],t)){emit(ctx,"select a captain first");return false;}
    Snapshot s;s.selected=t.key;RecipeFields(s,LoadCatalog());
    Ptr base=0;
    for(auto& f:s.fields)if(f.kind==Kind::Base)for(auto& c:f.choices)if(RecordName(c.value)==v[1])base=c.value;
    const bool trait=Bucket(actionKinds[n]);
    recipe::Recipe r;const auto session=sowmod::SaveSession();
    const auto rank=CellHash(Read<Ptr>(t.member+0x268),0x40c579dc,0);
    if(trait){
        Draft draft;if(!ReadRecipeDraft(s,v,draft,emit,ctx))return false;
        r=BuildRecipe(draft);
        if(!r.complete())return true;
        if(!engine::ScopeClassDraws(s.recipeCatalog,r,t.key.army))return false;
    }
    for(auto& f:s.fields)if(f.kind==actionKinds[n])for(auto& c:f.choices){
        if(n==2 && (!base || !recipe::fitsClass(s.recipeCatalog,base,c.value)))continue;
        auto id=RecordName(c.value);
        if(!matches(id,search) && !matches(c.label,search))continue;
        if(trait){
            auto& picks=r.traits[int(f.kind)-int(Kind::Immunities)];
            if(std::ranges::find(picks,c.value)==picks.end()){
                picks.push_back(c.value);
                engine::GenerationPlan plan;std::string reason;
                bool ok=engine::PlanGeneration(t.member,rank,s.recipeCatalog,r,plan,reason,session);
                picks.pop_back();if(!ok)continue;
            }
        }
        emit(ctx,(id+"\t"+c.label).c_str());
    }
    return true;
}
void emitRecipe(const recipe::Recipe& r,act::Emit emit,void* ctx){
    const Ptr kit[]={r.base,r.specialization,r.tribe,r.rarity};
    for(unsigned i=1;i<std::size(recipeInputs);++i){
        std::string line=std::string(recipeInputs[i].key)+"=";
        if(i<=4)line+=RecordName(kit[i-1]);
        else for(auto id:r.traits[i-5]){if(line.back()!='=')line+=',';line+=RecordName(id);}
        emit(ctx,line.c_str());
    }
}
bool templateAction(const char* const* v,act::Emit emit,void* ctx){
    Target t;if(!findCaptain(v[0],t)){emit(ctx,"select a captain first");return false;}
    Snapshot s;s.selected=t.key;s.session=sowmod::SaveSession();RecipeFields(s,LoadCatalog());
    Draft draft;
    if(!TemplateFromMember(t.member,s,draft,nullptr)){emit(ctx,"no compatible template for this captain");return false;}
    emitRecipe(BuildRecipe(draft),emit,ctx);return true;
}
bool reconcileAction(const char* const* v,act::Emit emit,void* ctx){
    Target t;if(!findCaptain(v[0],t)){emit(ctx,"select a captain first");return false;}
    Snapshot s;s.selected=t.key;s.session=sowmod::SaveSession();RecipeFields(s,LoadCatalog());
    Draft input;if(!ReadRecipeDraft(s,v,input,emit,ctx))return false;
    auto r=BuildRecipe(input);
    if(r.base && r.specialization && !recipe::fitsClass(s.recipeCatalog,r.base,r.specialization)){
        emit(ctx,"advanced=");return true;
    }
    if(!r.complete())return true; // No kit to validate against yet. Keep pending picks.
    auto& db=s.recipeCatalog;
    if(!engine::ScopeClassDraws(db,r,t.key.army)){emit(ctx,"Cannot check this kit yet. Draft kept.");return false;}
    auto rank=CellHash(Read<Ptr>(t.member+0x268),0x40c579dc,0);
    // Eliminate structural mismatches before searching combinations. Native
    // prerequisites are checked on whole sets, never inferred from a lone trait.
    for(size_t k=0;k<r.traits.size();++k)std::erase_if(r.traits[k],[&](Ptr id){
        auto one=r;one.traits={};one.traits[k]={id};
        return !recipe::resolve(db,one,[](const auto&){return true;});
    });
    if(!recipe::retainSelections(r,[&](const auto& candidate){
        engine::GenerationPlan plan;std::string error;
        return engine::PlanGeneration(t.member,rank,db,candidate,plan,error,s.session);
    })){emit(ctx,"Could not resolve the new kit without guessing. Draft kept.");return false;}
    emitRecipe(r,emit,ctx);return true;
}
bool generateAction(const char* const* v,act::Emit emit,void* ctx) {
    Target t;if(!findCaptain(v[0],t) || !Writable(t.member)){emit(ctx,"captain unavailable");return false;}
    Task task;task.session=sowmod::SaveSession();task.selected=t.key;task.baseline=Stamp(t.member);
    if(!task.session)return false;
    Snapshot s;s.selected=t.key;s.session=task.session;auto catalog=LoadCatalog();RecipeFields(s,catalog);
    if(!ReadRecipeDraft(s,v,task.draft,emit,ctx))return false;
    auto r=BuildRecipe(task.draft);auto gen=std::make_shared<engine::GenerationPlan>();std::string reason;
    if(!engine::Supported() || !engine::ScopeClassDraws(s.recipeCatalog,r,t.key.army) ||
       !engine::PlanGeneration(t.member,CellHash(Read<Ptr>(t.member+0x268),0x40c579dc,0),s.recipeCatalog,r,*gen,reason,task.session)){
        emit(ctx,reason.empty()?"recipe not available":reason.c_str());return false;}
    Choice prepared{};prepared.generation=std::move(gen);
    task.draft.edits[Kind::Regenerate]={0,std::move(prepared)};
    if(sowmod::SaveSession()!=task.session){emit(ctx,"save session changed");return false;}
    for(auto& t:Targets())if(t.key==task.selected && Stamp(t.member)==task.baseline){
        std::string status;bool ok=Commit(t.member,task,catalog,status);
        if(status.empty())status="engine rejected save";
        if(!GenerationReport(task,catalog,t.member,status))status+=" (generation log unavailable)";
        emit(ctx,status.c_str());return ok;
    }
    emit(ctx,"Captain changed during generation. No build applied.");return false;
}
}
bool RegisterGenerationActions(const act::Registrar& r){
    return r.Add(act::Action{"inspect","Inspect captain",true,recipeInputs,1,[](const char* const* v,act::Emit emit,void* ctx){
        Target t;if(!findCaptain(v[0],t)){emit(ctx,"captain not found");return false;}
        emit(ctx,t.label.c_str());emit(ctx,("level="+std::to_string(1+Read<unsigned>(t.member+0x4b8))).c_str());recipe::Catalog db;
        if(!LoadRecipeCatalog(LoadCatalog(),db,t.key.army))return false;
        emit(ctx,KitText(MemberKit(t.member,db)).c_str());
        std::vector<Ptr> active;if(!MemberRecords(t.member+0x198,active))return false;
        for(auto id:active)emit(ctx,RecordName(id).c_str());return true;},actionChoices}.IpcOnly()) &&
        r.Add(act::Action{"generate","Generate Captain",true,recipeInputs,unsigned(std::size(recipeInputs)),generateAction,actionChoices,nullptr,templateAction,"Take Over Template"}) &&
        r.Reconcile("generate",0x1f,reconcileAction);
}
}
