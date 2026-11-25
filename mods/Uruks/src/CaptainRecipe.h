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
    bool fits(Id type) const {
        return !blockedTypes.contains(type) && (allowedTypes.empty() || allowedTypes.contains(type));
    }
};
struct Catalog {
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
        if (at==rows.size()) return accept(picks);
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
inline bool optionalResult(const Catalog& db,const Recipe& r,const std::vector<Id>& active) {
    std::array<std::set<Id>,nodes.size()> actual;
    for (auto id:active) {
        auto group=db.categories.find(id);
        if (group!=db.categories.end() && !db.managedTraits.contains(id))
            actual[group->second].insert(id);
    }
    for (size_t i=0;i<nodes.size();++i)
        if (actual[i]!=std::set<Id>(r.traits[i].begin(),r.traits[i].end())) return false;
    return true;
}
}
