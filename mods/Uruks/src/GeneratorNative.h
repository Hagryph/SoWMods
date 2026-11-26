#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "TraitRules.h"
#include "CaptainRecipe.h"

namespace uruk::engine {
using P = std::uintptr_t;
namespace detail {
// A matching generation tree asks for the class ceiling, not Talion's follower ceiling.
template<class Query>
unsigned queryTreeCap(Query query,P tree,bool follower,bool* capped,P generationTree) {
    return query(tree,follower && (!generationTree || tree!=generationTree),capped);
}
}
struct Look {
    P base{}, head{}, pieces[23]{}, designs[15]{};
    std::uint32_t body{}, palette[17]{};
    std::uint64_t detail{}; // including appearance tags and material overrides
    bool operator==(const Look&) const = default;
};
bool Supported();
// Native per-member trait visibility, independent of whether its picker was granted.
std::optional<bool> ArmyTraitVisible(P member,P trait);
bool TribeAvailable(P tribe);
bool ScopeClassDraws(recipe::Catalog&,const recipe::Recipe&,P army);
int ProjectTraits(P member,const P* pickers,int count,unsigned level,P rank,P (&active)[512],P* byPicker=nullptr);
struct GenerationPlan {
    P army{},row{},originalSeed{},rank{};
    unsigned level{},generationLevel{},index{};
    P role{},roleSeed{},personality{};
    recipe::Loadout weapons;
    recipe::Encounter encounter;
    recipe::Recipe requested;
    std::vector<P> grants,exclude;
    std::set<P> requiredMarkers,forbiddenMarkers,automatic,blockedClassRewards;
};
bool PlanGeneration(P target,P rank,const recipe::Catalog&,const recipe::Recipe&,GenerationPlan&,std::string& error,uint64_t session);
enum class ReplaceResult { Rejected, Replaced, ChangedUnexpectedly, SeatUnavailable, CleanupFailed, RollbackFailed };
using WritePickers=bool(*)(P,const std::vector<P>&);
ReplaceResult ReplaceCaptain(P target,const GenerationPlan&,const recipe::Catalog&,const recipe::Recipe&,WritePickers,std::string& detail);
}
