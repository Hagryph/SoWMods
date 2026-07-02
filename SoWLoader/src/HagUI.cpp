#include "HagUI.h"
#include "Overlay.h"
#include "Log.h"

#include <cmath>
#include <cctype>
#include <vector>
#include <string>

namespace sow {

HagUI& HagUI::Get() { static HagUI h; return h; }

// ============================================================================
//  Palette (1:1 with SkyrimMods/HagUI/assets/HagUI_Root.as)
// ============================================================================
namespace {
struct RGB { float r, g, b; };
constexpr RGB kBg0   { 0x0A / 255.f, 0x0A / 255.f, 0x0C / 255.f };  // backdrop bottom
constexpr RGB kBg1   { 0x12 / 255.f, 0x10 / 255.f, 0x13 / 255.f };  // backdrop top
constexpr RGB kPanel { 0x1A / 255.f, 0x17 / 255.f, 0x12 / 255.f };  // hero card
constexpr RGB kPanel2{ 0x23 / 255.f, 0x1E / 255.f, 0x16 / 255.f };  // insets / widgets
constexpr RGB kWarm  { 0x24 / 255.f, 0x1D / 255.f, 0x0F / 255.f };  // top-right glow
constexpr RGB kAccent{ 0xE0 / 255.f, 0xB3 / 255.f, 0x4A / 255.f };  // gold
constexpr RGB kAccD  { 0xB8 / 255.f, 0x86 / 255.f, 0x2F / 255.f };  // gold dim
constexpr RGB kText  { 0xEC / 255.f, 0xE6 / 255.f, 0xDA / 255.f };  // primary text
constexpr RGB kTextD { 0x9C / 255.f, 0x94 / 255.f, 0x86 / 255.f };  // dim text
constexpr RGB kTextF { 0x6B / 255.f, 0x64 / 255.f, 0x56 / 255.f };  // faint text

inline Color C(RGB c, float a = 1.0f) { return { c.r, c.g, c.b, a }; }

inline uint32_t Pack(float r, float g, float b, float a) {
    auto q = [](float v) { int i = (int)(v * 255.0f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
    return (uint32_t)q(r) | ((uint32_t)q(g) << 8) | ((uint32_t)q(b) << 16) | ((uint32_t)q(a) << 24);
}
inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Signed distance to a rounded rect [0..w,0..h] (negative inside), point at pixel centre.
float RRSDF(float px, float py, float w, float h, float rad) {
    float qx = std::fabs(px - w * 0.5f) - (w * 0.5f - rad);
    float qy = std::fabs(py - h * 0.5f) - (h * 0.5f - rad);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    float outside = std::sqrt(ax * ax + ay * ay);
    float inside  = std::fmin(std::fmax(qx, qy), 0.0f);
    return outside + inside - rad;
}

// ---- bake once into the overlay's keyed cache (zero-size draw = cache only, no pixels) ----

void EnsureBackdrop(Overlay& r, const char* key) {
    if (r.HasImage(key)) return;
    const int tw = 320, th = 180;
    std::vector<uint32_t> buf((size_t)tw * th);
    // radial warm glow centred near top-right (from AS glow box, normalised)
    const float gcx = 0.865f, gcy = 0.131f, grx = 0.62f, gry = 0.88f;
    for (int y = 0; y < th; ++y) {
        float v = (y + 0.5f) / th;
        for (int x = 0; x < tw; ++x) {
            float u = (x + 0.5f) / tw;
            float cr = Lerp(kBg1.r, kBg0.r, v);
            float cg = Lerp(kBg1.g, kBg0.g, v);
            float cb = Lerp(kBg1.b, kBg0.b, v);
            float dx = (u - gcx) / grx, dy = (v - gcy) / gry;
            float g = Clamp01(1.0f - std::sqrt(dx * dx + dy * dy));
            g = g * g * 0.85f;
            cr += kWarm.r * g; cg += kWarm.g * g; cb += kWarm.b * g;
            buf[(size_t)y * tw + x] = Pack(cr, cg, cb, 1.0f);
        }
    }
    r.DrawImage(key, 0, 0, 0, 0, buf.data(), tw, th);   // cache only
}

// Hero card: rounded panel + gold hairline border + left accent rail (glow + bright bar).
void EnsureCard(Overlay& r, const char* key) {
    if (r.HasImage(key)) return;
    const int ss = 2;
    const int CW = (int)(820 * ss), CH = (int)(462 * ss);
    const float R = 14.0f * ss, BW = 1.6f * ss;
    const float railBar = 6.0f * ss, railGlow = 30.0f * ss;
    std::vector<uint32_t> buf((size_t)CW * CH);
    for (int y = 0; y < CH; ++y) {
        for (int x = 0; x < CW; ++x) {
            float sdf = RRSDF(x + 0.5f, y + 0.5f, (float)CW, (float)CH, R);
            float cov = Clamp01(0.5f - sdf);
            if (cov <= 0) { buf[(size_t)y * CW + x] = 0; continue; }
            float cr = kPanel.r, cg = kPanel.g, cb = kPanel.b, ca = 0.97f;
            // left accent rail
            float lx = (float)x;
            float glow = Clamp01(1.0f - lx / railGlow); glow *= glow * 0.26f;
            cr = Lerp(cr, kAccent.r, glow); cg = Lerp(cg, kAccent.g, glow); cb = Lerp(cb, kAccent.b, glow);
            if (lx < railBar) {
                float t = (float)y / CH;
                cr = Lerp(kAccent.r, kAccD.r, t);
                cg = Lerp(kAccent.g, kAccD.g, t);
                cb = Lerp(kAccent.b, kAccD.b, t);
                ca = 1.0f;
            }
            // gold hairline border
            float innerCov = Clamp01(0.5f - (sdf + BW));
            float bf = Clamp01(cov - innerCov);
            cr = Lerp(cr, kAccent.r, bf * 0.85f);
            cg = Lerp(cg, kAccent.g, bf * 0.85f);
            cb = Lerp(cb, kAccent.b, bf * 0.85f);
            buf[(size_t)y * CW + x] = Pack(cr, cg, cb, ca * cov);
        }
    }
    r.DrawImage(key, 0, 0, 0, 0, buf.data(), CW, CH);   // cache only
}

// Generic rounded pill (fill + gold border), used for buttons / checkboxes.
// `vw`,`vh` are the pill's virtual (1280x720) size; baked at a fixed high resolution.
void EnsurePill(Overlay& r, const char* key, float vw, float vh,
                float vrad, RGB fill, float fillA, float borderA) {
    if (r.HasImage(key)) return;
    const int ss = 3;
    const int PW = (int)(vw * ss), PH = (int)(vh * ss);
    const float R = vrad * ss, BW = 1.4f * ss;
    std::vector<uint32_t> buf((size_t)PW * PH);
    for (int y = 0; y < PH; ++y) {
        for (int x = 0; x < PW; ++x) {
            float sdf = RRSDF(x + 0.5f, y + 0.5f, (float)PW, (float)PH, R);
            float cov = Clamp01(0.5f - sdf);
            if (cov <= 0) { buf[(size_t)y * PW + x] = 0; continue; }
            float cr = fill.r, cg = fill.g, cb = fill.b, ca = fillA;
            float innerCov = Clamp01(0.5f - (sdf + BW));
            float bf = Clamp01(cov - innerCov);
            cr = Lerp(cr, kAccent.r, bf * borderA);
            cg = Lerp(cg, kAccent.g, bf * borderA);
            cb = Lerp(cb, kAccent.b, bf * borderA);
            float a = std::fmax(ca, bf * borderA) * cov;
            buf[(size_t)y * PW + x] = Pack(cr, cg, cb, a);
        }
    }
    r.DrawImage(key, 0, 0, 0, 0, buf.data(), PW, PH);   // cache only
}
}  // namespace

void HagUI::Init() {
    if (inited_) return;
    inited_ = true;
    // NO pages are registered here: tabs exist only for pages registered by mods through the
    // cross-plugin API (HagUI_GetAPI). The hub always has the WELCOME landing tab.
    // HagUI logs through its own channel: logs\HagUI.log + [HagUI]-prefixed console lines.
    Log::Channel("HagUI").Line("initialized (no built-in pages; mods register tabs); press F8 to open the hub");
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
void HagUI::AddList(int page, const char* const* items, const char* const* cats, int count) {
    if (page < 0 || page >= (int)pages_.size() || count < 0) return;
    Widget w{}; w.type = WList; w.toggle = nullptr; w.onClick = nullptr;
    w.items.reserve(count); w.cats.reserve(count);
    for (int i = 0; i < count; ++i) {
        w.items.emplace_back(items && items[i] ? items[i] : "");
        w.cats.emplace_back(cats && cats[i] ? cats[i] : "");
    }
    // filter dropdown = "All" + the distinct buckets in first-seen order
    w.filters.emplace_back("All");
    for (const auto& c : w.cats) {
        if (c.empty()) continue;
        bool seen = false;
        for (const auto& f : w.filters) if (f == c) { seen = true; break; }
        if (!seen) w.filters.push_back(c);
    }
    pages_[page].widgets.push_back(std::move(w));
}

void HagUI::PollInput(Overlay& r) {
    auto d = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool cur[9] = { d(VK_F8), d(VK_UP), d(VK_DOWN), d(VK_LEFT), d(VK_RIGHT),
                          d(VK_RETURN), d(VK_SPACE), d(VK_ESCAPE), d(VK_LBUTTON) };
    auto pressed = [&](int i) { return cur[i] && !prev_[i]; };

    if (!r.Mouse(mx_, my_)) { mx_ = my_ = -1; }
    click_ = open_ && pressed(8);

    if (pressed(0)) open_ = !open_;
    if (open_ && pressed(7)) open_ = false;                 // ESC closes
    if (open_) {
        const int tabs = 1 + (int)pages_.size();
        if (pressed(3)) { tab_ = (tab_ - 1 + tabs) % tabs; sel_ = 0; }   // <-
        if (pressed(4)) { tab_ = (tab_ + 1) % tabs; sel_ = 0; }          // ->
        if (tab_ >= 1 && tab_ <= (int)pages_.size()) {
            const int n = (int)pages_[tab_ - 1].widgets.size();
            if (pressed(1) && n > 0) sel_ = (sel_ - 1 + n) % n;
            if (pressed(2) && n > 0) sel_ = (sel_ + 1) % n;
            if ((pressed(5) || pressed(6)) && sel_ >= 0 && sel_ < n) {
                Widget& w = pages_[tab_ - 1].widgets[sel_];
                if (w.type == WToggle && w.toggle)       *w.toggle = !*w.toggle;
                else if (w.type == WButton && w.onClick)  w.onClick();
            }
        }
    }
    ::memcpy(prev_, cur, sizeof(prev_));
}

void HagUI::Render(Overlay& r) {
    PollInput(r);
    if (open_) DrawHub(r);
}

// Warm-mode pass (Overlay::BeginWarm active): walks every tab so all chrome textures and glyph
// strings bake up front — the first real F8 then renders from cache with no hitch.
void HagUI::Prebake(Overlay& r) {
    const bool wasOpen = open_; const int wasTab = tab_;
    open_ = true;
    EnsurePill(r, "cb_on",  22, 22, 5.0f, kAccent, 0.92f, 0.0f);
    EnsurePill(r, "cb_off", 22, 22, 5.0f, kPanel2, 0.85f, 0.55f);
    { // the check glyph only draws when a toggle is on — warm it explicitly
        int p18 = (int)(18.0f * r.Height() / 720.0f + 0.5f); if (p18 < 1) p18 = 1;
        float gw, gh; r.MeasureText("\xE2\x9C\x93", p18, gw, gh);
    }
    for (int t = 0; t <= (int)pages_.size(); ++t) { tab_ = t; DrawHub(r); }
    open_ = wasOpen; tab_ = wasTab;
}

// ============================================================================
//  The hub — full-screen 1:1 replica of the Skyrim HagUI Scaleform UI
// ============================================================================
void HagUI::DrawHub(Overlay& r) {
    const float W = r.Width(), H = r.Height();
    float s = H / 720.0f; if (s <= 0) s = 1.0f;
    const float ox = (W - 1280.0f * s) * 0.5f;
    auto X = [&](float vx) { return ox + vx * s; };
    auto Y = [&](float vy) { return vy * s; };
    auto S = [&](float v)  { return v * s; };
    auto P = [&](float v)  { int i = (int)(v * s + 0.5f); return i < 1 ? 1 : i; };
    const char* kSerif = "Georgia";

    // --- backdrop (opaque, covers the game) ---
    EnsureBackdrop(r, "bg");
    r.DrawImage("bg", 0, 0, W, H, nullptr, 0, 0);

    // --- hero card ---
    const float cx = 230, cy = 123, cw = 820, ch = 462;
    EnsureCard(r, "card");
    // soft drop shadow (reuse the card's rounded alpha shape, darkened + offset)
    r.DrawImage("card", X(cx) + S(7), Y(cy) + S(10), S(cw), S(ch), nullptr, 0, 0, { 0, 0, 0, 0.28f });
    r.DrawImage("card", X(cx),        Y(cy),         S(cw), S(ch), nullptr, 0, 0);

    const float pxi = cx + 60;      // inner content x
    const float cwid = cw - 108;

    // --- corner flourishes (thin gold L's @30%) ---
    Color fl = C(kAccent, 0.30f);
    r.DrawRect(X(cx + 30), Y(cy + 22), S(26), P(2), fl);
    r.DrawRect(X(cx + 30), Y(cy + 22), P(2), S(18), fl);
    r.DrawRect(X(cx + cw - 56), Y(cy + ch - 24), S(26), P(2), fl);
    r.DrawRect(X(cx + cw - 32), Y(cy + ch - 40), P(2), S(18), fl);

    // --- nav tabs (hover + click) ---
    auto hit = [&](float hx, float hy, float hw2, float hh2) {
        return mx_ >= hx && mx_ <= hx + hw2 && my_ >= hy && my_ <= hy + hh2;
    };
    const float ny = cy + 28;
    float tx = pxi;
    std::vector<std::string> tabs; tabs.push_back("WELCOME");
    for (auto& pg : pages_) {
        std::string t = pg.title; for (auto& ch2 : t) ch2 = (char)::toupper((unsigned char)ch2);
        tabs.push_back(t);
    }
    for (int i = 0; i < (int)tabs.size(); ++i) {
        const bool active = (i == tab_);
        float tw2, th2; r.MeasureText(tabs[i], P(15), tw2, th2);
        const bool hov = hit(X(tx), Y(ny) - S(4), tw2, th2 + S(8));
        if (hov && click_) { tab_ = i; sel_ = 0; }
        r.DrawText(X(tx), Y(ny), tabs[i], P(15),
                   active ? C(kAccent) : (hov ? C(kText) : C(kTextD)));
        const float twv = tw2 / s;
        if (active)      r.DrawRect(X(tx), Y(ny + 22), S(twv), P(2), C(kAccent));
        else if (hov)    r.DrawRect(X(tx), Y(ny + 22), S(twv), P(2), C(kAccent, 0.35f));
        tx += twv + 34;
    }
    r.DrawRect(X(pxi), Y(ny + 34), S(cwid), P(1), C(kAccent, 0.14f));   // baseline hairline

    // --- content ---
    const float cyTop = cy + 86;
    if (tab_ == 0 || pages_.empty()) {
        // WELCOME landing
        r.DrawText(X(pxi), Y(cyTop), "W E L C O M E   T O", P(13), C(kTextF), kSerif, false);
        float wx = X(pxi);
        wx += r.DrawText(wx, Y(cyTop + 20), "Hag", P(64), C(kText), kSerif, true);
        r.DrawText(wx, Y(cyTop + 20), "UI", P(64), C(kAccent), kSerif, true);
        r.DrawRect(X(pxi), Y(cyTop + 108), S(250), P(2), C(kAccent, 0.85f));
        r.DrawText(X(pxi), Y(cyTop + 126),
                   "Your private control room for every Hagryph mod - configuration,",
                   P(18), C(kTextD), kSerif, false);
        r.DrawText(X(pxi), Y(cyTop + 152),
                   "tools, and more, gathered in one place.",
                   P(18), C(kTextD), kSerif, false);
    } else {
        // registered page: render widgets (keyboard sel_ + mouse hover/click)
        Page& pg = pages_[tab_ - 1];
        float ry = cyTop + 6;
        for (int i = 0; i < (int)pg.widgets.size(); ++i) {
            Widget& w = pg.widgets[i];
            const bool hov = w.type != WLabel &&
                             hit(X(pxi - 14), Y(ry - 6), S(cwid + 28), S(40));
            if (hov) {
                sel_ = i;
                if (click_) {
                    if (w.type == WToggle && w.toggle)       *w.toggle = !*w.toggle;
                    else if (w.type == WButton && w.onClick)  w.onClick();
                }
            }
            const bool selrow = (i == sel_);
            if (selrow || hov)
                r.DrawRect(X(pxi - 14), Y(ry - 6), S(cwid + 28), S(40), C(kAccent, hov ? 0.10f : 0.07f));
            Color txt = selrow ? C(kText) : C(kTextD);
            if (w.type == WToggle) {
                bool on = w.toggle && *w.toggle;
                const char* bkey = on ? "cb_on" : "cb_off";
                EnsurePill(r, bkey, 22, 22, 5.0f,
                           on ? kAccent : kPanel2, on ? 0.92f : 0.85f, on ? 0.0f : 0.55f);
                r.DrawImage(bkey, X(pxi), Y(ry - 2), S(22), S(22), nullptr, 0, 0);
                if (on) r.DrawText(X(pxi + 4), Y(ry - 5), "\xE2\x9C\x93", P(18), C(kBg0), nullptr, true);
                r.DrawText(X(pxi + 34), Y(ry), w.text, P(18), txt, kSerif, false);
            } else if (w.type == WButton) {
                r.DrawText(X(pxi), Y(ry), w.text, P(18), selrow ? C(kAccent) : C(kAccD), kSerif, true);
            } else {
                r.DrawText(X(pxi), Y(ry), w.text, P(18), C(kTextD), kSerif, false);
            }
            ry += 44;
        }
    }

    // --- CLOSE button (hover highlight + click) + ESC hint ---
    const float bx = pxi, by = cy + ch - 86, bw = 152, bh = 40;
    const bool closeHov = hit(X(bx), Y(by), S(bw), S(bh));
    if (closeHov && click_) { open_ = false; return; }
    EnsurePill(r, "btn_close",    bw, bh, 7.0f, kPanel2, 0.85f, 0.55f);
    EnsurePill(r, "btn_close_hi", bw, bh, 7.0f, RGB{ kAccent.r * 0.25f, kAccent.g * 0.25f, kAccent.b * 0.25f }, 0.95f, 1.0f);
    r.DrawImage(closeHov ? "btn_close_hi" : "btn_close", X(bx), Y(by), S(bw), S(bh), nullptr, 0, 0);
    {
        float lw; float lh; r.MeasureText("CLOSE", P(15), lw, lh, kSerif, true);
        r.DrawText(X(bx) + (S(bw) - lw) * 0.5f, Y(by) + (S(bh) - lh) * 0.5f, "CLOSE", P(15),
                   closeHov ? C(kText) : C(kAccent), kSerif, true);
    }
    r.DrawText(X(bx + bw + 16), Y(by + 12), "or press ESC", P(13), C(kTextF), kSerif, false);

    // --- footer, bottom-right of card (kept clear of the corner flourish) ---
    {
        const std::string ft = "HAGRYPH \xC2\xB7 EST. MMXXVI";
        float fw, fh; r.MeasureText(ft, P(11), fw, fh, kSerif, false);
        r.DrawText(X(cx + cw - 66) - fw, Y(cy + ch - 38), ft, P(11), C(kTextF), kSerif, false);
    }
}

}  // namespace sow
