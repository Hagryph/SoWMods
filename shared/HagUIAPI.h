#pragma once
// HagUIAPI.h - stable, versioned, flat extern "C" cross-plugin ABI for SoW HagUI.
//
// HagUI (hosted in the SoWLoader proxy, steam_api64.dll) exports HagUI_GetAPI. Any other SoW mod
// registers its own option page WITHOUT linking HagUI: resolve the API at runtime with
//
//   auto get = (HagUI_GetAPI_t)GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "HagUI_GetAPI");
//   HagUIAPI* ui = get ? get(HAGUI_ABI_VERSION) : nullptr;
//   if (ui) { int p = ui->RegisterPage("My Mod"); ui->AddToggle(p, "Enable X", &gEnableX); }
//
// No C++/STL types cross the boundary - only C primitives + function pointers.
#include <cstdint>

extern "C" {

constexpr std::uint32_t HAGUI_ABI_VERSION = 14;

typedef void (*HagUI_GameTaskFn)(void* ctx);
typedef void (*HagUI_PageOpenFn)(int page);

struct HagUI_Property {
    std::uint64_t id;
    char label[96];
    char value[1024];
    bool editable;
    char readOnlyReason[192];
};
struct HagUI_Choice {
    std::uint64_t id;
    char label[256];
    bool selected;
};
// Callbacks read cached snapshots only. Native access belongs in QueueGameTask.
struct HagUI_PropertySource {
    void (*refresh)(void* context);
    int (*count)(void* context);
    bool (*read)(void* context, int index, HagUI_Property* property);
    int (*choiceCount)(void* context, std::uint64_t field);
    bool (*choice)(void* context, std::uint64_t field, int index, HagUI_Choice* choice);
    bool (*apply)(void* context, std::uint64_t field, std::uint64_t choice);
};

// Optional popup/draft controls, separate from the stable v10 property structs.
struct HagUI_PopupState {
    bool generator, busy, dirty, canSave, needsRepair, unavailable;
    char status[256];
    char preview[4096];
    std::uint32_t palette[17]; // ARGB; transparent cells are not displayed
};
struct HagUI_PopupActions {
    const char* saveLabel;
    void (*state)(void*, std::uint64_t field, std::uint64_t choice, HagUI_PopupState*);
    bool (*generate)(void*, std::uint64_t field);
    void (*save)(void*);
    void (*discard)(void*);
    void (*close)(void*, std::uint64_t field);
};
// 0: no scene, 1: message only, 2: show the current native scene. A zero
// field ends the preview (including hub close); callbacks never render or block.
using HagUI_SceneFn = int (*)(void*, std::uint64_t field, std::uint64_t choice, char (&message)[192]);

struct HagUI_SetEditor {
    bool (*handles)(void*,std::uint64_t field);
    bool (*compatible)(void*,std::uint64_t field,std::uint64_t candidate,const std::uint64_t* selected,int count);
    bool (*apply)(void*,std::uint64_t field,const std::uint64_t* selected,int count);
};
struct HagUIAPI {
    std::uint32_t abiVersion;                              // == HAGUI_ABI_VERSION
    int  (*RegisterPage)(const char* title);               // -> page handle (>=0), or -1 on failure
    void (*AddLabel)(int page, const char* text);
    void (*AddToggle)(int page, const char* label, bool* value);   // HagUI reads/writes *value
    void (*AddButton)(int page, const char* label, void (*onClick)());
    // v2: a searchable + filterable scrolling list. items[i] is the row text; cats[i] its filter
    // bucket (nullptr/"" = none). Implemented as a one-facet AddFacetedList. Kept for simple mods.
    void (*AddList)(int page, const char* const* items, const char* const* cats, int count);
    // v3: a searchable, MULTI-FACET-filtered, grouped list. facetNames[0..facetCount) name the filter
    // facets; facet 0 is the group header, facet 1 the sub-group header. displays[0..itemCount) are the
    // row labels. facetValues is ROW-MAJOR itemCount x facetCount (item i, facet f => [i*facetCount+f]);
    // a ""/"-" value means the item has no value for that facet (it isn't offered as an option and is
    // excluded only when that facet has an active selection). Filtering is OR within a facet, AND across
    // facets, plus a case-insensitive substring search over the display text. HagUI COPIES everything;
    // the caller's arrays need not outlive the call. Feed items pre-sorted by facet 0 then 1 for tidy
    // group headers.
    void (*AddFacetedList)(int page, const char* const* facetNames, int facetCount,
                           const char* const* displays, int itemCount,
                           const char* const* facetValues);
    // v4: same visual/list behavior as AddFacetedList, but each row carries a stable caller-owned
    // id copied by HagUI. Clicking a row opens a count prompt and then calls onAdd(id, count).
    void (*AddFacetedActionList)(int page, const char* const* facetNames, int facetCount,
                                 const char* const* displays, const char* const* ids, int itemCount,
                                 const char* const* facetValues,
                                 void (*onAdd)(const char* id, int count));
    // v5: queue work that must run from a game-owned hook point, not from the Present/ImGui UI path.
    // The callback is invoked later in-process with the same ctx pointer; ownership is caller-defined.
    bool (*QueueGameTask)(HagUI_GameTaskFn fn, void* ctx);
    // v6: lazy save-local page init. The loader calls this once for the page when F8 is first opened
    // while a save/world is loaded, before the page's widgets are drawn.
    void (*SetPageOnFirstOpenInSave)(int page, HagUI_PageOpenFn fn);
    // v7: evaluated on the UI thread before drawing or activating the button. Keep the
    // predicate nonblocking; queue native queries with QueueGameTask, never call them here.
    void (*AddConditionalButton)(int page, const char* label, void (*onClick)(), bool (*visible)());
    // Nonzero identity for the currently loaded world; changes on each world load.
    // Zero at the main menu. Queued tasks can reject work from an earlier save.
    std::uint64_t (*SaveSession)();
    // v8: conditional action on the title line, right-aligned; consumes no content row.
    void (*AddHeaderButton)(int page, const char* label, void (*onClick)(), bool (*visible)());
    // v9: callback-backed toggle for atomic settings and immediate persistence on change.
    void (*AddSettingToggle)(int page, const char* label, bool (*get)(), void (*set)(bool));
    // v10: nested pages stay out of the tab bar; Back returns to their parent screen.
    int (*RegisterSubPage)(int parent, const char* title);
    void (*AddSubPageButton)(int page, const char* label, int child);
    void (*AddPropertyEditor)(int page, void* context, const HagUI_PropertySource* source);
    void (*AddPopupEditor)(int page, void* context, const HagUI_PropertySource*, const HagUI_PopupActions*);
    void (*AddSceneEditor)(int page, void* context, const HagUI_PropertySource*, const HagUI_PopupActions*, HagUI_SceneFn);
    void (*AddSetEditor)(int,void*,const HagUI_PropertySource*,const HagUI_PopupActions*,HagUI_SceneFn,const HagUI_SetEditor*);
    // v14: promotes a generator to the submenu footer; query returns its ID or zero if disabled.
    void (*SetEditorFooterGenerator)(int page,const char* label,std::uint64_t (*query)(void*));
};

// Exported by the loader DLL. Returns null if the requested ABI version is unsupported.
typedef HagUIAPI* (*HagUI_GetAPI_t)(std::uint32_t abiVersion);

}  // extern "C"
