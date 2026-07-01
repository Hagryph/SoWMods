// Exports the flat C cross-plugin API (shared/HagUIAPI.h). Other SoW mods resolve HagUI_GetAPI via
// GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI") and register their own pages
// without linking HagUI. The C wrappers just delegate to the HagUI singleton.
#include "HagUI.h"
#include "HagUIAPI.h"   // ../shared (on the include path)

namespace {
int  ApiRegisterPage(const char* t)                        { return sow::HagUI::Get().RegisterPage(t); }
void ApiAddLabel(int p, const char* t)                     { sow::HagUI::Get().AddLabel(p, t); }
void ApiAddToggle(int p, const char* l, bool* v)           { sow::HagUI::Get().AddToggle(p, l, v); }
void ApiAddButton(int p, const char* l, void (*f)())       { sow::HagUI::Get().AddButton(p, l, f); }

HagUIAPI g_api = { HAGUI_ABI_VERSION, &ApiRegisterPage, &ApiAddLabel, &ApiAddToggle, &ApiAddButton };
}  // namespace

extern "C" __declspec(dllexport) HagUIAPI* HagUI_GetAPI(std::uint32_t abiVersion) {
    return (abiVersion == HAGUI_ABI_VERSION) ? &g_api : nullptr;
}
