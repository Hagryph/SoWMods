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
bool GenerateName(P member, std::uint32_t& id, char (&label)[256]);
P GenerateTitle(P member);
bool Appearance(P member, P role, std::uint64_t roll, Look& result, const Look* commit);
bool NameAvailable(P member, std::uint32_t id);
bool SetName(P member, std::uint32_t id);
int LevelCap(P member);
bool WeaponAllowed(P member, P item);
bool EquipmentGrantReady();
int KitItems(P member,P (&items)[512]);
bool WeaponLook(P member,P item,unsigned& slot,P& look);
struct StageLease { P army{}, actor{}, privateLook{}; unsigned index{}; std::uint64_t seed{}; };
bool PreviewStage(P member,P role,std::uint64_t roll,const Look& expected,P item,unsigned slot,StageLease& lease);
void RestoreStage(StageLease& lease);
bool StageCurrent(const StageLease& lease);
bool StageSelection(P& army,unsigned& index);
bool ReadTraitRules(P member,TraitRules& out);
struct Advancement { P list{},reward{}; unsigned level{}; };
bool RollAdvancement(P member,Advancement& out);
// 0 = no advancement, 1 = previewed reward used, 2 = level changed under native fallback.
int Advance(P member,const Advancement& preview);
int RewardPickers(P member,P reward,bool remove,P (&out)[128]);
// -1 unavailable, 0 no failure, 1 failure; never dispatches a contract.
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
int OrderCheck(P member,P contract,unsigned level,P title,unsigned name,const P* active,int count,P& reason);
}
