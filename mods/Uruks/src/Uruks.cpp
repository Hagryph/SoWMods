#include <Windows.h>
#include <string_view>
#include "HagUIAPI.h"
#include "SoWModAPI.h"
#include "Settings.h"
#include "RecruitmentHooks.h"
#include "UrukEditor.h"

extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "Uruks"; }
extern "C" __declspec(dllexport) bool SoWMod_RegisterActions(const sowmod::actions::Registrar* r) {
    using namespace sowmod::actions;
    static constexpr Input toggle[]={{"enabled","Enabled",Boolean,"false"}};
    return r && r->version==1 && uruk::RegisterEditorActions(*r) &&
        r->Add({"get_recruitment","Read recruitment setting",false,nullptr,0,
            [](const char* const*,Emit emit,void* ctx){emit(ctx,uruk::IgnoreRecruitmentLevel()?"true":"false");return true;}}) &&
        r->Add({"set_recruitment","Ignore recruitment level",false,toggle,1,
            [](const char* const* v,Emit emit,void* ctx){bool on=std::string_view(v[0])=="true";
                uruk::SetIgnoreRecruitmentLevel(on);bool actual=uruk::IgnoreRecruitmentLevel();
                emit(ctx,actual?"true":"false");return actual==on;}});
}
extern "C" __declspec(dllexport) int SoWMod_Scope() { return SOWMOD_GLOBAL; }
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    const auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    auto* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { uruk::Log("HagUI v11 unavailable"); return; }
    if (!uruk::LoadSettings(page)) {
        uruk::Log("Cannot initialize mods/cfg/Uruks.ini");
        ui->AddLabel(page, "Uruks configuration could not be loaded.");
        return;
    }
    if (!uruk::InstallRecruitmentHooks()) {
        uruk::Log("Recruitment hooks unavailable; setting cannot be enabled");
        ui->AddLabel(page, "Recruitment hooks unavailable for this game build.");
        return;
    }
    ui->AddSettingToggle(page, "Ignore Recruitment Level", &uruk::IgnoreRecruitmentLevel,
                         &uruk::SetIgnoreRecruitmentLevel);
    uruk::AddUrukEditor(page, ui);
    uruk::Log("Registered global Uruks settings");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) ::DisableThreadLibraryCalls(module);
    return TRUE;
}
