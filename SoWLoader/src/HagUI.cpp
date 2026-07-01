#include "HagUI.h"
#include "Overlay.h"
#include "Log.h"

namespace sow {

HagUI& HagUI::Get() { static HagUI h; return h; }

// --- built-in demo page (proves the API + interactivity) ---
static bool g_demoToggle = false;
static void DemoButton() { Log::Get().Line("[hagui] demo button clicked"); }

void HagUI::Init() {
    if (inited_) return;
    inited_ = true;
    int p = RegisterPage("General");
    AddLabel(p, "SoW HagUI framework - cross-plugin page API");
    AddToggle(p, "Example toggle", &g_demoToggle);
    AddButton(p, "Log a message", &DemoButton);
    Log::Get().Line("[hagui] initialized (demo page 'General'); press F8 to open the hub");
}

int HagUI::RegisterPage(const char* title) {
    pages_.push_back(Page{ title ? title : "", {} });
    return (int)pages_.size() - 1;
}
void HagUI::AddLabel(int page, const char* text) {
    if (page >= 0 && page < (int)pages_.size()) pages_[page].widgets.push_back({ WLabel, text ? text : "", nullptr, nullptr });
}
void HagUI::AddToggle(int page, const char* label, bool* value) {
    if (page >= 0 && page < (int)pages_.size()) pages_[page].widgets.push_back({ WToggle, label ? label : "", value, nullptr });
}
void HagUI::AddButton(int page, const char* label, void (*onClick)()) {
    if (page >= 0 && page < (int)pages_.size()) pages_[page].widgets.push_back({ WButton, label ? label : "", nullptr, onClick });
}

void HagUI::PollInput() {
    auto d = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool cur[7] = { d(VK_F8), d(VK_UP), d(VK_DOWN), d(VK_LEFT), d(VK_RIGHT), d(VK_RETURN), d(VK_SPACE) };
    auto pressed = [&](int i) { return cur[i] && !prev_[i]; };

    if (pressed(0)) open_ = !open_;
    if (open_ && !pages_.empty()) {
        const int n = (int)pages_[page_].widgets.size();
        if (pressed(1) && n > 0) sel_ = (sel_ - 1 + n) % n;
        if (pressed(2) && n > 0) sel_ = (sel_ + 1) % n;
        if (pressed(3)) { page_ = (page_ - 1 + (int)pages_.size()) % (int)pages_.size(); sel_ = 0; }
        if (pressed(4)) { page_ = (page_ + 1) % (int)pages_.size(); sel_ = 0; }
        if (pressed(5) || pressed(6)) {
            if (sel_ >= 0 && sel_ < n) {
                Widget& w = pages_[page_].widgets[sel_];
                if (w.type == WToggle && w.toggle)      *w.toggle = !*w.toggle;
                else if (w.type == WButton && w.onClick) w.onClick();
            }
        }
    }
    ::memcpy(prev_, cur, sizeof(prev_));
}

void HagUI::Render(Overlay& r) {
    PollInput();
    DrawMenuEntry(r);
    if (open_) DrawHub(r);
}

void HagUI::DrawMenuEntry(Overlay& r) {
    // Dummy "HagUI" entry beneath the native START list. Positions scale from a 1440p reference so
    // it lands in roughly the right place at other resolutions. (Gold = clearly ours.)
    float s = r.Height() / 1440.0f; if (s <= 0) s = 1.0f;
    r.DrawText(52.0f * s, 352.0f * s, "HagUI", (int)(30 * s), { 0.878f, 0.702f, 0.290f, 1.0f });
    r.DrawText(52.0f * s, 352.0f * s + 33.0f * s, "[F8] open", (int)(15 * s), { 0.60f, 0.60f, 0.63f, 0.9f });
}

void HagUI::DrawHub(Overlay& r) {
    const float W = r.Width(), H = r.Height();
    const float pw = 560, ph = 430, px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    r.DrawRect(px, py, pw, ph, { 0.04f, 0.04f, 0.05f, 0.93f });          // panel background
    r.DrawRect(px, py, pw, 42, { 0.878f, 0.702f, 0.290f, 1.0f });        // gold title bar
    std::string title = pages_.empty() ? "HagUI" : ("HagUI   -   " + pages_[page_].title);
    r.DrawText(px + 16, py + 9, title, 24, { 0.06f, 0.06f, 0.06f, 1.0f });
    r.DrawText(px + 16, py + 54, "[<-/->] page    [up/down] select    [enter] activate    [F8] close",
               15, { 0.62f, 0.62f, 0.66f, 1.0f });

    float y = py + 92;
    if (!pages_.empty()) {
        Page& pg = pages_[page_];
        for (int i = 0; i < (int)pg.widgets.size(); ++i) {
            Widget& w = pg.widgets[i];
            const bool selrow = (i == sel_);
            if (selrow) r.DrawRect(px + 10, y - 4, pw - 20, 34, { 1, 1, 1, 0.10f });
            const Color c = selrow ? Color{ 1, 1, 1, 1 } : Color{ 0.82f, 0.82f, 0.86f, 1 };
            std::string line = w.text;
            if (w.type == WToggle)      line += (w.toggle && *w.toggle) ? "     [ON]" : "     [OFF]";
            else if (w.type == WButton) line = "> " + line;
            r.DrawText(px + 22, y, line, 22, c);
            y += 38;
        }
    } else {
        r.DrawText(px + 22, y, "(no pages registered)", 22, { 0.7f, 0.7f, 0.7f, 1 });
    }
}

}  // namespace sow
