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
    void AddLabel(int page, const char* text);
    void AddToggle(int page, const char* label, bool* value);
    void AddButton(int page, const char* label, void (*onClick)());

    // Read access for the renderer (Overlay::DrawHub): tabs exist ONLY for registered pages.
    enum WType { WLabel = 0, WToggle = 1, WButton = 2 };
    struct Widget { WType type; std::string text; bool* toggle; void (*onClick)(); };
    struct Page   { std::string title; std::vector<Widget> widgets; };
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
