#pragma once
#include "PCH.h"
#include <string>
#include <vector>

namespace sow {

class Overlay;

// The HagUI framework: an overlay-drawn UI hub with a cross-plugin page-registration API (exposed
// as a flat C ABI via HagUI_GetAPI, see shared/HagUIAPI.h). Draws a "HagUI" menu entry and, on F8,
// a hub listing every registered page and its widgets.
class HagUI {
public:
    static HagUI& Get();

    void Init();                 // called once when the overlay installs (registers the demo page)
    void Render(Overlay& r);     // called each frame from the Present hook
    void Prebake(Overlay& r);    // warm-mode pass: bakes every chrome texture + glyph up front

    // Page API (mirrors HagUIAPI; the exported C functions delegate here).
    int  RegisterPage(const char* title);
    void SetPageScope(int page, int scope);   // SOWMOD_GLOBAL / SOWMOD_LOCAL (see shared/SoWModAPI.h)
    void AddLabel(int page, const char* text);
    void AddToggle(int page, const char* label, bool* value);
    void AddButton(int page, const char* label, void (*onClick)());
    void AddList(int page, const char* const* items, const char* const* cats, int count);
    void AddFacetedList(int page, const char* const* facetNames, int facetCount,
                        const char* const* displays, int itemCount, const char* const* facetValues);
    void AddFacetedActionList(int page, const char* const* facetNames, int facetCount,
                              const char* const* displays, const char* const* ids, int itemCount,
                              const char* const* facetValues, void (*onAdd)(const char* id, int count));

    // Read access for the renderer (Overlay::DrawHub): tabs exist ONLY for registered pages.
    enum WType { WLabel = 0, WToggle = 1, WButton = 2, WList = 3 };
    // One filterable dimension: its distinct values + a live multi-select mask (parallel to opts).
    struct Facet {
        std::string name;
        std::vector<std::string> opts;      // distinct values, first-seen order
        mutable std::vector<char> sel;      // selected flag per opt (mutated by the renderer)
        int selCount() const { int n = 0; for (char c : sel) n += c ? 1 : 0; return n; }
    };
    struct Widget {
        WType type; std::string text; bool* toggle; void (*onClick)();
        // WList payload (set once) + live UI state (mutated by the renderer each frame).
        std::vector<std::string>      items;         // row display strings
        std::vector<std::string>      itemIds;       // optional stable row ids for action lists
        std::vector<std::vector<int>> itemFacetIdx;  // [item][facet] -> opt index in facets[f], or -1
        std::vector<Facet>            facets;        // facet defs + live selection masks
        void (*onItemAdd)(const char* id, int count) = nullptr;
        mutable int  listSel   = -1;                 // selected row (-1 = none)
        mutable int  actionSel = -1;                 // row pending in the count popup
        mutable int  actionCount = 1;
        mutable char search[64] = {};                // search box text (ImGui InputText buffer)
    };
    struct Page   { std::string title; int scope = 0; std::vector<Widget> widgets; };  // scope: 0=global,1=local
    const std::vector<Page>& Pages() const { return pages_; }

    HagUI(const HagUI&) = delete;
    HagUI& operator=(const HagUI&) = delete;

private:
    HagUI() = default;
    void PollInput(Overlay& r);
    void DrawHub(Overlay& r);

    std::vector<Page> pages_;
    bool inited_ = false, open_ = false;
    int  tab_ = 0, sel_ = 0;   // tab_ 0 = WELCOME landing, 1..N = registered pages
    bool prev_[9] = {};        // F8, Up, Down, Left, Right, Enter, Space, Esc, LMB (edge detection)
    float mx_ = -1, my_ = -1;  // mouse in back-buffer px (-1 = unavailable)
    bool  click_ = false;      // LMB pressed this frame (edge, consumed by DrawHub)
};

}  // namespace sow
