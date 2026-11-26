#include "GeneratorNative.h"
#include "MemberCopy.h"
#include "RetainedTraits.h"
#include "GameOffsets.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <MinHook.h>

namespace uruk::engine {
namespace {
template<class T> T& at(P p) { return *reinterpret_cast<T*>(p); }
template<class R, class... A> R call(P va, A... args) {
    return reinterpret_cast<R(__fastcall*)(A...)>(game::FromRVA(va))(args...);
}
P faction() { return at<P>(at<P>(game::FromRVA(game::kGameSystemsSingleton))+0x6c68); }
using CampaignTest=bool(*)(P);
CampaignTest nativeCampaignTest=nullptr;
thread_local const std::set<P>* generationPredicates=nullptr;
bool campaignTest(P predicate){
    if(generationPredicates && generationPredicates->contains(predicate))return true;
    return nativeCampaignTest(predicate);
}
bool installCampaignTest(){
    if(nativeCampaignTest)return true;
    const P addr=game::FromRVA(0x1401e1118);
    if(at<uint64_t>(addr)!=0x74894808245c8948ULL)return false;
    auto target=reinterpret_cast<void*>(addr);
    if(MH_CreateHook(target,reinterpret_cast<void*>(&campaignTest),reinterpret_cast<void**>(&nativeCampaignTest))!=MH_OK)return false;
    if(MH_EnableHook(target)==MH_OK)return true;
    MH_RemoveHook(target);nativeCampaignTest=nullptr;return false;
}
// Quest progress is not modified. Only generator-owned evaluations on this
// thread can waive a typed predicate referenced by the Uruk catalog.
template<class Work> bool withCampaignRules(const recipe::Catalog& db,Work work){
    if(db.campaignPredicates.empty())return work();
    if(!installCampaignTest())return false;
    auto previous=generationPredicates;
    __try {generationPredicates=&db.campaignPredicates;return work();}
    __finally {generationPredicates=previous;}
}
P army(P m) { return call<P>(0x14018de04, faction()+0x98, at<P>(m+0x178)); }
struct alignas(16) Context { unsigned char data[0x260]{}; P ptr() { return reinterpret_cast<P>(data); } };
bool inspect(P p,Look& out) {
    out.body=at<unsigned>(p);
    out.base=at<P>(p+0x20); out.head=at<P>(p+0x170);
    std::memcpy(out.pieces,(void*)(p+0x28),sizeof out.pieces);
    std::memcpy(out.designs,(void*)(p+0xe0),sizeof out.designs);
    std::memcpy(out.palette,(void*)(p+0x178),sizeof out.palette);
    out.detail=1469598103934665603ULL;
    for (int offset : {8,0x158}) {
        P b=at<P>(p+offset), e=at<P>(p+offset+8);
        if (e<b || e-b>32768 || (e-b)%(offset==8?16:8)) return false;
        for (; b<e; b+=offset==8?16:8) {
            out.detail=(out.detail^at<P>(b))*1099511628211ULL;
            if (offset==8) out.detail=(out.detail^at<unsigned>(b+8))*1099511628211ULL;
        }
    }
    return out.base && out.head && out.pieces[0] && out.pieces[1];
}
}
static bool VerifyBuild() {
    __try {
        static constexpr struct { P va; std::uint64_t prefix; } entries[]={
#include "GeneratorSignatures.inl"
        };
        for (auto e:entries) if (std::memcmp(reinterpret_cast<const void*>(game::FromRVA(e.va)),&e.prefix,8)) return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Supported() { static const bool verified=VerifyBuild(); return verified; }
std::optional<bool> ArmyTraitVisible(P member,P trait) {
    __try {
        constexpr P fn=0x141bb9cd4;
        if (member && trait && Supported() && at<uint64_t>(game::FromRVA(fn))==0x83485708245c8948ULL)
            return call<bool>(fn,member,trait);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return std::nullopt;
}
bool TribeAvailable(P tribe) {
    __try { return tribe && Supported() && call<bool>(0x141bb8bb0,tribe); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace {struct Iter { unsigned char data[40]; unsigned index,count; };}
bool WeaponAllowed(P m,P item) {
    __try {
        P type=call<P>(0x14011980c,at<P>(m+0x188));
        return type && call<bool>(0x14030cf30,game::FromRVA(0x14068c8f4),game::FromRVA(0x14068c940),item,type)
          && !call<bool>(0x14036686c,game::FromRVA(0x140690734),game::FromRVA(0x140690780),item,type);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace {
// These are the read-only iterators and predicates used before the weighted
// choices in 141bb876c / 141bb84e0. No sampler or live picker slot is consulted.
template<class F> void eachRecord(P owner,P init,P get,F visit) {
    if (!owner) return;
    Iter it{};
    call<void>(init,&it,owner);
    if (it.count>8192) throw std::runtime_error("Invalid picker list");
    for (;it.index<it.count;++it.index) {
        P scratch{};
        P record=get==0x141156ac0 || get==0x141150460 ?
            call<P>(get,&it,it.index,&scratch):call<P>(get,&it);
        if (record) visit(record);
    }
}
int maxQuantity(P row) {
    int def=1;
    const int lo=call<bool>(0x141bbd6c8)?call<int>(0x140278758,row,call<unsigned char>(0x141bbe880),0,&def)
        :call<int>(0x141169594,row,call<P>(0x141bbb3d8),0,&def);
    const int hi=call<int>(0x141bbcda8,row);
    // Native bounded RNG is [0, hi-lo); a fixed quantity does not call it.
    return hi>lo?hi-1:lo;
}
template<class Eligible>
void addDrawList(P list,P markers,TraitRules& out,Eligible eligible) {
    int cap=0;
    eachRecord(list,0x141bb63b4,0x141156ac0,[&](P q){
        if (call<bool>(0x141bb9e94,q,markers) && call<float>(0x141bbe34c,q)>0)
            cap=(std::max)(cap,maxQuantity(q));
    });
    if (cap<=0) return;
    TraitSource source{unsigned(cap),{}};
    eachRecord(list,0x141bb5634,0x141bb705c,[&](P row){
        if (!call<bool>(0x141bb9da4,row,markers) || !(call<float>(0x141bbe2f8,row)>0)) return;
        std::set<P> entries;
        eachRecord(row,0x141bb5ff4,0x141bb71ac,[&](P p){if (eligible(p)) entries.insert(p);});
        if (!entries.empty()) source.tables.push_back(std::move(entries));
    });
    if (!source.tables.empty()) out.sources.push_back(std::move(source));
}
bool scopeDraws(recipe::Catalog& db,const recipe::Recipe& r,P armyRecord) {
    if (!Supported() || !armyRecord || !r.complete() || !recipe::fitsClass(db,r.base,r.specialization)) return false;
    P ids[]{r.base,r.specialization,r.rarity};
    struct {P* begin;P* end;P* capacity;} span{ids,ids+3,ids+3};
    const P markers=reinterpret_cast<P>(&span);
    auto has=[&](P id){return std::ranges::find(ids,id)!=std::end(ids);};
    P groups[65]{};
    TraitRules draws;
    auto allowed=[&](P p){return db.routes.contains(p) && call<bool>(0x1407d4890,p,markers,groups);};
    auto draw=[&](P p){
        if (!allowed(p) || db.managedPickers.contains(p)) return false;
        draws.groups.emplace(p,db.routes.at(p).groups);
        return true;
    };
    std::set<P> lists,direct;
    std::set<std::set<P>> alternatives;
    bool first=true;
    for (const auto& kit:db.kits.at(r.specialization)) {
        if (!kit.fits(db.types.at(r.base))) continue;
        std::set<P> singles;
        for (const auto& group:kit.grants) if (group.size()==1 && allowed(group.front())) singles.insert(group.front());
        if (first) {db.fixedClassInputs=singles;first=false;}
        else std::erase_if(db.fixedClassInputs,[&](P id){return !singles.contains(id);});
        if (std::ranges::any_of(kit.forbiddenMarkers,has) ||
            !std::ranges::all_of(kit.requiredAny,[&](const auto& req){return std::ranges::any_of(req,has);})) continue;
        direct.insert(singles.begin(),singles.end());
        lists.insert(kit.drawLists.begin(),kit.drawLists.end());
        for (const auto& group:kit.grants) if (group.size()>1) {
            std::set<P> pool;
            for (auto id:group) if (draw(id)) pool.insert(id);
            if (!pool.empty()) alternatives.insert(std::move(pool));
        }
    }
    direct.insert(db.fixedClassInputs.begin(),db.fixedClassInputs.end());
    recipe::addClassRewardDraws(db,r,draws,[&](P p){return !direct.contains(p) && draw(p);});
    for (const auto& pool:alternatives) draws.sources.push_back({1,{pool}});
    eachRecord(call<P>(0x141bbedc4,armyRecord),0x141bb66d4,0x141bb7318,[&](P list){lists.insert(list);});
    for (auto list:lists) addDrawList(list,markers,draws,draw);
    db.ordinaryDraws=draws;
    // The same army/tree role tables used by the ordinary captain generator.
    // Keep all grants from one role in ONE alternative, never one source per role.
    TraitSource roleSource{512,{}};
    Context check{};
    at<P>(check.ptr()+0x148)=db.types.at(r.base);
    at<P>(check.ptr()+0x150)=r.tribe;
    P config=call<P>(0x140684dc8,armyRecord,db.trees.at(r.base));
    if (config) eachRecord(config,0x141bd73bc,0x141bd75ac,[&](P list){
        eachRecord(list,0x140350d84,0x140350d68,[&](P role){
            auto it=db.roles.find(role);
            if (it==db.roles.end() || db.roleLists.contains(role)) return;
            const auto& kit=it->second;
            if (auto never=db.neverClass.find(r.specialization);never!=db.neverClass.end() &&
                std::ranges::find(kit.markers,never->second)!=kit.markers.end()) return;
            float weight=1;
            if (!call<bool>(0x141bdb4f0,check.ptr(),role,&weight)) return;
            std::set<P> options;
            for (P id:kit.grants) if (db.routes.contains(id) && !db.managedPickers.contains(id)) {
                std::set<P> required,forbidden;
                if (!recipe::markerInputs(db,r,{id},required,forbidden)) continue;
                options.insert(id);
                draws.groups.emplace(id,db.routes.at(id).groups);
            }
            db.roleLists.emplace(role,list);
            if (!options.empty()) {
                roleSource.tables.push_back(std::move(options));
            }
        });
    });
    if (!roleSource.tables.empty()) draws.sources.push_back(std::move(roleSource));
    TraitSource rewards{512,{}};
    for (const auto& e:db.encounters) if (recipe::fitsEncounter(db,r,e)) {
        std::set<P> table;
        for (P id:e.grants) if (db.routes.contains(id) && !db.managedPickers.contains(id)) {
            draws.groups.emplace(id,db.routes.at(id).groups);table.insert(id);
        }
        if (!table.empty()) rewards.tables.push_back(std::move(table));
    }
    if (!rewards.tables.empty()) draws.sources.push_back(std::move(rewards));
    recipe::classifyClassDraws(db,std::move(draws),direct);
    return true;
}
}
bool ScopeClassDraws(recipe::Catalog& db,const recipe::Recipe& r,P armyRecord) {
    __try {return withCampaignRules(db,[&]{return scopeDraws(db,r,armyRecord);});}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
namespace {
bool rewardEligible(P m,P list,P reward) {
    Iter it{};
    call<void>(0x140350f80,&it,list);
    if (it.count>4096) return false;
    for (;it.index<it.count;++it.index) if (call<P>(0x1403505d8,&it)==reward) {
        if (!call<bool>(0x140105aec,m,reward)) return false;
        float w=call<float>(0x1403505f4,m,reward);
        return std::isfinite(w) && w>0;
    }
    return false;
}
}
namespace {
struct HateGateScope {
    const recipe::Catalog& db;
    const recipe::Recipe& recipe;
    P memberTraits{}, projectedTraits{};
};
thread_local HateGateScope* hateScope=nullptr;
using AnyTraitFn=bool(*)(P,P);
AnyTraitFn nativeAnyTrait=nullptr;
bool anyTraitForRecipe(P entry,P ctx) {
    if (retained::WeaknessWaived(entry,ctx)) return true;
    auto* scope=hateScope;
    if (scope && ctx) {
        const P traits=at<P>(ctx);
        if (traits && (traits==scope->memberTraits || traits==scope->projectedTraits) &&
            recipe::waiveWeaknessGate(scope->db,scope->recipe,entry)) return true;
    }
    return nativeAnyTrait(entry,ctx);
}
}
bool InstallAnyTraitPredicate() {
    if (!Supported()) return false;
    if (!nativeAnyTrait) {
        const P address=game::FromRVA(0x1403532bc);
        if (at<std::uint64_t>(address)!=0x4808588948c48b48ULL) return false;
        void* target=reinterpret_cast<void*>(address);
        if (MH_CreateHook(target,reinterpret_cast<void*>(&anyTraitForRecipe),reinterpret_cast<void**>(&nativeAnyTrait))!=MH_OK)
            return false;
        if (MH_EnableHook(target)!=MH_OK) {MH_RemoveHook(target);nativeAnyTrait=nullptr;return false;}
    }
    return true;
}
namespace {
bool beginHateScope(HateGateScope& scope) {
    if (!std::ranges::any_of(scope.db.weaknessGatedHateEntries,[&](const auto& pair){
        return recipe::waiveWeaknessGate(scope.db,scope.recipe,pair.first);
    })) {hateScope=nullptr;return true;}
    if (!InstallAnyTraitPredicate()) return false;
    hateScope=&scope;
    return true;
}
P projectSlot(P slot,unsigned short* mask,P ctx) {
    if (!hateScope) return call<P>(0x1403528c4,slot,mask,ctx);
    const P previous=hateScope->projectedTraits;
    __try {
        hateScope->projectedTraits=at<P>(ctx);
        return call<P>(0x1403528c4,slot,mask,ctx);
    } __finally {hateScope->projectedTraits=previous;}
}
}
int ProjectTraits(P m,const P* pickers,int count,unsigned level,P rank,P (&active)[512],P* byPicker) {
    struct Slot { P picker,trait,mission; unsigned short mask; short option; unsigned pad; };
    struct Span { P* begin; P* end; } span{active,active};
    struct Inputs { Span* traits; P remap; unsigned level,intel; P rank; bool follower; };
    if (count<0 || count>512 || (count && !pickers) || !rank) return -1;
    __try {
        Slot slots[512]{};
        for (int i=0;i<count;++i) {
            call<void>(0x1407cd150,&slots[i],pickers[i]);
            P b=at<P>(m+0x1e0),e=at<P>(m+0x1e8);
            if (e<b || e-b>32768) return -1;
            for (;b<e;b+=32) if (at<P>(b)==pickers[i]) { slots[i].mission=at<P>(b+16); break; }
        }
        std::stable_sort(slots,slots+count,[](const Slot& a,const Slot& b){
            return call<int>(0x1407127f8,a.picker)>call<int>(0x1407127f8,b.picker);
        });
        Inputs ctx{&span,call<P>(0x141156510,faction(),at<P>(m+0x280)),level,at<unsigned char>(m+0x51a),rank,
            !!(at<std::uint64_t>(m+0x508)&(1ull<<27))};
        for (int pass=0;pass<2;++pass) {
            unsigned short mask=pass?16:1;
            for (int i=0;i<count;++i) {
                P old=slots[i].trait;
                P trait=projectSlot(reinterpret_cast<P>(&slots[i]),&mask,reinterpret_cast<P>(&ctx));
                if (!pass || old!=trait) {
                    if (old) {
                        auto at=std::find(span.begin,span.end,old);
                        if (at!=span.end) span.end=std::move(at+1,span.end,at);
                    }
                    if (trait && std::find(span.begin,span.end,trait)==span.end) *span.end++=trait;
                    mask|=16;
                }
                if (byPicker && pass) {
                    auto source=std::find(pickers,pickers+count,slots[i].picker);
                    if (source==pickers+count) return -1;
                    byPicker[source-pickers]=trait;
                }
            }
        }
        return int(span.end-span.begin);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
namespace {
struct BuildParams {
    alignas(16) unsigned char data[400]{};
    P ptr() {return reinterpret_cast<P>(data);}
    BuildParams() {call<P>(0x141c62054,ptr());}
    ~BuildParams() {call<void>(0x141c628d0,ptr());}
};
// Present one real role to the native eligibility/weight evaluator. The source
// field descriptor is copied, not the GameDB record mutated. The accumulated
// weight is the eligibility result; the random selection is immaterial.
bool roleHasWeight(P ctx,P list,P role) {
    if (!list || at<unsigned char>(list)!=1 || (at<unsigned char>(list+1)&16)) return false;
    alignas(8) unsigned char header[40],field[16];
    P record=reinterpret_cast<P>(header),f=reinterpret_cast<P>(field);
    std::memcpy(header,(void*)list,sizeof header);
    std::memcpy(field,(void*)at<P>(list+8),sizeof field);
    if (at<unsigned>(f)!=0x0c7a758d || (at<unsigned char>(f+10)&63)!=10) return false;
    at<unsigned short>(f+12)=1;at<unsigned short>(f+14)=0;
    at<P>(record+8)=f;at<P>(record+16)=reinterpret_cast<P>(&role);
    P localSeed=1;
    struct Weight {float total{};unsigned pad{};P seed;} weight{0,0,reinterpret_cast<P>(&localSeed)};
    call<P>(0x141bd8144,ctx,record,&weight,P(0));
    return std::isfinite(weight.total) && weight.total>0;
}
bool roleWitness(P target,P tree,P list,P role,const GenerationPlan& p) {
    // Reconstruct only the pre-role model inputs of the normal builder. No
    // faction member, inventory, or campaign seat is allocated or modified.
    alignas(16) unsigned char scratch[0x530]{};
    P m=reinterpret_cast<P>(scratch),model=0;
    at<P>(m+0x168)=p.roleSeed;at<P>(m+0x188)=tree;
    at<P>(m+0x270)=p.personality;at<P>(m+0x278)=call<P>(0x14057710c,tree);
    call<void>(0x1402eb490,&model);
    if (!model) return false;
    bool ok=false;
    __try {
        Context params;
        call<void>(0x140577190,params.ptr());
        __try {
            call<void>(0x141bea190,params.ptr(),m);
            call<void>(0x1404c5194,model,params.ptr());
        } __finally {call<void>(0x141821734,params.ptr());}
        Context c;
        call<P>(0x141bd6a48,c.ptr(),army(target),p.army,model,tree,p.personality,
            call<float>(0x1404e3e5c,p.roleSeed),1,p.requested.tribe,P(0),p.row,P(0));
        __try {
            ok=roleHasWeight(c.ptr(),list,role);
            // The ordinary role generator retries without tribe preference;
            // this does not change the tribe passed to the captain builder.
            if (!ok) {at<P>(c.ptr()+0x150)=0;ok=roleHasWeight(c.ptr(),list,role);}
        } __finally {call<void>(0x141b59d2c,c.ptr());}
    } __finally {call<void>(0x1402ed9d4,&model);}
    return ok;
}
bool records(P vector,unsigned stride,std::vector<P>& out) {
    P b=at<P>(vector),e=at<P>(vector+8);
    if (e<b || (e-b)%stride || (e-b)/stride>512) return false;
    out.clear();
    for (;b!=e;b+=stride) out.push_back(at<P>(b));
    return true;
}
bool permitsGrants(P m,const std::vector<P>& picks) {
    // Same marker and exclusive-group checks as the authored grant at
    // 141bc6e04. Natural table membership and weight are not grant criteria.
    struct { unsigned count{},pad{}; P ids[64]{}; } held;
    for (auto p:picks) {
        if (!call<bool>(0x1407d4890,p,m+0x1b0,&held)) return false;
        bool overflow=false;
        eachRecord(p,0x14019f734,0x14019f814,[&](P g){
            if (std::find(held.ids,held.ids+held.count,g)!=held.ids+held.count) return;
            if (held.count==64) overflow=true; else held.ids[held.count++]=g;
        });
        if (overflow) return false;
    }
    return true;
}
P equippedRecord(P inv,unsigned slot) {
    const P instance=at<P>(inv+8*slot);
    return instance?at<P>(instance+0x38):0;
}
bool equipmentMatches(P m,const recipe::Loadout& locked) {
    for (auto [slot,item]:locked) if (slot>=10 || equippedRecord(m+0x50,slot)!=item) return false;
    return true;
}
struct GearScope {
    P member;const recipe::Catalog& db;const recipe::Loadout& locked;
    bool populated=false;
    bool owns(P inv,P m) const {return member==m && inv==m+0x50;}
};
thread_local GearScope* gearScope=nullptr;
using PopulateFn=P(*)(P,P,P);
using GiveFn=bool(*)(P,P,P,unsigned char,P);
PopulateFn nativePopulate=nullptr;
GiveFn nativeGive=nullptr;
bool giveInFreeSlot(P inv,P m,P item,unsigned char source,P look) {
    if (auto* s=gearScope;s && s->owns(inv,m)) {
        if (!recipe::permitsGeneratedWeapon(s->db,s->locked,item)) return false;
        for (auto [slot,id]:s->locked)
            if (id && id==item && equippedRecord(inv,slot)==id) return true;
    }
    return nativeGive(inv,m,item,source,look);
}
P populateWithReservations(P inv,P m,P look) {
    if (auto* s=gearScope;s && s->owns(inv,m)) {
        // Native inventory reset has just finished; no kit has rolled equipment yet.
        for (auto [slot,item]:s->locked) if (item && equippedRecord(inv,slot)!=item)
            nativeGive(inv,m,item,8,look);
        s->populated=equipmentMatches(m,s->locked);
    }
    return nativePopulate(inv,m,look);
}
bool installGearLocks() {
    if (nativePopulate && nativeGive) return true;
    const P a=game::FromRVA(0x141c10ab0),b=game::FromRVA(game::kFactionMemberGiveItem);
    if (at<uint64_t>(a)!=0x4808588948c48b48ULL || at<uint64_t>(b)!=0x4408588948c48b48ULL) return false;
    auto pop=reinterpret_cast<void*>(a),give=reinterpret_cast<void*>(b);
    if (MH_CreateHook(pop,reinterpret_cast<void*>(&populateWithReservations),reinterpret_cast<void**>(&nativePopulate))!=MH_OK) return false;
    if (MH_CreateHook(give,reinterpret_cast<void*>(&giveInFreeSlot),reinterpret_cast<void**>(&nativeGive))==MH_OK) {
        if (MH_EnableHook(give)==MH_OK && MH_EnableHook(pop)==MH_OK) return true;
        MH_RemoveHook(give);
    }
    MH_RemoveHook(pop);nativeGive=nullptr;nativePopulate=nullptr;return false;
}
constexpr P treeCapAddress=0x1403517e0;
using TreeCapFn=unsigned(*)(P,bool,bool*);
TreeCapFn originalTreeCap=nullptr;
thread_local P generatingTree=0;
using RewardPredicate=bool(*)(P,P);
RewardPredicate originalEligibility=nullptr;
thread_local P rewardMember=0;
thread_local const std::set<P>* deniedRewards=nullptr;
bool eligibleDuringBuild(P member,P reward) {
    return !(member==rewardMember && deniedRewards && deniedRewards->contains(reward)) &&
        originalEligibility(member,reward);
}
bool installRewardGate() {
    if (originalEligibility) return true;
    auto target=reinterpret_cast<void*>(game::FromRVA(0x140105aec));
    if (MH_CreateHook(target,reinterpret_cast<void*>(&eligibleDuringBuild),
        reinterpret_cast<void**>(&originalEligibility))!=MH_OK) return false;
    if (MH_EnableHook(target)==MH_OK) return true;
    MH_RemoveHook(target);originalEligibility=nullptr;return false;
}
unsigned treeCapDuringBuild(P tree,bool follower,bool* capped) {
    return detail::queryTreeCap(originalTreeCap,tree,follower,capped,generatingTree);
}
bool installBuildCap() {
    if (originalTreeCap) return true;
    const P address=game::FromRVA(treeCapAddress);
    if (at<std::uint64_t>(address)!=0x83485708245c8948ULL) return false;
    void* target=reinterpret_cast<void*>(address);
    if (MH_CreateHook(target,reinterpret_cast<void*>(&treeCapDuringBuild),reinterpret_cast<void**>(&originalTreeCap))!=MH_OK)
        return false;
    if (MH_EnableHook(target)==MH_OK) return true;
    MH_RemoveHook(target);originalTreeCap=nullptr;return false;
}
void buildAtClassCap(P member,P ctx,P params,P row,P tree,const std::set<P>& blocked) {
    const P previous=generatingTree;
    const P oldMember=rewardMember;
    auto* oldDenied=deniedRewards;
    __try {
        generatingTree=tree;rewardMember=member;deniedRewards=&blocked;
        // Fresh-member builder used inside the native same-seat replacement,
        // without its destruction of the old captain and command relationships.
        call<void>(0x141bbf928,member,ctx,params,row,P(0));
    } __finally {generatingTree=previous;rewardMember=oldMember;deniedRewards=oldDenied;}
}
bool acquisitionTraits(P m,const std::vector<P>& inputs,const GenerationPlan& p,const recipe::Catalog& db,
                       std::vector<P>& result,recipe::PositionOutputs& overrides) {
    overrides.clear();
    P active[512]{},perSlot[512]{};
    int n=ProjectTraits(m,inputs.data(),int(inputs.size()),p.generationLevel,p.rank,active,perSlot);
    if(n<0)return false;
    result.assign(active,active+n);
    std::map<P,std::set<P>> pending;
    for(const auto& group:p.requested.traits)for(P id:group){
        if(std::ranges::find(result,id)!=result.end())continue;
        auto producers=db.producers.find(id);
        if(producers!=db.producers.end())for(const auto& src:producers->second)
            if(db.positionPickers.contains(src.id) && std::ranges::find(inputs,src.id)!=inputs.end())
                pending[src.id].insert(id);
    }
    if(pending.empty())return true;
    std::vector<recipe::Slot> slots;
    for(size_t i=0;i<inputs.size();++i)slots.push_back({inputs[i],perSlot[i]});
    for(P rank:db.ranks)if(rank!=p.rank){
        if(ProjectTraits(m,inputs.data(),int(inputs.size()),p.generationLevel,rank,active,perSlot)<0)return false;
        for(size_t i=0;i<inputs.size();++i)if(auto it=pending.find(inputs[i]);it!=pending.end() && it->second.contains(perSlot[i])){
            overrides.emplace(inputs[i],recipe::PositionOutput{slots[i].definition,perSlot[i]});pending.erase(it);
        }
        if(pending.empty())break;
    }
    return recipe::accountPositions(overrides,slots,result);
}
bool ownedOptionalResult(P m,const recipe::Catalog& db,const GenerationPlan& p,const std::vector<P>& active){
    if(p.positionOutputs.empty())return recipe::optionalResult(db,p.requested,active,p.automatic);
    std::vector<P> owned;
    if(!records(m+0x1e0,32,owned))return false;
    std::vector<recipe::Slot> slots;
    P begin=at<P>(m+0x1e0);
    for(size_t i=0;i<owned.size();++i)slots.push_back({owned[i],at<P>(begin+32*i+8)});
    auto accounted=active;
    return recipe::accountPositions(p.positionOutputs,slots,accounted) &&
        recipe::optionalResult(db,p.requested,accounted,p.automatic);
}
bool makePlan(P target,P rank,const recipe::Catalog& db,const recipe::Recipe& r,GenerationPlan& p,std::string& error,uint64_t session) {
    if (!Supported() || !target || !rank || !r.complete() || !TribeAvailable(r.tribe)) {
        error="The selected captain or kit is unavailable."; return false;
    }
    p={}; p.army=at<P>(target+0x178); p.row=at<P>(target+0x268); p.index=at<unsigned>(target+0x174);
    p.originalSeed=at<P>(target+0x168);p.level=at<unsigned>(target+0x4b8);p.rank=rank;p.requested=r;
    if (!p.row || !p.army || p.level>84 || !db.rarityRecords.contains(r.rarity) ||
        !db.trees.contains(r.base)) {error="The selected Army seat or level is unavailable.";return false;}
    bool capped=false;
    auto flags=at<std::uint64_t>(target+0x508);
    const P tree=db.trees.at(r.base);
    unsigned cap=detail::queryTreeCap(reinterpret_cast<TreeCapFn>(game::FromRVA(treeCapAddress)),tree,true,&capped,tree);
    if (p.level>cap) {error="The selected class cannot retain this level.";return false;}
    p.generationLevel=(std::min)(cap,84u);
    // Evaluate the same kit and level that the native builder will receive.
    alignas(16) unsigned char input[0x530]{};
    P m=reinterpret_cast<P>(input);
    at<unsigned char>(m+0x51a)=at<unsigned char>(target+0x51a);
    at<P>(m+0x188)=tree;
    at<std::uint64_t>(m+0x508)=flags&(1ull<<27);
    auto fixed=recipe::coreInputs(db,r);
    P active[512]{};
    int n=ProjectTraits(m,fixed.data(),int(fixed.size()),p.generationLevel,rank,active);
    if(n<0){error="Could not resolve the automatic class kit.";return false;}
    const std::set<P> automatic(active,active+n);
    p.automatic=automatic;
    unsigned budget=8192;
    bool ok=recipe::resolve(db,r,[&](const auto& picks,const auto& classPicks){
      auto expand=[&](const std::vector<P>& chain) {
       auto source=[&](const recipe::Encounter* origin) {
        const auto* e=origin;
        auto authored=chain;
        if (e) {
            if (!recipe::fitsEncounter(db,r,*e) || std::ranges::none_of(e->grants,[&](P id){
                return std::ranges::find(chain,id)!=chain.end();})) return false;
            for (P id:e->grants) if (std::ranges::find(authored,id)==authored.end()) authored.push_back(id);
        }
        if (!budget) return false;
        --budget;
        auto chooseRole=[&](const std::vector<P>& expanded) {
        return recipe::withRequiredRole(db,r,e,fixed,picks,classPicks,expanded,[&](P role){
        auto bundle=fixed;
        if (role) for (P id:db.roles.at(role).grants) {
            std::set<P> need,deny;
            if (!db.routes.contains(id) || !recipe::markerInputs(db,r,{id},need,deny)) continue;
            if (std::ranges::find(bundle,id)==bundle.end()) {
                if (!recipe::fitsCore(db,bundle,{id})) return false;
                bundle.push_back(id);
            }
        }
        if (!recipe::fitsCore(db,bundle,expanded) || !recipe::fitsCore(db,expanded,expanded)) return false;
        auto requiredOutputs=automatic;
        if (role) {
            int size=ProjectTraits(m,bundle.data(),int(bundle.size()),p.generationLevel,rank,active);
            if (size<0) return false;
            requiredOutputs.insert(active,active+size);
            for (const auto& row:r.traits) for (auto trait:row) requiredOutputs.erase(trait);
        }
        auto all=bundle;
        for (P id:expanded) if (std::ranges::find(all,id)==all.end()) all.push_back(id);
        auto accept=[&](const std::vector<P>& inputs){
            if (!recipe::markerInputs(db,r,inputs,p.requiredMarkers,p.forbiddenMarkers)) return false;
            if (e) {
                for (P id:e->removes) if (std::ranges::find(inputs,id)!=inputs.end()) return false;
                for (P id:e->blocksPicker) if (std::ranges::find(inputs,id)!=inputs.end() &&
                    std::ranges::find(e->grants,id)==e->grants.end()) return false;
                p.requiredMarkers.insert(origin->markers.begin(),origin->markers.end());
                auto markers=p.requiredMarkers;
                if (role) markers.insert(db.roles.at(role).markers.begin(),db.roles.at(role).markers.end());
                // A condition such as a story/rank marker is not a grant. Do not manufacture it.
                for (P id:e->markers) if (!markers.contains(id)) return false;
                p.forbiddenMarkers.insert(e->blocksMarker.begin(),e->blocksMarker.end());
                if (!recipe::compatibleMarkers(db,markers) || std::ranges::any_of(markers,[&](P id){
                    return p.forbiddenMarkers.contains(id);})) return false;
            }
            if (role) for (P marker:db.roles.at(role).markers) if (p.forbiddenMarkers.contains(marker) ||
                (db.allSpecializations.contains(marker) && marker!=r.specialization) ||
                (db.combatClasses.contains(marker) && marker!=r.base) ||
                (db.rarities.contains(marker) && marker!=r.rarity)) return false;
            auto protectedMarkers=p.requiredMarkers;
            if (role) protectedMarkers.insert(db.roles.at(role).markers.begin(),db.roles.at(role).markers.end());
            if (!recipe::classRewardPool(db,r,protectedMarkers,p.forbiddenMarkers,p.blockedClassRewards)) return false;
            std::vector<P> projected;
            if(!acquisitionTraits(m,inputs,p,db,projected,p.positionOutputs) ||
               !recipe::matches(db,r,projected.data(),projected.size(),requiredOutputs) ||
               std::ranges::count_if(projected,[&](P id){return db.epics.contains(id);})>db.epicCaps.at(r.rarity)) return false;
            if (!recipe::planWeapons(db,projected.data(),projected.size(),role,[&](P id){return WeaponAllowed(m,id);},p.weapons)) return false;
            if (role) {
                if (!p.personality) {
                    // All option checks and the staged generation must validate the same native identity.
                    const std::array<P,9> key{session,target,p.army,p.originalSeed,tree,r.tribe,r.specialization,r.rarity,p.row};
                    thread_local std::array<P,9> last{};
                    thread_local P seed{},personality{};
                    if (last!=key || !personality) {
                        BuildParams sample;
                        seed=at<P>(sample.ptr()+0x170);
                        personality=call<P>(0x141bda7ac,p.army,tree,sample.ptr()+0xf8,sample.ptr()+0x110);
                        last=key;
                    }
                    p.roleSeed=seed;p.personality=personality;
                }
                if (!p.personality || !roleWitness(target,tree,db.roleLists.at(role),role,p)) return false;
            }
            p.role=role;p.grants=inputs;p.automatic=requiredOutputs;p.encounter=origin?*origin:recipe::Encounter{};return true;
        };
        auto finish=[&](const std::vector<P>& inputs) {
            // A prerequisite introduced by the role's own kit cannot borrow another role or reward.
            for (P id:inputs) if (std::ranges::find(expanded,id)==expanded.end() &&
                std::ranges::find(bundle,id)==bundle.end() && (db.encounterPickers.contains(id) || db.rolePickers.contains(id)) &&
                !db.ordinaryDraws.permits({id})) return false;
            for (P id:inputs) if (auto it=db.routes.find(id);it!=db.routes.end()) for (P mark:it->second.required)
                if (db.roleMarkers.contains(mark) && (!role ||
                    std::ranges::find(db.roles.at(role).markers,mark)==db.roles.at(role).markers.end())) return false;
            return accept(inputs);
        };
        return recipe::withHiddenGrants(db,all,budget,finish);
        });
        };
        if (!origin) return chooseRole(authored);
        return recipe::withRewardRequirements(*origin,authored,budget,[&](const auto& resolved,auto& grants){
            e=&resolved;
            return recipe::withHiddenGrants(db,grants,budget,chooseRole);
        });
       };
       if (source(nullptr)) return true;
       for (const auto& e:db.encounters) if (source(&e)) return true;
       return false;
      };
      auto roots=picks;
      return recipe::withHiddenGrants(db,roots,budget,expand);
    },[&](const auto& picks){return recipe::fitsCore(db,fixed,picks);},&budget,nullptr,
      unsigned(std::ranges::count_if(automatic,[&](P v){return db.epics.contains(v);})));
    if (!ok) {error="The requested traits or their prerequisites are incompatible with this kit.";return false;}
    p.exclude=recipe::exclusions(db,p.grants);
    return true;
}
ReplaceResult replace(P original,P m,const GenerationPlan& p,const recipe::Catalog& db,const recipe::Recipe& r,WritePickers write) {
    if (!original || !m || original==m) return ReplaceResult::Rejected;
    if (!Supported() || !m || !write || p.requested!=r || !r.complete() || !db.trees.contains(r.base) ||
        !db.rarityRecords.contains(r.rarity) || !TribeAvailable(r.tribe) || !p.rank ||
        at<P>(original+0x168)!=p.originalSeed || at<unsigned>(original+0x4b8)!=p.level) return ReplaceResult::Rejected;
    const P ctx=army(original);
    if (!ctx) return ReplaceResult::Rejected;
    const auto intel=at<unsigned char>(original+0x51a), column=at<unsigned char>(original+0x51b);
    const bool follower=!!(at<std::uint64_t>(original+0x508)&(1ull<<27));
    BuildParams params;
    const P b=params.ptr();
    const auto& event=p.encounter;
    if (event.reward && (std::ranges::find(db.encounters,event)==db.encounters.end() ||
        !recipe::fitsEncounter(db,r,event) ||
        (!event.roles.empty() && std::ranges::find(event.roles,p.role)==event.roles.end()))) return ReplaceResult::Rejected;
    if (p.role) {
        if (!db.roleLists.contains(p.role) || !p.personality ||
            !roleWitness(original,db.trees.at(r.base),db.roleLists.at(p.role),p.role,p)) return ReplaceResult::Rejected;
        at<P>(b+0x170)=p.roleSeed;
        call<void>(0x141c62b24,b,p.personality);
        at<P>(b+0x148)=p.role; // native builder's single fixed-role input
    }
    at<unsigned>(b+0x178)=p.generationLevel+1; at<unsigned char>(b+0x17d)=follower;
    call<void>(0x141c4edc0,b,db.trees.at(r.base));
    call<void>(0x141c4ee54,b,r.tribe);
    for(auto id:db.allSpecializations) if(id!=r.specialization) call<void>(0x141c62c44,b,id);
    for(auto [id,_]:db.rarityRecords) if(id!=r.rarity) call<void>(0x141c62c44,b,id);
    for (P marker:p.requiredMarkers) if (marker!=r.base) call<void>(0x141c4ee48,b,marker);
    for (P marker:p.forbiddenMarkers)
        if (!db.allSpecializations.contains(marker) && !db.rarityRecords.contains(marker)) call<void>(0x141c62c44,b,marker);
    for(P id:p.grants) if (std::ranges::find(event.grants,id)==event.grants.end()) call<void>(0x141c62aa4,b,id);
    for(P id:p.exclude) call<void>(0x141c62ab0,b,id);
    if (!installBuildCap() || (!p.blockedClassRewards.empty() && !installRewardGate())) return ReplaceResult::Rejected;
    call<void>(0x141bc6014,m,p.army);
    at<unsigned>(m+0x174)=p.index;
    at<P>(m+0x268)=p.row;
    buildAtClassCap(m,ctx,b,p.row,db.trees.at(r.base),p.blockedClassRewards);
    call<void>(0x14061ba74,m); // Row-dependent gear and look, without broadcasting a live seat change.
    at<unsigned char>(m+0x51a)=intel;
    at<unsigned char>(m+0x51b)=column;
    std::vector<P> markers,active;
    if(at<P>(m+0x178)!=p.army || at<P>(m+0x268)!=p.row || at<unsigned>(m+0x174)!=p.index ||
       at<unsigned char>(m+0x51b)!=column || !!(at<std::uint64_t>(m+0x508)&(1ull<<27))!=follower ||
       at<unsigned>(m+0x4b8)!=p.generationLevel || at<P>(m+0x188)!=db.trees.at(r.base) || at<P>(m+0x328)!=r.tribe ||
       at<P>(m+0x390)!=db.rarityRecords.at(r.rarity) || !records(m+0x1b0,8,markers) ||
       !std::ranges::all_of(p.requiredMarkers,[&](P id){return std::ranges::find(markers,id)!=markers.end();}) ||
       std::ranges::any_of(markers,[&](P id){return p.forbiddenMarkers.contains(id);}) ||
       std::ranges::any_of(markers,[&](P v){return (db.allSpecializations.contains(v) && v!=r.specialization) ||
           (db.rarityRecords.contains(v) && v!=r.rarity);}) || !records(m+0x198,8,active))
        return ReplaceResult::ChangedUnexpectedly;
    if ((p.role && at<P>(m+0x318)!=p.role) || (!p.weapons.empty() && (!gearScope || !gearScope->populated)) ||
        !equipmentMatches(m,p.weapons)) return ReplaceResult::ChangedUnexpectedly;
    if (event.reward) {
        if (!rewardEligible(m,event.list,event.reward)) return ReplaceResult::ChangedUnexpectedly;
        // Same dispatcher and arguments as the encounter-list branch of the captain builder.
        call<void>(0x1403511f4,m,event.reward,P(0),static_cast<unsigned char>(1));
        std::vector<P> slots;
        if (!records(m+0x1e0,32,slots) || std::ranges::any_of(event.grants,[&](P id){
            return std::ranges::find(slots,id)==slots.end();}) || !equipmentMatches(m,p.weapons) ||
            !records(m+0x198,8,active)) return ReplaceResult::ChangedUnexpectedly;
    }
    if(ownedOptionalResult(m,db,p,active)) return ReplaceResult::Replaced;
    // Direct grants and later rewards do not consult the weighted-draw exclusion list.
    // Only reconcile optional slots if the finished native result actually differs.
    std::vector<P> native,fixed;
    if (!records(m+0x1e0,32,native)) return ReplaceResult::ChangedUnexpectedly;
    const P slots=at<P>(m+0x1e0);
    for(size_t i=0;i<native.size();++i) {
        P v=at<P>(slots+32*i+8);
        if (p.automatic.contains(v) || !db.categories.contains(v) || db.managedTraits.contains(v) || db.managedPickers.contains(native[i]))
            fixed.push_back(native[i]);
    }
    std::set<P> automatic=p.automatic;
    for(P v:active) if(db.managedTraits.contains(v) || !db.categories.contains(v)) automatic.insert(v);
    std::vector<P> wanted;
    wanted=fixed;
    std::vector<P> additions;
    for(P id:p.grants) if(std::ranges::find(wanted,id)==wanted.end()) {wanted.push_back(id);additions.push_back(id);}
    std::vector<P> projected;
    recipe::PositionOutputs overrides;
    if(!permitsGrants(m,additions) || !recipe::fitsCore(db,fixed,additions) ||
       !acquisitionTraits(m,wanted,p,db,projected,overrides) ||
       !recipe::matches(db,r,projected.data(),projected.size(),automatic) || !write(m,wanted) ||
       !records(m+0x198,8,active) || !ownedOptionalResult(m,db,p,active))
        return ReplaceResult::ChangedUnexpectedly;
    return ReplaceResult::Replaced;
}
bool restoreLevel(P m,const GenerationPlan& p) {
    if (at<P>(m+0x178)!=p.army || at<unsigned>(m+0x174)!=p.index || at<P>(m+0x268)!=p.row) return false;
    if (at<unsigned>(m+0x4b8)!=p.level) call<void>(game::kFactionMemberSetXPLevel,m,p.level);
    return at<unsigned>(m+0x4b8)==p.level;
}
void refreshGeneratedTraits(P m,const GenerationPlan& p) {
    // The XP setter can reevaluate level-gated slots. Refresh only the trait
    // calculation at its generation level; do not dispatch another level-up.
    at<unsigned>(m+0x4b8)=p.generationLevel;
    __try {call<void>(game::kFactionMemberUpdateTraits,m,0u);}
    __finally {at<unsigned>(m+0x4b8)=p.level;}
}
bool verifyRestored(P m,const GenerationPlan& p,const recipe::Catalog& db,bool allowRefresh=true) {
    std::vector<P> active,markers;
    const auto& r=p.requested;
    auto matches=[&]{return (!p.role || at<P>(m+0x318)==p.role) && equipmentMatches(m,p.weapons) &&
        at<unsigned>(m+0x4b8)==p.level && at<P>(m+0x188)==db.trees.at(r.base) &&
        at<P>(m+0x328)==r.tribe && at<P>(m+0x390)==db.rarityRecords.at(r.rarity) &&
        records(m+0x1b0,8,markers) &&
        std::ranges::all_of(p.requiredMarkers,[&](P id){return std::ranges::find(markers,id)!=markers.end();}) &&
        std::ranges::none_of(markers,[&](P id){return p.forbiddenMarkers.contains(id) ||
            (db.allSpecializations.contains(id) && id!=r.specialization) ||
            (db.rarityRecords.contains(id) && id!=r.rarity);}) &&
        records(m+0x198,8,active) && ownedOptionalResult(m,db,p,active) &&
        std::ranges::all_of(p.automatic,[&](P id){return std::ranges::find(active,id)!=active.end();});};
    if (matches()) return true;
    if (!allowRefresh) return false;
    refreshGeneratedTraits(m,p);
    return matches();
}
bool detachedNode(P n) {return at<P>(n)==n && at<P>(n+8)==n;}
// These native vectors own heap storage. Exchanging both headers retains the
// original buffers (including picker-slot state) intact for a possible undo.
constexpr membercopy::Region buildVectors[]={{0x198,8},{0x1b0,8},{0x1e0,32},{0x228,8},{0x358,8}};
bool independentBuildStorage(P a,P b,std::string& error) {
    for (unsigned off:{0x150,0x158,0x2d8}) {
        if (!at<P>(a+off) || !at<P>(b+off) || at<P>(a+off)==at<P>(b+off)) {
            error="Missing or shared build subobject at offset "+std::to_string(off)+". No build applied.";
            return false;
        }
    }
    auto internal=[&](P ptr){return (ptr>=a && ptr-a<0x528) || (ptr>=b && ptr-b<0x528);};
    struct Range { P start,end; };
    std::vector<Range> ranges;
    for (P m:{a,b}) for (auto [off,stride]:buildVectors) {
        const P h=m+off,begin=at<P>(h),end=at<P>(h+8);
        const size_t cap=at<unsigned>(h+16);
        auto reject=[&](const char* why){
            error=std::string(m==a?"Original":"Preview")+" vector at offset "+std::to_string(off)+": "+why+". No build applied.";
            return false;
        };
        if (!membercopy::transferableBacking(begin,at<unsigned char>(h+20))) return reject("backing is not independently owned");
        if (cap>65536 || end<begin || (end-begin)%stride ||
            end-begin>cap*stride || (!begin && cap) || internal(begin) || begin>UINTPTR_MAX-cap*stride) return reject("invalid bounds");
        if (begin) {
            const auto limit=begin+cap*stride;
            for (auto other:ranges) if (other.start==begin || (begin<other.end && other.start<limit)) return reject("overlapping storage");
            if ((begin<a+0x528 && a<limit) || (begin<b+0x528 && b<limit)) return reject("storage overlaps a captain");
            ranges.push_back({begin,limit});
            for (P pos=begin;pos<end;pos+=sizeof(P)) if (internal(at<P>(pos))) return reject("entry references a captain directly");
        }
    }
    return true;
}
void exchangeBuild(P a,P b) {
    auto* left=reinterpret_cast<std::byte*>(a);
    auto* right=reinterpret_cast<std::byte*>(b);
    membercopy::exchangeValues(left,right);
    for (auto [off,_]:buildVectors) std::swap_ranges(left+off,left+off+24,right+off);
    // Derived by the same native trait calculation as the active array.
    std::swap(at<P>(a+0x378),at<P>(b+0x378));
    constexpr uint64_t traitBits=(1ull<<14)|(1ull<<19)|(1ull<<34);
    const auto delta=(at<uint64_t>(a+0x508)^at<uint64_t>(b+0x508))&traitBits;
    at<uint64_t>(a+0x508)^=delta;at<uint64_t>(b+0x508)^=delta;
}
// Use the member copier's own subobject operations, never share an inventory
// instance or replace an appearance object that other game code may reference.
bool copyOwnedBuild(P dst,P src,bool inSeat) {
    __try {
        call<void>(0x141bc6040,dst,at<unsigned>(src+0x170));
        call<void>(0x141c3c798,at<P>(dst+0x150),at<P>(src+0x150));
        if (inSeat) call<void>(0x141c3ccc0,at<P>(dst+0x150),at<P>(dst+0x178),at<P>(dst+0x268),
            at<P>(dst+0x318),at<unsigned>(dst+0x4b8),call<bool>(0x14114dfd8,dst));
        call<void>(0x141c66788,at<P>(dst+0x158),at<P>(src+0x158));
        call<void>(0x1401c7948,dst+0x50,src+0x50);
        call<void>(0x1402eba60,at<P>(dst+0x2d8),at<P>(src+0x2d8));
        if (at<unsigned>(dst+0x170)!=at<unsigned>(src+0x170)) return false;
        for (unsigned i=0;i<10;++i)
            if (equippedRecord(dst+0x50,i)!=equippedRecord(src+0x50,i)) return false;
        Look actual{},expected{};
        return inspect(at<P>(dst+0x2d8),actual) && inspect(at<P>(src+0x2d8),expected) && actual==expected;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool supportedMemberCopy() {
    static const bool supported=[]{
        const auto* code=reinterpret_cast<const unsigned char*>(game::FromRVA(0x141bb936c));
        uint64_t hash=1469598103934665603ull;
        for (size_t i=0;i<0x551;++i) hash=(hash^code[i])*1099511628211ull;
        return hash==0x34fcc366f571f893ull;
    }();
    return supported;
}
P seatOwner(P m,const GenerationPlan& p) {
    if (!m || at<P>(m)!=game::FromRVA(game::kFactionMemberVtable) ||
        at<P>(m+0x168)!=p.originalSeed || at<unsigned>(m+0x174)!=p.index ||
        at<P>(m+0x178)!=p.army || at<P>(m+0x268)!=p.row || at<unsigned>(m+0x4b8)!=p.level ||
        at<P>(m+0x48) || at<P>(m+0x190) || (at<uint64_t>(m+0x508)&(4|(1ull<<37))) ||
        !detachedNode(m+8) || at<int>(m+0x2d0)!=-1 ||
        at<P>(m+0x2e8)!=at<P>(m+0x2f0)) return 0;
    P ctx=army(m),pool=ctx?at<P>(ctx+0x20):0;
    if (!pool) return 0;
    const P b=at<P>(pool+0x18),e=at<P>(pool+0x20);
    if (!b || e<b || (e-b)%sizeof(P) || (e-b)/sizeof(P)>4096 || p.index>=(e-b)/sizeof(P)) return 0;
    P slot=b+p.index*sizeof(P);
    return at<P>(slot)==m?slot:0;
}
bool readbackBuild(P member,const GenerationPlan& p,const recipe::Catalog& db) {
    __try {return seatOwner(member,p) && verifyRestored(member,p,db,false);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
bool PlanGeneration(P m,P rank,const recipe::Catalog& db,const recipe::Recipe& r,GenerationPlan& p,std::string& error,uint64_t session) {
    HateGateScope scope{db,r};
    auto* previous=hateScope;
    __try {
        __try {
            if (!beginHateScope(scope)) {error="Cannot apply the selected Hate's weakness exception.";return false;}
            return withCampaignRules(db,[&]{return makePlan(m,rank,db,r,p,error,session);});
        } __finally {hateScope=previous;}
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {error="Could not prepare the native generation inputs.";return false;}
}
ReplaceResult ReplaceCaptain(P m,const GenerationPlan& p,const recipe::Catalog& db,const recipe::Recipe& r,WritePickers write,std::string& detailMessage) {
    P owned=0,backup=0;
    bool published=false;
    GearScope gear{0,db,p.weapons};
    auto* previousGear=gearScope;
    HateGateScope scope{db,r};
    auto* previous=hateScope;
    __try {
        __try {
            if (!Supported() || !supportedMemberCopy() || !seatOwner(m,p)) return ReplaceResult::SeatUnavailable;
            call<void>(0x1419ed468,&owned);
            if (!owned) return ReplaceResult::Rejected;
            gear.member=owned;
            scope.memberTraits=owned+0x198;
            if (!beginHateScope(scope)) return ReplaceResult::Rejected;
            if (!p.weapons.empty() && !installGearLocks()) return ReplaceResult::Rejected;
            gearScope=p.weapons.empty()?nullptr:&gear;
            bool valid=withCampaignRules(db,[&]{return replace(m,owned,p,db,r,write)==ReplaceResult::Replaced &&
                restoreLevel(owned,p) && verifyRestored(owned,p,db);});
            gearScope=previousGear;hateScope=previous;
            if (!valid) return ReplaceResult::Rejected;
            if (!seatOwner(m,p)) return ReplaceResult::SeatUnavailable;
            if (!independentBuildStorage(m,owned,detailMessage)) return ReplaceResult::Rejected;
            call<void>(0x1419ed468,&backup);
            if (!backup || !copyOwnedBuild(backup,m,false)) {
                detailMessage="Could not back up the captain's owned build data. No build applied.";
                return ReplaceResult::Rejected;
            }
            if (!seatOwner(m,p)) return ReplaceResult::SeatUnavailable;
            // Neither the Army slot nor either member's observer nodes move.
            auto outcome=membercopy::commit([&]{exchangeBuild(m,owned);},
                [&]{return withCampaignRules(db,[&]{return copyOwnedBuild(m,owned,true) && readbackBuild(m,p,db);});},
                [&]{return copyOwnedBuild(m,backup,true);});
            if (outcome==membercopy::Outcome::Applied) {
                published=true;return ReplaceResult::Replaced;
            }
            if (outcome==membercopy::Outcome::RollbackFailed) return ReplaceResult::RollbackFailed;
            detailMessage="In-place build readback failed. Restored the original build.";
            return ReplaceResult::Rejected;
        } __finally {
            gearScope=previousGear;hateScope=previous;
            if (backup) call<void>(0x1419ef15c,&backup);
            if (owned) call<void>(0x1419ef15c,&owned);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {return published?ReplaceResult::CleanupFailed:ReplaceResult::Rejected;}
}
}
