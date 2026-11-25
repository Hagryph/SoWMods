// Exports the flat C cross-plugin API (shared/HagUIAPI.h). Other SoW mods resolve HagUI_GetAPI via
// GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI") and register their own pages
// without linking HagUI. The C wrappers just delegate to the HagUI singleton.
#include "HagUI.h"
#include "GameTaskQueue.h"
#include "GameHooks.h"
#include "HagUIAPI.h"   // ../shared (on the include path)

namespace {
int  ApiRegisterPage(const char* t)                        { return sow::HagUI::Get().RegisterPage(t); }
void ApiAddLabel(int p, const char* t)                     { sow::HagUI::Get().AddLabel(p, t); }
void ApiAddToggle(int p, const char* l, bool* v)           { sow::HagUI::Get().AddToggle(p, l, v); }
void ApiAddButton(int p, const char* l, void (*f)())       { sow::HagUI::Get().AddButton(p, l, f); }
void ApiAddList(int p, const char* const* it, const char* const* ct, int n) { sow::HagUI::Get().AddList(p, it, ct, n); }
void ApiAddFacetedList(int p, const char* const* fn, int fc, const char* const* d, int ic, const char* const* fv) {
    sow::HagUI::Get().AddFacetedList(p, fn, fc, d, ic, fv);
}
void ApiAddFacetedActionList(int p, const char* const* fn, int fc, const char* const* d, const char* const* ids,
                             int ic, const char* const* fv, void (*onAdd)(const char* id, int count)) {
    sow::HagUI::Get().AddFacetedActionList(p, fn, fc, d, ids, ic, fv, onAdd);
}
bool ApiQueueGameTask(HagUI_GameTaskFn fn, void* ctx) {
    return sow::QueueGameTask(fn, ctx);
}
void ApiSetPageOnFirstOpenInSave(int p, HagUI_PageOpenFn fn) {
    sow::HagUI::Get().SetPageOnFirstOpenInSave(p, fn);
}
void ApiAddConditionalButton(int p, const char* label, void (*click)(), bool (*visible)()) {
    sow::HagUI::Get().AddConditionalButton(p, label, click, visible);
}
void ApiAddHeaderButton(int p, const char* label, void (*click)(), bool (*visible)()) {
    sow::HagUI::Get().AddHeaderButton(p, label, click, visible);
}
void ApiAddSettingToggle(int p, const char* label, bool (*get)(), void (*set)(bool)) {
    sow::HagUI::Get().AddSettingToggle(p, label, get, set);
}
int ApiRegisterSubPage(int parent, const char* title) { return sow::HagUI::Get().RegisterSubPage(parent, title); }
void ApiAddSubPageButton(int p, const char* label, int child) { sow::HagUI::Get().AddSubPageButton(p, label, child); }
void ApiAddPropertyEditor(int p, void* context, const HagUI_PropertySource* source) { sow::HagUI::Get().AddPropertyEditor(p, context, source); }

void ApiAddPopupEditor(int p, void* c, const HagUI_PropertySource* s, const HagUI_PopupActions* a) { sow::HagUI::Get().AddPropertyEditor(p,c,s,a); }
void ApiAddSceneEditor(int p, void* c, const HagUI_PropertySource* s, const HagUI_PopupActions* a,HagUI_SceneFn scene) { sow::HagUI::Get().AddPropertyEditor(p,c,s,a,scene); }
HagUIAPI g_api = { HAGUI_ABI_VERSION, &ApiRegisterPage, &ApiAddLabel, &ApiAddToggle, &ApiAddButton,
                   &ApiAddList, &ApiAddFacetedList, &ApiAddFacetedActionList, &ApiQueueGameTask,
                   &ApiSetPageOnFirstOpenInSave, &ApiAddConditionalButton, &sow::CurrentSaveSession,
                   &ApiAddHeaderButton, &ApiAddSettingToggle, &ApiRegisterSubPage, &ApiAddSubPageButton, &ApiAddPropertyEditor, &ApiAddPopupEditor, &ApiAddSceneEditor };
}  // namespace

// Any ABI from 1..current is served the same struct: the layout is append-only, so a v1 caller simply
// reads the first five members and never touches AddList.
extern "C" __declspec(dllexport) HagUIAPI* HagUI_GetAPI(std::uint32_t abiVersion) {
    return (abiVersion >= 1 && abiVersion <= HAGUI_ABI_VERSION) ? &g_api : nullptr;
}
