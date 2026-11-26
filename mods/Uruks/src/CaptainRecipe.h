#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>
#include <optional>
#include "TraitRules.h"

namespace uruk::recipe {
using Id = std::uintptr_t;
inline constexpr std::array nodes{
    "Strengths_Immunities", "Strengths_Hates", "Strengths_Classes", "Strengths_Bonus",
    "Weakness_MortalVulnerabilities", "Weakness_Vulnerabilities", "Weakness_WeakenedBy", "Weakness_Hints"};
inline constexpr std::array labels{
    "Immunities", "Hates", "Class Traits", "Bonuses", "Mortal Weaknesses", "Vulnerabilities", "Dazed By", "Hints"};
struct Picker {
    Id id{};
    std::vector<Id> groups, required, forbidden;
};
struct ClassKit {
    std::set<Id> allowedTypes, blockedTypes, forbiddenMarkers, removed;
    std::vector<std::vector<Id>> requiredAny, grants;
    std::vector<Id> drawLists;
    Id reward{};
    std::vector<Id> addsMarkers, removesMarkers;
    bool fits(Id type) const {
        return !blockedTypes.contains(type) && (allowedTypes.empty() || allowedTypes.contains(type));
    }
};
struct RoleKit {
    std::vector<Id> grants, markers, weapons;
};
struct Encounter {
    Id builder{},list{},reward{},tribe{},rank{},tree{};
    std::vector<Id> roles,markers,grants,removes;
    std::vector<std::vector<Id>> needsPicker,needsMarker;
    std::set<Id> blocksPicker,blocksMarker,types,blockedTypes;
    bool operator==(const Encounter&) const = default;
};
struct Catalog {
    std::vector<Encounter> encounters;
    std::set<Id> encounterPickers,rolePickers,roleMarkers;
    std::map<Id,std::vector<Id>> markerGroups;
    std::map<Id, unsigned> categories;
    std::map<Id,Id> weaknessGatedHateEntries; // native option row -> single Hate output
    std::map<Id,std::set<Id>> hiddenDependencies,hiddenProducers;
    std::map<Id, std::vector<Picker>> producers;
    std::set<Id> definitions, classes, rarities;
    std::set<Id> epics;
    std::map<Id,unsigned> epicCaps;
    std::map<Id,Id> neverClass;
    std::set<Id> allSpecializations;
    std::map<Id,Id> rarityRecords;
    std::map<Id,Id> classOnlyPickers;
    std::set<Id> combatClasses;
    std::map<Id,Id> trees, types, baseGrants;
    using RewardPools=std::map<Id,std::set<Id>>;
    std::map<Id,RewardPools> classRewards;
    std::map<Id,std::vector<Id>> tribes;
    std::map<Id,std::vector<ClassKit>> kits;
    std::set<Id> managedPickers, managedTraits;
    std::set<Id> optionalOnly;
    std::map<Id,Picker> routes;
    std::optional<engine::TraitRules> classDraws;
    std::map<Id,RoleKit> roles;
    std::map<Id,std::vector<Id>> traitWeapons;
    std::map<Id,std::vector<Id>> pairedWeapons;
    std::map<Id,std::set<unsigned>> weaponSlots;
    std::set<Id> bothHands;
    std::map<Id,Id> roleLists; // eligible role -> native source list for this army/tree
    engine::TraitRules ordinaryDraws;
    std::set<Id> fixedClassInputs;
};
struct Recipe {
    Id specialization{}, rarity{};
    std::array<std::vector<Id>,nodes.size()> traits;
    Id base{},tribe{};
    bool complete() const { return base && specialization && rarity && tribe; }
    bool operator==(const Recipe&) const = default;
};
struct Slot { Id picker{}, definition{}; };
// An inactive slot can retain its picker after its active definition is cleared.
// Infer a missing output only for an exclusively optional, single-output picker.
inline std::vector<Id> templateTraits(const Catalog& db,const std::vector<Id>& active,
                                     const std::vector<Slot>& slots,const std::vector<Id>& fixed){
    auto automatic=db.managedTraits;
    auto owned=[&](Id p){return db.managedPickers.contains(p) || std::ranges::find(fixed,p)!=fixed.end();};
    for(auto slot:slots)if(owned(slot.picker))automatic.insert(slot.definition);
    std::vector<Id> result;
    auto add=[&](Id id){
        if(db.producers.contains(id) && !automatic.contains(id) && std::ranges::find(result,id)==result.end())
            result.push_back(id);
    };
    for(Id id:active)add(id);
    for(auto [picker,definition]:slots){
        if(!picker || owned(picker))continue;
        if(definition){
            auto it=db.producers.find(definition);
            if(it!=db.producers.end() && std::ranges::find(it->second,picker,&Picker::id)!=it->second.end())add(definition);
        }else if(db.optionalOnly.contains(picker)){
            Id output=0;
            for(auto& [id,producers]:db.producers)if(std::ranges::find(producers,picker,&Picker::id)!=producers.end()){
                if(output){output=0;break;}
                output=id;
            }
            if(output)add(output);
        }
    }
    return result;
}
// One captain has one role. Ordinary draws may still supply any of the other picks.
template<class Accept>
bool withRole(const Catalog& db,const std::vector<Id>& classPicks,Accept accept,const std::vector<Id>& allowed={}) {
    if (allowed.empty() && db.ordinaryDraws.permits(classPicks)) return accept(Id{});
    for (auto [role,list]:db.roleLists) {
        if (!allowed.empty() && std::ranges::find(allowed,role)==allowed.end()) continue;
        const auto& grants=db.roles.at(role).grants;
        auto remaining=classPicks;
        std::erase_if(remaining,[&](Id id){return std::ranges::find(grants,id)!=grants.end();});
        if ((!allowed.empty() || remaining.size()!=classPicks.size()) && db.ordinaryDraws.permits(remaining) && accept(role)) return true;
    }
    return false;
}
inline bool fitsEncounter(const Catalog& db,const Recipe& r,const Encounter& e,Id rank) {
    if ((e.tribe && r.tribe!=e.tribe) || (e.rank && rank!=e.rank) ||
        (e.tree && (!db.trees.contains(r.base) || db.trees.at(r.base)!=e.tree))) return false;
    if ((!e.types.empty() || !e.blockedTypes.empty()) && (!db.types.contains(r.base) ||
        e.blockedTypes.contains(db.types.at(r.base)) || (!e.types.empty() && !e.types.contains(db.types.at(r.base))))) return false;
    for (Id marker:e.markers) {
        if ((db.combatClasses.contains(marker) && marker!=r.base) ||
            (db.allSpecializations.contains(marker) && marker!=r.specialization)) return false;
        if (db.neverClass.contains(r.specialization) && marker==db.neverClass.at(r.specialization)) return false;
        if (db.rarityRecords.contains(marker) && (!db.rarityRecords.contains(r.rarity) ||
            db.rarityRecords.at(marker)!=db.rarityRecords.at(r.rarity))) return false;
    }
    return true;
}
inline bool compatibleMarkers(const Catalog& db,const std::set<Id>& markers) {
    std::set<Id> held;
    for (Id id:markers) if (auto i=db.markerGroups.find(id);i!=db.markerGroups.end())
        for (Id group:i->second) if (!held.insert(group).second) return false;
    return true;
}
// Filter authored reward effects, not a sample of what the RNG happened to draw.
inline bool classRewardPool(const Catalog& db,const Recipe& r,const std::set<Id>& required,
                            const std::set<Id>& forbidden,std::set<Id>& blocked) {
    blocked.clear();
    bool available=false;
    for (const auto& kit:db.kits.at(r.specialization)) {
        auto markers=required;
        markers.insert(kit.addsMarkers.begin(),kit.addsMarkers.end());
        if (!kit.fits(db.types.at(r.base)) || !compatibleMarkers(db,markers) ||
            std::ranges::any_of(kit.addsMarkers,[&](Id id){return forbidden.contains(id);}) ||
            std::ranges::any_of(kit.removesMarkers,[&](Id id){return required.contains(id);})) {
            blocked.insert(kit.reward);
        } else available=true;
    }
    return available;
}
using WeaponSlots=std::map<unsigned,std::set<Id>>;
using Loadout=std::map<unsigned,Id>; // zero reserves a hand that must remain empty
// Items in one source are alternatives per slot. Independent requirements intersect.
inline bool requireWeapons(WeaponSlots& plan,const WeaponSlots& source) {
    for (const auto& [slot,items]:source) {
        if (items.empty()) return false;
        auto [it,added]=plan.try_emplace(slot,items);
        if (!added) std::erase_if(it->second,[&](Id item){return !items.contains(item);});
        if (it->second.empty()) return false;
    }
    return true;
}
template<class Allowed>
bool lockWeapons(const Catalog& db,const WeaponSlots& clauses,Allowed allowed,Loadout& out) {
    Loadout chosen;
    auto attach=[&](auto&& self,unsigned slot,Id item)->bool {
        if (chosen.contains(slot)) return chosen.at(slot)==item;
        if (clauses.contains(slot) && !clauses.at(slot).contains(item)) return false;
        chosen[slot]=item;
        if (!item) return true;
        if (!allowed(item)) return false;
        auto pairs=db.pairedWeapons.find(item);
        if (pairs!=db.pairedWeapons.end()) for (Id other:pairs->second) {
            auto s=db.weaponSlots.find(other);
            if (s==db.weaponSlots.end() || s->second.size()!=1 || !self(self,*s->second.begin(),other)) return false;
        }
        if (slot!=0 || !db.bothHands.contains(item)) return true;
        if (pairs!=db.pairedWeapons.end() && chosen.contains(9) &&
            std::ranges::find(pairs->second,chosen.at(9))!=pairs->second.end()) return true;
        return self(self,9,0);
    };
    auto solve=[&](auto&& self,auto it)->bool {
        if (it==clauses.end()) return true;
        for (Id item:it->second) {
            auto before=chosen;
            if (attach(attach,it->first,item) && self(self,std::next(it))) return true;
            chosen.swap(before);
        }
        return false;
    };
    if (!solve(solve,clauses.begin())) return false;
    out=std::move(chosen);return true;
}
template<class Allowed>
bool planWeapons(const Catalog& db,const Id* active,int count,Id role,Allowed allowed,Loadout& result) {
    WeaponSlots clauses;
    auto require=[&](const std::vector<Id>& items,bool mandatory){
        WeaponSlots source;
        for (Id item:items) {
            auto slots=db.weaponSlots.find(item);
            if (slots!=db.weaponSlots.end() && slots->second.size()==1 && allowed(item))
                source[*slots->second.begin()].insert(item);
        }
        return (!mandatory || !source.empty()) && requireWeapons(clauses,source);
    };
    if (role && !db.roles.at(role).weapons.empty() && !require(db.roles.at(role).weapons,true)) return false;
    for (int i=0;i<count;++i)
        if (auto it=db.traitWeapons.find(active[i]);it!=db.traitWeapons.end() && !require(it->second,true)) return false;
    return lockWeapons(db,clauses,allowed,result);
}
inline bool permitsSlot(const Catalog& db,const Loadout& locked,Id item) {
    auto slots=db.weaponSlots.find(item);
    if (slots==db.weaponSlots.end()) return true;
    for (unsigned slot:slots->second) {
        if (auto it=locked.find(slot);it!=locked.end() && it->second!=item) return false;
        if (slot==0 && db.bothHands.contains(item) && locked.contains(9) && locked.at(9)) {
            auto pairs=db.pairedWeapons.find(item);
            if (pairs==db.pairedWeapons.end() || std::ranges::find(pairs->second,locked.at(9))==pairs->second.end()) return false;
        }
    }
    return true;
}
inline bool permitsGeneratedWeapon(const Catalog& db,const Loadout& locked,Id item) {
    if (!db.pairedWeapons.contains(item)) return permitsSlot(db,locked,item);
    std::set<Id> visited;
    auto check=[&](auto&& self,Id id)->bool {
        if (!visited.insert(id).second) return true;
        if (!permitsSlot(db,locked,id)) return false;
        auto p=db.pairedWeapons.find(id);
        return p==db.pairedWeapons.end() ||
            std::ranges::all_of(p->second,[&](Id paired){return self(self,paired);});
    };
    return check(check,item);
}
template<class Eligible>
void addClassRewardDraws(const Catalog& db,const Recipe& r,engine::TraitRules& draws,Eligible eligible) {
    for (Id owner:{r.base,r.specialization}) if (auto it=db.classRewards.find(owner);it!=db.classRewards.end())
        for (const auto& [track,options]:it->second) {
            std::set<Id> pool;
            for (Id p:options) if (eligible(p)) pool.insert(p);
            if (!pool.empty()) draws.sources.push_back({1,{std::move(pool)}});
        }
}
inline void indexWeaknessGate(Catalog& db,Id entry,const std::vector<Id>& outputs,const std::vector<Id>& anyOf) {
    if (outputs.size()!=1) return;
    const auto output=db.categories.find(outputs.front());
    if (output==db.categories.end() || output->second!=1) return;
    if (std::ranges::any_of(anyOf,[&](Id id){
        auto it=db.categories.find(id);
        return it!=db.categories.end() && it->second>=4 && it->second<nodes.size();
    })) db.weaknessGatedHateEntries.emplace(entry,outputs.front());
}
inline bool waiveWeaknessGate(const Catalog& db,const Recipe& r,Id entry) {
    auto it=db.weaknessGatedHateEntries.find(entry);
    return it!=db.weaknessGatedHateEntries.end() &&
        std::ranges::find(r.traits[1],it->second)!=r.traits[1].end();
}
// A picker can resolve to different definitions at different levels. A fixed
// output of one producer must never disable a different producer wholesale.
inline void classifyClassDraws(Catalog& db,engine::TraitRules draws,const std::set<Id>& direct) {
    db.classDraws=std::move(draws);
    db.managedPickers.insert(direct.begin(),direct.end());
    for (const auto& [trait,producers]:db.producers) {
        bool fixed=std::ranges::any_of(producers,[&](const Picker& p){return direct.contains(p.id);});
        if (fixed || (db.categories.at(trait)==2 && !std::ranges::any_of(producers,[&](const Picker& p){
            return !db.managedPickers.contains(p.id) && db.classDraws->groups.contains(p.id);
        }))) db.managedTraits.insert(trait);
    }
    for (auto trait:db.managedTraits) if (db.producers.contains(trait))
        for (const auto& p:db.producers.at(trait)) db.optionalOnly.erase(p.id);
}
inline std::vector<Id> coreInputs(const Catalog& db,const Recipe& r) {
    std::vector<Id> fixed;
    for (Id marker:{r.base,r.specialization}) if (db.baseGrants.contains(marker)) fixed.push_back(db.baseGrants.at(marker));
    for (Id id:db.fixedClassInputs) if (std::ranges::find(fixed,id)==fixed.end()) fixed.push_back(id);
    if (auto t=db.tribes.find(r.tribe);t!=db.tribes.end()) for (Id id:t->second) {
        auto route=db.routes.find(id);
        if (route!=db.routes.end() && std::ranges::any_of(route->second.forbidden,[&](Id m){
            return m==r.base || m==r.specialization || m==r.rarity;
        })) continue;
        if (std::ranges::find(fixed,id)==fixed.end()) fixed.push_back(id);
    }
    return fixed;
}
inline bool fitsCore(const Catalog& db,const std::vector<Id>& fixed,const std::vector<Id>& picks) {
    for (Id id:picks) if (auto route=db.routes.find(id);route!=db.routes.end())
        for (Id f:fixed) if (f!=id && db.routes.contains(f))
            for (Id g:db.routes.at(f).groups)
                if (std::ranges::find(route->second.groups,g)!=route->second.groups.end()) return false;
    return true;
}
inline bool fitsClass(const Catalog& db,Id base,Id specialization) {
    auto type=db.types.find(base);
    auto kits=db.kits.find(specialization);
    return type!=db.types.end() && type->second && kits!=db.kits.end() &&
        std::ranges::any_of(kits->second,[&](const ClassKit& kit){return kit.fits(type->second);});
}
inline bool markerInputs(const Catalog& db,const Recipe& r,const std::vector<Id>& grants,
                         std::set<Id>& required,std::set<Id>& forbidden) {
    required={r.base,r.specialization,r.rarity}; forbidden.clear();
    for (Id p:grants) if (auto it=db.routes.find(p);it!=db.routes.end()) {
        for (Id m:it->second.required) {
            if ((db.combatClasses.contains(m) && m!=r.base) ||
                (db.allSpecializations.contains(m) && m!=r.specialization) ||
                (db.rarities.contains(m) && m!=r.rarity)) return false;
            required.insert(m);
        }
        forbidden.insert(it->second.forbidden.begin(),it->second.forbidden.end());
    }
    return std::ranges::none_of(required,[&](Id id){return forbidden.contains(id);});
}
template<class Accept>
bool withRequiredRole(const Catalog& db,const Recipe& r,const Encounter* e,const std::vector<Id>& fixed,
                      const std::vector<Id>& selected,const std::vector<Id>& classPicks,
                      const std::vector<Id>& expanded,Accept accept) {
    auto has=[](const auto& list,Id id){return std::ranges::find(list,id)!=list.end();};
    std::vector<Id> dependent,allowed;
    std::set<Id> required,forbidden;
    if (!markerInputs(db,r,expanded,required,forbidden)) return false;
    if (e) for (Id marker:e->markers)
        if (std::ranges::any_of(e->needsMarker,[&](const auto& clause){return has(clause,marker);})) required.insert(marker);
    for (Id id:expanded) {
        if (has(fixed,id) || (e && has(e->grants,id))) continue;
        if (has(classPicks,id) || db.encounterPickers.contains(id) ||
            (!has(selected,id) && db.rolePickers.contains(id))) dependent.push_back(id);
    }
    if ((e && !e->roles.empty()) || std::ranges::any_of(required,[&](Id id){return db.roleMarkers.contains(id);})) {
        for (const auto& [id,list]:db.roleLists) {
            if (e && !e->roles.empty() && !has(e->roles,id)) continue;
            if (std::ranges::all_of(required,[&](Id mark){return !db.roleMarkers.contains(mark) ||
                has(db.roles.at(id).markers,mark);})) allowed.push_back(id);
        }
        if (allowed.empty()) return false;
    }
    return withRole(db,dependent,accept,allowed);
}
// An authored OR-list is one prerequisite, not a request to grant every alternative.
template<class Accept>
bool withRewardRequirements(const Encounter& source,std::vector<Id>& inputs,unsigned& budget,Accept accept) {
    Encounter chosen=source;
    auto walk=[&](auto&& self,size_t i)->bool {
        if (!budget) return false;
        --budget;
        if (i==source.needsPicker.size()+source.needsMarker.size()) return accept(chosen,inputs);
        bool marker=i>=source.needsPicker.size();
        const auto& options=marker?source.needsMarker[i-source.needsPicker.size()]:source.needsPicker[i];
        auto& result=marker?chosen.markers:inputs;
        for (Id v:options) {
            if ((marker?source.blocksMarker:source.blocksPicker).contains(v)) continue;
            // A reward cannot supply its own precondition.
            if (!marker && std::ranges::find(source.grants,v)!=source.grants.end()) continue;
            bool add=std::ranges::find(result,v)==result.end();
            if (add) result.push_back(v);
            bool ok=self(self,i+1);
            if (add) result.pop_back();
            if (ok) return true;
        }
        return false;
    };
    return walk(walk,0);
}
// Try the native inputs first. Only expand missing, non-UI prerequisites; the
// final projection must still yield precisely the player's visible categories.
template<class Accept>
bool withHiddenGrants(const Catalog& db,std::vector<Id>& grants,unsigned& budget,Accept& accept) {
    if (!budget || grants.size()>512) return false;
    --budget;
    if (accept(grants)) return true;
    const size_t count=grants.size();
    for (size_t i=0;i<count;++i) {
        auto deps=db.hiddenDependencies.find(grants[i]);
        if (deps==db.hiddenDependencies.end()) continue;
        for (Id d:deps->second) {
            auto routes=db.hiddenProducers.find(d);
            if (db.categories.contains(d) || routes==db.hiddenProducers.end()) continue;
            for (Id p:routes->second) {
                if (!budget) return false;
                if (std::ranges::find(grants,p)!=grants.end() || !fitsCore(db,grants,{p})) continue;
                grants.push_back(p);
                if (withHiddenGrants(db,grants,budget,accept)) return true;
                grants.pop_back();
            }
        }
    }
    return false;
}

// A dormant slot is engine-managed too. Never erase it just because one of its
// possible outputs appears on the Army screen at a different level.
inline bool retained(const Catalog& db, const std::vector<Slot>& slots, std::vector<Id>& result) {
    result.clear();
    for (auto [picker,trait]:slots) {
        if (!picker || (trait && !db.definitions.contains(trait))) return false;
        if (!db.categories.contains(trait) && !db.managedPickers.contains(picker) && !db.managedTraits.contains(trait)) result.push_back(picker);
    }
    return true;
}

// A definition can have several producers. Conflicts on one producer must not
// hide a definition that another producer can legally supply.
template<class Accept,class Extend>
bool resolve(const Catalog& db,const Recipe& recipe,Accept accept,Extend extend,unsigned* remaining=nullptr,
             const std::map<Id,std::set<Id>>* eligible=nullptr,unsigned automaticEpics=0) {
    if ((recipe.specialization && !db.classes.contains(recipe.specialization)) ||
        (recipe.rarity && !db.rarities.contains(recipe.rarity)) ||
        (recipe.base && !db.trees.contains(recipe.base)) ||
        (recipe.tribe && !db.tribes.contains(recipe.tribe))) return false;
    if (recipe.base && recipe.specialization && !fitsClass(db,recipe.base,recipe.specialization)) return false;
    std::vector<std::pair<Id,const std::vector<Picker>*>> rows;
    std::set<Id> distinct;
    unsigned epics=automaticEpics;
    for (unsigned i=0;i<nodes.size();++i) for (auto trait:recipe.traits[i]) {
        auto group=db.categories.find(trait);
        auto options=db.producers.find(trait);
        if (db.managedTraits.contains(trait) || group==db.categories.end() || group->second!=i || options==db.producers.end() ||
            !distinct.insert(trait).second) return false;
        rows.emplace_back(trait,&options->second);
        epics+=db.epics.contains(trait);
    }
    if (auto cap=db.epicCaps.find(recipe.rarity);cap!=db.epicCaps.end() && epics>cap->second) return false;
    std::stable_sort(rows.begin(),rows.end(),[](auto a,auto b){ return a.second->size()<b.second->size(); });
    std::set<Id> groups,used,required,forbidden;
    if (recipe.specialization) required.insert(recipe.specialization);
    if (recipe.base) required.insert(recipe.base);
    if (recipe.rarity) required.insert(recipe.rarity);
    std::vector<Id> picks,classPicks;
    auto walk=[&](auto&& self,size_t at)->bool {
        if (remaining) {
            if (!*remaining) return false;
            --*remaining;
        }
        if (at==rows.size()) {
            if constexpr (requires {accept(picks,classPicks);}) return accept(picks,classPicks);
            else return accept(picks);
        }
        auto [trait,routes]=rows[at];
        if (eligible && !eligible->contains(trait)) return false;
        for (const auto& p:*routes) {
            if (remaining && !*remaining) return false;
            if ((eligible && !eligible->at(trait).contains(p.id)) || db.managedPickers.contains(p.id) || used.contains(p.id) || std::ranges::any_of(p.groups,[&](Id g){return groups.contains(g);}) ||
                std::ranges::any_of(p.required,[&](Id m){return forbidden.contains(m) ||
                    std::ranges::any_of(required,[&](Id other){return other!=m &&
                        ((db.combatClasses.contains(m) && db.combatClasses.contains(other)) ||
                         (db.classes.contains(m) && db.classes.contains(other)) ||
                         (db.rarities.contains(m) && db.rarities.contains(other)));}) ||
                    (recipe.specialization && db.classes.contains(m) && m!=recipe.specialization) ||
                    (recipe.rarity && db.rarities.contains(m) && m!=recipe.rarity);}) ||
                std::ranges::any_of(p.forbidden,[&](Id m){return required.contains(m);})) continue;
            auto beforeRequired=required, beforeForbidden=forbidden;
            required.insert(p.required.begin(),p.required.end()); forbidden.insert(p.forbidden.begin(),p.forbidden.end());
            used.insert(p.id); groups.insert(p.groups.begin(),p.groups.end()); picks.push_back(p.id);
            bool classPick=db.classDraws && db.categories.at(trait)==2;
            if (classPick) classPicks.push_back(p.id);
            if ((!classPick || db.classDraws->permits(classPicks)) && extend(picks) && self(self,at+1)) return true;
            if (classPick) classPicks.pop_back();
            picks.pop_back(); used.erase(p.id); for (auto g:p.groups) groups.erase(g);
            required.swap(beforeRequired); forbidden.swap(beforeForbidden);
        }
        return false;
    };
    return walk(walk,0);
}
template<class Accept>
bool resolve(const Catalog& db,const Recipe& recipe,Accept accept) {
    return resolve(db,recipe,accept,[](const auto&){return true;});
}
inline bool matches(const Catalog& db,const Recipe& wanted,const Id* active,size_t count,const std::set<Id>& automatic={}) {
    std::array<std::set<Id>,nodes.size()> actual;
    for (size_t i=0;i<count;++i) {
        if (!db.definitions.contains(active[i])) return false;
        if (!automatic.contains(active[i])) {
            if (db.managedTraits.contains(active[i])) return false;
            if (auto it=db.categories.find(active[i]);it!=db.categories.end()) actual[it->second].insert(active[i]);
        }
    }
    for (auto id:automatic) if (!count || std::find(active,active+count,id)==active+count) return false;
    for (unsigned i=0;i<nodes.size();++i)
        if (actual[i]!=std::set<Id>(wanted.traits[i].begin(),wanted.traits[i].end())) return false;
    return true;
}
inline std::vector<Id> exclusions(const Catalog& db,const std::vector<Id>& grants) {
    std::vector<Id> result;
    for (Id id:db.optionalOnly) if (!db.managedPickers.contains(id) &&
        std::ranges::find(grants,id)==grants.end()) result.push_back(id);
    return result;
}
inline bool optionalResult(const Catalog& db,const Recipe& r,const std::vector<Id>& active,const std::set<Id>& automatic={}) {
    std::array<std::set<Id>,nodes.size()> actual;
    for (auto id:active) {
        auto group=db.categories.find(id);
        if (group!=db.categories.end() && !db.managedTraits.contains(id) && !automatic.contains(id))
            actual[group->second].insert(id);
    }
    for (size_t i=0;i<nodes.size();++i)
        if (actual[i]!=std::set<Id>(r.traits[i].begin(),r.traits[i].end())) return false;
    return true;
}
}
