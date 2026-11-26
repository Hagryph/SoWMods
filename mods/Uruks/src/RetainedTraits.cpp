#include "RetainedTraits.h"
#include "GeneratorNative.h"
#include "GameOffsets.h"
#include "Settings.h"
#include <MinHook.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <span>
#include <string_view>

namespace uruk::retained {
namespace {
using P=std::uintptr_t;
using Identity=std::array<std::string,5>; // Name, class tree, tribe, role, head; no Army seat or runtime address.
using Rules=std::set<std::pair<std::string,std::string>>;
std::map<Identity,Rules> profiles;
std::mutex profileLock;
std::filesystem::path path;
bool ready=false;
template<class T> T read(P p) {
    __try {return p?*reinterpret_cast<const T*>(p):T{};} __except(EXCEPTION_EXECUTE_HANDLER){return {};}
}
bool copy(P p,char* dest,unsigned n) {
    __try {
        if(p) for(unsigned i=0;i<n;++i) if(!(dest[i]=read<char>(p+i)))return i>0;
    } __except(EXCEPTION_EXECUTE_HANDLER){}
    dest[0]=0;return false;
}
std::string name(P rec) {char buf[256]{};copy(read<P>(rec?rec+32:0),buf,sizeof buf);return buf;}
Identity identity(P m,const char* first) {
    return {first,name(read<P>(m+0x188)),name(read<P>(m+0x328)),name(read<P>(m+0x318)),
            name(read<P>(read<P>(m+0x2d8)+0x170))};
}
std::span<const P> links(P rec,unsigned hash) {
    if (!rec || (read<unsigned char>(rec+1)&16))return {};
    const P fields=read<P>(rec+8),cells=read<P>(rec+16);
    if(!fields || !cells)return {};
    for(unsigned i=0;i<read<unsigned char>(rec);++i){
        const P f=fields+16*i;
        if(read<unsigned>(f)==hash && (read<unsigned char>(f+10)&63)==10){
            unsigned n=read<unsigned short>(f+12);
            if(n>512)return {};
            return {reinterpret_cast<const P*>(cells+8ull*read<unsigned short>(f+14)),n};
        }
    }
    return {};
}
template<class Work> void slots(P m,Work work) {
    P b=read<P>(m+0x1e0),e=read<P>(m+0x1e8);
    if(!b || e<b || e-b>512*32 || (e-b)%32)return;
    for(;b<e;b+=32)work(read<P>(b),read<P>(b+8));
}
bool weaknessOnly(P entry,const std::string& output){
    if(!output.starts_with("Hate_"))return false;
    auto required=links(entry,0xb05e2ca4);
    return !required.empty() && std::ranges::all_of(required,[](P id){
        auto s=name(id);return s.starts_with("Vulnerable_") || s.starts_with("MortalVulnerable_");
    });
}
struct Binding { P member{}; std::set<P> levels,weakness; };
thread_local const Binding* current=nullptr;
bool bind(P m,Binding& b) {
    char first[32]{};
    if(read<P>(m)!=game::FromRVA(game::kFactionMemberVtable) || !copy(m+0x498,first,sizeof first))return false;
    std::lock_guard lock(profileLock);
    if(!std::ranges::any_of(profiles,[&](const auto& kv){return kv.first[0]==first;}))return false;
    auto found=profiles.find(identity(m,first));
    if(found==profiles.end())return false;
    b.member=m;
    slots(m,[&](P picker,P){
        auto id=name(picker);
        auto it=found->second.lower_bound({id,{}});
        if(it==found->second.end() || it->first!=id)return;
        for(P entry:links(picker,0x6bd57b1d)){
            auto out=links(entry,0xbcb47686);
            if(out.size()!=1)continue;
            auto trait=name(out.front());
            if(!found->second.contains({id,trait}))continue;
            b.levels.insert(entry);
            if(weaknessOnly(entry,trait))b.weakness.insert(entry);
        }
    });
    return !b.levels.empty();
}
using Eval=bool(*)(P,unsigned char);
using Minimum=unsigned(*)(P);
Eval nativeEval=nullptr;
Minimum nativeMinimum=nullptr;
unsigned minimum(P entry){
    auto level=nativeMinimum(entry);
    // Level 1 is engine index zero. The separate mission condition is never hooked.
    return level && current && current->levels.contains(entry)?1u:level;
}
bool scoped(P m,unsigned char pass,const Binding* next){
    auto previous=current;
    __try {current=next;return nativeEval(m,pass);}
    __finally {current=previous;}
}
bool evaluate(P m,unsigned char pass){
    if (!RetainGeneratedTraits()) return scoped(m,pass,nullptr);
    Binding b;
    return scoped(m,pass,bind(m,b)?&b:nullptr);
}
bool writeProfiles(const std::map<Identity,Rules>& data){
    auto tmp=path;tmp+=L".tmp";
    std::ofstream out(tmp,std::ios::trunc);
    for(auto& [id,rules]:data)for(auto& [picker,trait]:rules){
        for(auto& part:id)out<<std::quoted(part)<<' ';
        out<<std::quoted(picker)<<' '<<std::quoted(trait)<<'\n';
    }
    out.close();
    return out && MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
}
bool WeaknessWaived(P entry,P ctx) {
    return current && read<P>(ctx)==current->member+0x198 && current->weakness.contains(entry);
}
bool Install(){
    if(!engine::Supported())return false;
    wchar_t local[MAX_PATH]{};
    auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(!n || n>=MAX_PATH)return false;
    path=std::filesystem::path(local)/"SoWLoader"/"Uruks-retained.txt";
    std::ifstream in(path);std::string line;
    while(std::getline(in,line)){
        std::istringstream row(line);Identity id;std::string picker,trait;
        for(auto& s:id)row>>std::quoted(s);
        if(row>>std::quoted(picker)>>std::quoted(trait))profiles[id].emplace(picker,trait);
    }
    auto eval=reinterpret_cast<void*>(game::FromRVA(game::kFactionMemberEvaluatePass));
    auto min=reinterpret_cast<void*>(game::FromRVA(game::kPickerOptionMinXPLevel));
    if(read<uint64_t>(P(eval))!=0x4154415756535540ULL || read<uint64_t>(P(min))!=0xe8d18b4c28ec8348ULL ||
       !engine::InstallAnyTraitPredicate())return false;
    if(MH_CreateHook(eval,reinterpret_cast<void*>(&evaluate),reinterpret_cast<void**>(&nativeEval))!=MH_OK)return false;
    if(MH_CreateHook(min,reinterpret_cast<void*>(&minimum),reinterpret_cast<void**>(&nativeMinimum))!=MH_OK){
        MH_RemoveHook(eval);return false;
    }
    if(MH_EnableHook(min)!=MH_OK || MH_EnableHook(eval)!=MH_OK){
        MH_DisableHook(min);MH_RemoveHook(min);MH_RemoveHook(eval);return false;
    }
    ready=true;Log("Local owned-slot reevaluation installed");return true;
}
bool Remember(P m,const std::set<P>& outputs){
    if(!ready)return false;
    char first[32]{};
    if(!copy(m+0x498,first,sizeof first))return false;
    auto id=identity(m,first);Rules rules;
    slots(m,[&](P picker,P trait){
        if(!trait || !outputs.contains(trait))return;
        auto t=name(trait);
        for(P e:links(picker,0x6bd57b1d)){
            auto out=links(e,0xbcb47686);
            if(out.size()==1 && out.front()==trait && (nativeMinimum(e)>1 || weaknessOnly(e,t)))
                rules.emplace(name(picker),t);
        }
    });
    std::lock_guard lock(profileLock);
    auto next=profiles;
    if(rules.empty())next.erase(id);else next[id]=std::move(rules);
    if(!writeProfiles(next))return false;
    profiles.swap(next);return true;
}
int RefreshLoaded(){
    if(!ready)return -1;
    P sys=read<P>(game::FromRVA(game::kGameSystemsSingleton)),fm=read<P>(sys+0x6c68);
    P pools=read<P>(fm+0x68);unsigned n=read<unsigned>(fm+0x70);
    if(!pools || n>32)return 0;
    int matched=0;
    for(unsigned i=0;i<n;++i){
        P pool=pools+i*0xc0,b=read<P>(pool+0x20),e=read<P>(pool+0x28);
        if(!b || e<b || e-b>4096*8 || (e-b)%8)continue;
        for(;b<e;b+=8){
            P m=read<P>(b);Binding check;
            if(!m || !bind(m,check))continue;
            reinterpret_cast<void(*)(P,unsigned)>(game::FromRVA(game::kFactionMemberUpdateTraits))(m,0);
            ++matched;
        }
    }
    return matched;
}
}
