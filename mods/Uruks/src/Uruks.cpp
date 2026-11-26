#include <Windows.h>
#include <string_view>
#include <string>
#include "SoWModAPI.h"
#include "Settings.h"
#include "RecruitmentHooks.h"
#include "RetainedTraits.h"
#include "CaptainGeneration.h"

namespace { bool recruitmentReady = false; }
extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "Uruks"; }
extern "C" __declspec(dllexport) void SoWMod_WorldLoaded() {
    uruk::retained::RefreshLoaded();
    if (uruk::FreeGarrisonPlacement() && !uruk::ApplyGarrisonCost(true))
        uruk::Log("Could not apply Free Garrison Placement on world load");
}
extern "C" __declspec(dllexport) bool SoWMod_RegisterActionsV4(const sowmod::actions::Registrar* r) {
    using namespace sowmod::actions;
    static constexpr Input toggle[]={{"enabled","Enabled",Boolean,"false",0,0,nullptr,
        {[](unsigned,const char* const*,const char*,Emit emit,void* ctx){emit(ctx,uruk::IgnoreRecruitmentLevel()?"true":"false");return true;},OnOpen|AfterRun}}};
    static constexpr Input placement[]={{"enabled","Enabled",Boolean,"false",0,0,nullptr,
        {[](unsigned,const char* const*,const char*,Emit emit,void* ctx){emit(ctx,uruk::FreeGarrisonPlacement()?"true":"false");return true;},OnOpen|AfterRun}}};
    return r && r->version==SchemaVersion && uruk::RegisterGenerationActions(*r) &&
        r->Add(Action{"refresh_retained","Reevaluate retained slots",true,nullptr,0,
            [](const char* const*,Emit emit,void* ctx){auto n=uruk::retained::RefreshLoaded();emit(ctx,std::to_string(n).c_str());return n>=0;}}.IpcOnly()) &&
        r->Add(Action{"get_recruitment","Read recruitment setting",false,nullptr,0,
            [](const char* const*,Emit emit,void* ctx){emit(ctx,uruk::IgnoreRecruitmentLevel()?"true":"false");return true;}}.IpcOnly()) &&
        r->Add({"set_recruitment","Ignore Recruitment Level",false,toggle,1,
            [](const char* const* v,Emit emit,void* ctx){bool on=std::string_view(v[0])=="true";
                if (!recruitmentReady) { emit(ctx,"recruitment hooks unavailable"); return false; }
                uruk::SetIgnoreRecruitmentLevel(on);bool actual=uruk::IgnoreRecruitmentLevel();
                emit(ctx,actual?"true":"false");return actual==on;}}) &&
        r->Add({"set_free_garrison","Free Garrison Placement",true,placement,1,
            [](const char* const* v,Emit emit,void* ctx){
                const bool on=std::string_view(v[0])=="true", previous=uruk::FreeGarrisonPlacement();
                if (!uruk::ApplyGarrisonCost(on)) { emit(ctx,"Reassignment Order cost unavailable"); return false; }
                if (!uruk::SaveFreeGarrisonPlacement(on)) {
                    uruk::ApplyGarrisonCost(previous); emit(ctx,"Could not save Garrison setting"); return false;
                }
                emit(ctx,on?"true":"false");return true;}});
}
extern "C" __declspec(dllexport) int SoWMod_Scope() { return SOWMOD_GLOBAL; }
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    if (!uruk::LoadSettings(page)) { uruk::Log("Cannot initialize mods/cfg/Uruks.ini"); return; }
    recruitmentReady = uruk::InstallRecruitmentHooks();
    if (!uruk::retained::Install()) uruk::Log("Owned-slot retention unavailable");
    if (!recruitmentReady) uruk::Log("Recruitment hooks unavailable");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) ::DisableThreadLibraryCalls(module);
    return TRUE;
}
