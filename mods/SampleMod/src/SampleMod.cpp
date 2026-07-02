// SampleMod - a minimal external SoW mod DLL. Proves the mods/ pipeline end to end: the loader
// finds it in x64\mods\, LoadLibrary()s it, and calls SoWMod_Init, where we register a HagUI page.
// This is the template every real mod (e.g. the inventory editor) follows — none of this lives in
// the loader; only the loader core + HagUI do.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "HagUIAPI.h"    // ../../shared : the HagUI cross-plugin API (resolved at runtime, not linked)
#include "SoWModAPI.h"   // ../../shared : the mod entry-point contract

namespace {
bool g_sampleToggle = false;

void OnSampleButton() {
    ::OutputDebugStringA("[SampleMod] button clicked\n");
}
}  // namespace

// Called once by the loader after this DLL is loaded (on the worker thread, outside the loader lock).
extern "C" __declspec(dllexport) void SoWMod_Init() {
    auto get = reinterpret_cast<HagUI_GetAPI_t>(
        ::GetProcAddress(::GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI"));
    HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
    if (!ui) { ::OutputDebugStringA("[SampleMod] HagUI API unavailable\n"); return; }

    const int page = ui->RegisterPage("Sample Mod");
    ui->AddLabel(page, "This page comes from mods/SampleMod - an external mod DLL, not the loader.");
    ui->AddToggle(page, "Sample toggle", &g_sampleToggle);
    ui->AddButton(page, "Sample button", &OnSampleButton);
    ::OutputDebugStringA("[SampleMod] registered its HagUI page\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) ::DisableThreadLibraryCalls(hModule);   // keep DllMain trivial
    return TRUE;
}
