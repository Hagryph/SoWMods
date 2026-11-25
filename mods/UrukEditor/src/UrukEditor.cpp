#include <Windows.h>
#include "HagUIAPI.h"
#include "SoWModAPI.h"
#include "Settings.h"
#include "RecruitmentHooks.h"

extern "C" __declspec(dllexport) const char* SoWMod_Name() { return "UrukEditor"; }
extern "C" __declspec(dllexport) int SoWMod_Scope() { return SOWMOD_GLOBAL; }
extern "C" __declspec(dllexport) void SoWMod_Init(int page) {
    const auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    auto* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { uruk::Log("HagUI v9 unavailable"); return; }
    if (!uruk::LoadSettings(page)) {
        uruk::Log("Cannot initialize mods/cfg/UrukEditor.ini");
        ui->AddLabel(page, "UrukEditor configuration could not be loaded.");
        return;
    }
    if (!uruk::InstallRecruitmentHooks()) {
        uruk::Log("Recruitment hooks unavailable; setting cannot be enabled");
        ui->AddLabel(page, "Recruitment hooks unavailable for this game build.");
        return;
    }
    ui->AddSettingToggle(page, "Ignore Recruitment Level", &uruk::IgnoreRecruitmentLevel,
                         &uruk::SetIgnoreRecruitmentLevel);
    uruk::Log("Registered global UrukEditor settings");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) ::DisableThreadLibraryCalls(module);
    return TRUE;
}
