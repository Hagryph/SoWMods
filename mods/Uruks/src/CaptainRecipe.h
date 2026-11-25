#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

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
struct Catalog {
    std::map<Id, unsigned> categories;
    std::map<Id, std::vector<Picker>> producers;
    std::set<Id> definitions, classes, rarities;
    std::set<Id> epics;
    std::map<Id,unsigned> epicCaps;
    std::map<Id,Id> neverClass;
    std::set<Id> allSpecializations;
    std::map<Id,Id> rarityRecords;
    std::map<Id,Id> classOnlyPickers;
    std::set<Id> combatClasses;
};
struct Recipe {
    Id specialization{}, rarity{};
    std::array<std::vector<Id>,nodes.size()> traits;
};
struct Slot { Id picker{}, definition{}; };

// A dormant slot is engine-managed too. Never erase it just because one of its
// possible outputs appears on the Army screen at a different level.
inline bool retained(const Catalog& db, const std::vector<Slot>& slots, std::vector<Id>& result) {
    result.clear();
    for (auto [picker,trait]:slots) {
        if (!picker || (trait && !db.definitions.contains(trait))) return false;
        if (!db.categories.contains(trait)) result.push_back(picker);
    }
    return true;
}

// A definition can have several producers. Conflicts on one producer must not
// hide a definition that another producer can legally supply.
template<class Accept,class Extend>
bool resolve(const Catalog& db,const Recipe& recipe,Accept accept,Extend extend,unsigned* remaining=nullptr) {
    if ((recipe.specialization && !db.classes.contains(recipe.specialization)) ||
        (recipe.rarity && !db.rarities.contains(recipe.rarity))) return false;
    std::vector<const std::vector<Picker>*> rows;
    std::set<Id> distinct;
    unsigned epics=0;
    for (unsigned i=0;i<nodes.size();++i) for (auto trait:recipe.traits[i]) {
        auto group=db.categories.find(trait);
        auto options=db.producers.find(trait);
        if (group==db.categories.end() || group->second!=i || options==db.producers.end() ||
            !distinct.insert(trait).second) return false;
        rows.push_back(&options->second);
        epics+=db.epics.contains(trait);
    }
    if (auto cap=db.epicCaps.find(recipe.rarity);cap!=db.epicCaps.end() && epics>cap->second) return false;
    std::stable_sort(rows.begin(),rows.end(),[](auto a,auto b){ return a->size()<b->size(); });
    std::set<Id> groups,used,required,forbidden;
    if (recipe.specialization) required.insert(recipe.specialization);
    if (recipe.rarity) required.insert(recipe.rarity);
    std::vector<Id> picks;
    auto walk=[&](auto&& self,size_t at)->bool {
        if (remaining) {
            if (!*remaining) return false;
            --*remaining;
        }
        if (at==rows.size()) return accept(picks);
        for (const auto& p:*rows[at]) {
            if (remaining && !*remaining) return false;
            if (used.contains(p.id) || std::ranges::any_of(p.groups,[&](Id g){return groups.contains(g);}) ||
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
            if (extend(picks) && self(self,at+1)) return true;
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
inline bool matches(const Catalog& db,const Recipe& wanted,const Id* active,size_t count) {
    std::array<std::set<Id>,nodes.size()> actual;
    for (size_t i=0;i<count;++i) {
        if (!db.definitions.contains(active[i])) return false;
        if (auto it=db.categories.find(active[i]);it!=db.categories.end()) actual[it->second].insert(active[i]);
    }
    for (unsigned i=0;i<nodes.size();++i)
        if (actual[i]!=std::set<Id>(wanted.traits[i].begin(),wanted.traits[i].end())) return false;
    return true;
}
}
