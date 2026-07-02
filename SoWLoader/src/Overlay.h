#pragma once
#include "PCH.h"
#include <string>
#include <unordered_map>

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11Buffer;
struct ID3D11SamplerState;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11ShaderResourceView;

namespace sow {

struct Color { float r, g, b, a; };

// D3D11 renderer: MinHook-trampolines IDXGISwapChain::Present, then each frame runs an immediate-mode
// pass (DrawRect / DrawText) on the GAME's own device with full pipeline state save/restore. HagUI
// draws through these helpers; a watermark is drawn here too.
class Overlay {
public:
    static Overlay& Get();
    void Install();

    // Immediate-mode helpers — valid only while rendering inside the Present hook (HagUI::Render).
    // Coordinates are in back-buffer pixels; origin top-left.
    void  DrawRect(float x, float y, float w, float h, Color c);
    float DrawText(float x, float y, const std::string& utf8, int px, Color c,
                   const char* font = nullptr, bool bold = true);  // returns pixel width
    void  MeasureText(const std::string& utf8, int px, float& w, float& h,
                      const char* font = nullptr, bool bold = true);
    // Upload an RGBA8 buffer to a texture (cached by key; generated once) and draw it stretched.
    // Use for procedurally-baked chrome (gradients, rounded card, glow) that stays constant.
    void  DrawImage(const std::string& key, float x, float y, float w, float h,
                    const uint32_t* rgba, int tw, int th, Color tint = {1, 1, 1, 1});
    bool  HasImage(const std::string& key) const;
    float Width()  const { return curW_; }
    float Height() const { return curH_; }

    // Warm mode: all Draw* calls create/cache their textures but render nothing. Used once at
    // startup to prebake the HagUI chrome + glyphs so the first F8 opens without a hitch.
    void  BeginWarm() { warming_ = true; }
    void  EndWarm()   { warming_ = false; }

    // Mouse position in back-buffer coordinates (client area of the game window).
    bool  Mouse(float& x, float& y) const;

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

private:
    Overlay() = default;
    using PresentFn = long (__stdcall*)(IDXGISwapChain*, unsigned, unsigned);
    static long __stdcall HookPresent(IDXGISwapChain*, unsigned, unsigned);
    void DrawFrame(IDXGISwapChain* swap);   // per-frame: RTV + state save/restore + draw

    bool BuildResources(ID3D11Device* dev);
    struct Glyph { ID3D11ShaderResourceView* srv; int w, h; };
    const Glyph* GetGlyph(const std::string& utf8, int px, const char* font, bool bold);
    void DrawQuad(ID3D11ShaderResourceView* srv, float x, float y, float w, float h, Color c);
    struct Image { ID3D11ShaderResourceView* srv; int w, h; };

    static inline PresentFn oPresent_ = nullptr;
    bool installed_ = false, resTried_ = false, resReady_ = false, loggedDraw_ = false;
    bool warming_ = false, firstFrame_ = true;
    HWND gameWnd_ = nullptr;

    ID3D11Device*        dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11VertexShader*       vs_       = nullptr;
    ID3D11PixelShader*        ps_       = nullptr;
    ID3D11InputLayout*        layout_   = nullptr;
    ID3D11Buffer*             vb_       = nullptr;   // 4 verts, rewritten per quad
    ID3D11Buffer*             cb_       = nullptr;   // float4 tint
    ID3D11SamplerState*       samp_     = nullptr;
    ID3D11BlendState*         blend_    = nullptr;
    ID3D11DepthStencilState*  depthOff_ = nullptr;
    ID3D11RasterizerState*    raster_   = nullptr;
    ID3D11ShaderResourceView* white_    = nullptr;   // 1x1 white, for solid rects

    std::unordered_map<std::wstring, Glyph> glyphs_;
    std::unordered_map<std::string, Image>  images_;   // procedurally-baked chrome, keyed by name
    float curW_ = 0, curH_ = 0;
};

}  // namespace sow
