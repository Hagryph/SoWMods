#pragma once
#include <array>
#include <cstdint>

namespace uruk::engine {
using P = std::uintptr_t;
struct Look {
    P base{}, head{}, pieces[23]{}, designs[15]{};
    std::uint32_t body{}, palette[17]{};
    std::uint64_t detail{}; // including appearance tags and material overrides
    bool operator==(const Look&) const = default;
};
bool Supported();
bool GenerateName(P member, std::uint32_t& id, char (&label)[256]);
P GenerateTitle(P member);
bool Appearance(P member, P role, std::uint64_t roll, Look& result, const Look* commit);
bool NameAvailable(P member, std::uint32_t id);
bool SetName(P member, std::uint32_t id);
int LevelCap(P member);
bool WeaponAllowed(P member, P item);
int KitItems(P member,P (&items)[512]);
bool WeaponLook(P member,P item,unsigned& slot,P& look);
int TraitRoll(P member, P (&pickers)[512]);
struct Advancement { P list{},reward{}; unsigned level{}; };
bool RollAdvancement(P member,Advancement& out);
// 0 = no advancement, 1 = previewed reward used, 2 = level changed under native fallback.
int Advance(P member,const Advancement& preview);
int RewardPickers(P member,P reward,bool remove,P (&out)[128]);
// -1 unavailable, 0 no failure, 1 failure; never dispatches a contract.
int ProjectTraits(P member,const P* pickers,int count,unsigned level,P rank,P (&active)[512]);
int OrderCheck(P member,P contract,unsigned level,P title,unsigned name,const P* active,int count,P& reason);
}
