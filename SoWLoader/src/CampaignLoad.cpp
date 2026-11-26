#include "CampaignLoad.h"
#include "GameHooks.h"
#include "GameOffsets.h"
#include <atomic>
#include <mutex>

namespace sow {
namespace {
using namespace sowmod::actions;
using P=uintptr_t;
enum class Phase { Idle,Queued,Starting,Waiting,Loaded,Failed };
std::mutex lock;
Phase phase=Phase::Idle;
std::atomic<bool> pending=false;
bool frontEnd=false;
ULONGLONG deadline=0;
ULONGLONG nextCheck=0;
bool profileRequested=false;
std::string status="No load requested.";

template<class T>T at(P p){return *reinterpret_cast<T*>(p);}
const char* startCampaign(bool& retry){
    // The mode-selection Continue branch tail-calls this routine with the mode
    // in RDX. It resolves the saved region itself. Never call RestartDataGameMode.
    __try {
        const P base=reinterpret_cast<P>(GetModuleHandleW(nullptr));
        auto nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+at<IMAGE_DOS_HEADER>(base).e_lfanew);
        const P resume=base+0xcf8ed0;
        if(nt->FileHeader.TimeDateStamp!=0x5b7f5782 || nt->OptionalHeader.SizeOfImage!=0x73fd000 ||
           memcmp(reinterpret_cast<void*>(resume),"\x48\x8b\xc4\x48\x89\x48\x08\x56\x41\x56",10))
            return "Unsupported game executable.";
        const P shell=at<P>(base+0x26ffa98);
        if(!shell || !at<P>(shell+0xe38) || !at<P>(shell+0xeb8)){
            retry=true;return "Waiting for Steam profile enumeration.";
        }
        if(at<unsigned char>(shell+0xed0)==255){
            P begin=at<P>(shell+0xeb8),end=at<P>(shell+0xec0);
            if(end==begin){retry=true;return "Waiting for Steam profile enumeration.";}
            if(end-begin!=sizeof(P))return "Multiple local profiles; no account was selected.";
            P only=at<P>(begin);
            if(!only || !at<P>(only+0x168) || !at<unsigned>(only+0x38)){
                retry=true;return "Waiting for the signed-in Steam user.";
            }
            if(!profileRequested){
                profileRequested=true;
                reinterpret_cast<void(*)(P)>(base+0x1781a60)(at<P>(shell+0xe38));
            }
            retry=true;return "Waiting for the native profile to activate.";
        }
        P profile=at<P>(at<P>(shell+0xeb8)+8*at<unsigned char>(shell+0xed0));
        if(!profile || at<unsigned>(profile+0x38)!=3 || !at<unsigned char>(profile+0xd8b)){
            retry=true;return "Waiting for campaign save metadata.";
        }
        const P database=at<P>(game::FromRVA(game::kGameDatabase));
        if(!database || !at<P>(database+0x50) || at<unsigned>(database+0x48)>10000)return "Campaign catalog unavailable.";
        P mode=0;
        for(unsigned i=0;i<at<unsigned>(database+0x48);++i){
            P category=at<P>(database+0x50)+0x58*i;
            if(!at<P>(category) || strcmp(at<const char*>(category),"GameModes"))continue;
            auto count=at<unsigned>(category+0x28);auto rows=at<P>(category+0x38);
            if(!rows || count>4096)return "Invalid game-mode catalog.";
            for(unsigned j=0;j<count;++j){
                P row=rows+0x28*j;
                if(at<P>(row+0x18)!=category || !at<P>(row+0x20))continue;
                if(strcmp(at<const char*>(row+0x20),"SinglePlayer"))continue;
                auto slot=reinterpret_cast<unsigned(*)(P)>(base+0xcddb40)(row);
                if(slot!=0)continue; // main campaign, never DLC
                // Same availability query as the native Continue branch.
                if(reinterpret_cast<unsigned(*)(P,unsigned)>(base+0xd07aa0)(row,0)!=3)continue;
                if(mode)return "Ambiguous playable campaign; no load attempted.";
                mode=row;
            }
        }
        if(!mode)return "No resumable main campaign in the native catalog.";
        if(!at<P>(profile+0x1c8+8))return "Campaign has no saved region; will not start a new game.";
        reinterpret_cast<void(*)(P,P)>(resume)(0,mode);
        return nullptr;
    }__except(EXCEPTION_EXECUTE_HANDLER){return "Native campaign load faulted; check game state.";}
}
bool request(const char* const*,Emit emit,void* ctx){
    std::lock_guard guard(lock);
    if(CurrentSaveSession()){emit(ctx,"A save is already loaded.");return false;}
    if(phase==Phase::Queued || phase==Phase::Starting || phase==Phase::Waiting){emit(ctx,status.c_str());return false;}
    phase=Phase::Queued;status="Queued on the game thread.";deadline=GetTickCount64()+60000;
    profileRequested=false;nextCheck=0;pending=true;
    emit(ctx,status.c_str());return true;
}
bool readStatus(const char* const*,Emit emit,void* ctx){
    std::lock_guard guard(lock);
    if((phase==Phase::Queued || phase==Phase::Waiting) && GetTickCount64()>=deadline){
        phase=Phase::Failed;pending=false;status="Timed out; no campaign load confirmed.";
    }
    emit(ctx,status.c_str());return true;
}
}
void CampaignFrontEnd(){
    std::lock_guard guard(lock);frontEnd=true;
    if(phase==Phase::Loaded){phase=Phase::Idle;status="At the front end."; }
}
void CampaignWorldLoaded(){
    std::lock_guard guard(lock);frontEnd=false;pending=false;
    phase=Phase::Loaded;status="World loaded; session="+std::to_string(CurrentSaveSession());
}
void PumpCampaignLoad(){
    if(!pending.exchange(false))return;
    {
        std::lock_guard guard(lock);
        if(phase!=Phase::Queued)return;
        if(CurrentSaveSession() || GetTickCount64()>=deadline){
            phase=Phase::Failed;status="Load request expired or session changed.";return;
        }
        if(GetTickCount64()<nextCheck){pending=true;return;}
        phase=Phase::Starting;status="Starting campaign.";
    }
    bool retry=false;
    auto error=startCampaign(retry);
    std::lock_guard guard(lock);
    if(phase!=Phase::Starting)return;
    if(retry){
        status=error;phase=Phase::Queued;nextCheck=GetTickCount64()+200;pending=true;return;
    }
    phase=error?Phase::Failed:Phase::Waiting;status=error?error:"Waiting for campaign world.";
    deadline=GetTickCount64()+120000;
}
bool RegisterCampaignActions(const sowmod::actions::Registrar* r){
    return r && r->Add({"load_save","Continue campaign",false,nullptr,0,request,nullptr,"Campaign"}) &&
        r->Add({"load_status","Campaign load status",false,nullptr,0,readStatus,nullptr,"Campaign"});
}
}
