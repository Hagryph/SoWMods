#include <Windows.h>
#include <string_view>
#include "SoWModAPI.h"
#include "Settings.h"
#include "RecruitmentHooks.h"
#include "UrukEditor.h"

namespace { bool recruitmentReady = false; }
extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "Uruks"; }
extern "C" __declspec(dllexport) bool SoWMod_RegisterActionsV2(const sowmod::actions::Registrar* r) {
    using namespace sowmod::actions;
    static constexpr Input toggle[]={{"enabled","Enabled",Boolean,"false",0,0,nullptr,
        {[](unsigned,const char* const*,const char*,Emit emit,void* ctx){emit(ctx,uruk::IgnoreRecruitmentLevel()?"true":"false");return true;},OnOpen|AfterRun}}};
    return r && r->version==SchemaVersion && uruk::RegisterEditorActions(*r) &&
        r->Add({"get_recruitment","Read recruitment setting",false,nullptr,0,
            [](const char* const*,Emit emit,void* ctx){emit(ctx,uruk::IgnoreRecruitmentLevel()?"true":"false");return true;},nullptr,"Recruitment"}) &&
        r->Add({"set_recruitment","Ignore recruitment level",false,toggle,1,
            [](const char* const* v,Emit emit,void* ctx){bool on=std::string_view(v[0])=="true";
                if (!recruitmentReady) { emit(ctx,"recruitment hooks unavailable"); return false; }
                uruk::SetIgnoreRecruitmentLevel(on);bool actual=uruk::IgnoreRecruitmentLevel();
                emit(ctx,actual?"true":"false");return actual==on;},nullptr,"Recruitment"});
}
extern "C" __declspec(dllexport) int SoWMod_Scope() { return SOWMOD_GLOBAL; }
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    if (!uruk::LoadSettings(page)) { uruk::Log("Cannot initialize mods/cfg/Uruks.ini"); return; }
    recruitmentReady = uruk::InstallRecruitmentHooks();
    if (!recruitmentReady) uruk::Log("Recruitment hooks unavailable");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) ::DisableThreadLibraryCalls(module);
    return TRUE;
}
