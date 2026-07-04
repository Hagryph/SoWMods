#include "Overlay.h"
#include "Log.h"
#include "HagUI.h"
#include "GameOffsets.h"   // ../shared: game::FromRVA + kPauseToggle (call the game's own pause)
#include "Loader.h"
#include "SoWModAPI.h"   // ../shared: SOWMOD_LOCAL scope constant

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

// Provided by imgui_impl_win32 — feeds Win32 messages to ImGui (defined in the backend .cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace sow {

static std::string HexHR(HRESULT hr) { char b[32]{}; ::wsprintfA(b, "0x%08X", (unsigned)hr); return b; }

struct Vtx { float x, y, u, v; };   // NDC pos + uv

static const char* kVS =
    "struct VIn{ float2 pos:POSITION; float2 uv:TEXCOORD; };"
    "struct VOut{ float4 pos:SV_POSITION; float2 uv:TEXCOORD; };"
    "VOut main(VIn i){ VOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o; }";

static const char* kPS =
    "Texture2D tex:register(t0); SamplerState smp:register(s0);"
    "cbuffer Cb:register(b0){ float4 tint; };"
    "float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD):SV_TARGET{ return tex.Sample(smp,uv)*tint; }";

Overlay& Overlay::Get() { static Overlay o; return o; }

void Overlay::Install() {
    if (installed_) { return; }
    auto& log = Log::Get();

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr); wc.lpszClassName = L"SoWLoaderDummyWnd";
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { log.Line("[overlay] dummy window failed"); return; }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 }; D3D_FEATURE_LEVEL got{};
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                    want, 1, D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
    if (FAILED(hr) || !swap) { log.Line("[overlay] dummy device failed hr=" + HexHR(hr)); ::DestroyWindow(hwnd); return; }

    void** vtbl = *reinterpret_cast<void***>(swap);
    void* pPresent = vtbl[8];

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) log.Line("[overlay] MH_Initialize failed");
    if (MH_CreateHook(pPresent, reinterpret_cast<void*>(&HookPresent),
                      reinterpret_cast<void**>(&oPresent_)) == MH_OK && MH_EnableHook(pPresent) == MH_OK) {
        installed_ = true;
        log.Line("[overlay] Present hooked (vtable slot 8) — ImGui hub renders into the game's back buffer");
    } else {
        log.Line("[overlay] MinHook Present FAILED");
    }
    swap->Release(); if (ctx) ctx->Release(); if (dev) dev->Release(); ::DestroyWindow(hwnd);
}

// GDI-rasterize `text` (white on transparent) -> white RGBA with alpha=coverage (tinted at draw).
static bool RasterizeWhite(const wchar_t* text, int fontPx, std::vector<uint32_t>& out, int& w, int& h,
                           const wchar_t* face, bool bold) {
    HDC screen = ::GetDC(nullptr); HDC dc = ::CreateCompatibleDC(screen); ::ReleaseDC(nullptr, screen);
    if (!dc) return false;
    HFONT font = ::CreateFontW(fontPx, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, face ? face : L"Segoe UI");
    HGDIOBJ oldFont = ::SelectObject(dc, font);
    const int len = ::lstrlenW(text);
    SIZE ext{}; ::GetTextExtentPoint32W(dc, text, len, &ext);
    const int pad = 4; w = ext.cx + pad * 2; h = ext.cy + pad * 2;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr; HBITMAP dib = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { ::SelectObject(dc, oldFont); ::DeleteObject(font); ::DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = ::SelectObject(dc, dib);
    ::memset(bits, 0, (size_t)w * h * 4);
    ::SetBkMode(dc, TRANSPARENT); ::SetTextColor(dc, RGB(255, 255, 255));
    ::TextOutW(dc, pad, pad, text, len); ::GdiFlush();
    out.resize((size_t)w * h);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t p = src[i];
        const uint8_t cov = std::max((uint8_t)p, std::max((uint8_t)(p >> 8), (uint8_t)(p >> 16)));
        out[i] = 0x00FFFFFFu | ((uint32_t)cov << 24);   // white RGB, alpha = coverage
    }
    ::SelectObject(dc, oldBmp); ::DeleteObject(dib);
    ::SelectObject(dc, oldFont); ::DeleteObject(font); ::DeleteDC(dc);
    return true;
}

bool Overlay::BuildResources(ID3D11Device* dev) {
    auto& log = Log::Get();
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(::D3DCompile(kVS, ::strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsb, &err))) {
        log.Line("[overlay] VS compile failed"); if (err) err->Release(); return false; }
    if (FAILED(::D3DCompile(kPS, ::strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psb, &err))) {
        log.Line("[overlay] PS compile failed"); if (err) err->Release(); vsb->Release(); return false; }
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) {
        log.Line("[overlay] create VS/PS failed"); vsb->Release(); psb->Release(); return false; }
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    HRESULT hr = dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout_);
    vsb->Release(); psb->Release();
    if (FAILED(hr)) { log.Line("[overlay] input layout failed"); return false; }

    D3D11_BUFFER_DESC vbd{}; vbd.ByteWidth = sizeof(Vtx) * 4; vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER; vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&vbd, nullptr, &vb_))) { log.Line("[overlay] vb failed"); return false; }

    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cb_))) { log.Line("[overlay] cb failed"); return false; }

    D3D11_SAMPLER_DESC sdz{}; sdz.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdz.AddressU = sdz.AddressV = sdz.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sdz, &samp_);

    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &blend_);

    D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = FALSE; ds.StencilEnable = FALSE;
    dev->CreateDepthStencilState(&ds, &depthOff_);
    D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE; dev->CreateRasterizerState(&rs, &raster_);

    // 1x1 white texture for solid rects.
    uint32_t whitePx = 0xFFFFFFFF;
    D3D11_TEXTURE2D_DESC td{}; td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = &whitePx; srd.SysMemPitch = 4;
    ID3D11Texture2D* wtex = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &srd, &wtex)) && wtex) {
        dev->CreateShaderResourceView(wtex, nullptr, &white_); wtex->Release();
    }
    log.Line("[overlay] renderer resources ready");
    return white_ != nullptr;
}

const Overlay::Glyph* Overlay::GetGlyph(const std::string& utf8, int px, const char* font, bool bold) {
    wchar_t wbuf[1024]{};
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wbuf, 1024);
    wchar_t wface[128]{}; if (font) ::MultiByteToWideChar(CP_UTF8, 0, font, -1, wface, 128);
    std::wstring key = std::to_wstring(px); key += bold ? L'\x2' : L'\x1';
    if (font) { key += wface; } key += L'\x1'; key += wbuf;
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return &it->second;

    // Rasterize at 2x and box-downsample: GDI's grayscale AA alone looks pixelated at UI sizes.
    std::vector<uint32_t> hi; int hw = 0, hh = 0;
    if (!RasterizeWhite(wbuf, px * 2, hi, hw, hh, font ? wface : nullptr, bold) || hi.empty()) return nullptr;
    const int w = hw / 2, h = hh / 2;
    if (w <= 0 || h <= 0) return nullptr;
    std::vector<uint32_t> pix((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t a = (hi[(size_t)(y * 2) * hw + x * 2]     >> 24)
                             + (hi[(size_t)(y * 2) * hw + x * 2 + 1] >> 24)
                             + (hi[(size_t)(y * 2 + 1) * hw + x * 2]     >> 24)
                             + (hi[(size_t)(y * 2 + 1) * hw + x * 2 + 1] >> 24);
            pix[(size_t)y * w + x] = 0x00FFFFFFu | ((a / 4) << 24);
        }
    }
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = pix.data(); srd.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(dev_->CreateTexture2D(&td, &srd, &tex)) || !tex) return nullptr;
    HRESULT hr = dev_->CreateShaderResourceView(tex, nullptr, &srv); tex->Release();
    if (FAILED(hr)) return nullptr;
    Glyph g{ srv, w, h };
    return &glyphs_.emplace(key, g).first->second;
}

void Overlay::DrawQuad(ID3D11ShaderResourceView* srv, float x, float y, float w, float h, Color c) {
    if (warming_ || !srv || curW_ <= 0 || curH_ <= 0 || w <= 0 || h <= 0) return;
    auto nx = [&](float p) { return p / curW_ * 2.0f - 1.0f; };
    auto ny = [&](float p) { return 1.0f - p / curH_ * 2.0f; };
    const Vtx v[4] = { { nx(x), ny(y + h), 0, 1 }, { nx(x), ny(y), 0, 0 },
                       { nx(x + w), ny(y + h), 1, 1 }, { nx(x + w), ny(y), 1, 0 } };
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { ::memcpy(m.pData, v, sizeof(v)); ctx_->Unmap(vb_, 0); }
    const float col[4] = { c.r, c.g, c.b, c.a };
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { ::memcpy(m.pData, col, sizeof(col)); ctx_->Unmap(cb_, 0); }
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    ctx_->Draw(4, 0);
}

void Overlay::DrawRect(float x, float y, float w, float h, Color c) { DrawQuad(white_, x, y, w, h, c); }

float Overlay::DrawText(float x, float y, const std::string& utf8, int px, Color c,
                        const char* font, bool bold) {
    const Glyph* g = GetGlyph(utf8, px, font, bold); if (!g) return 0;
    DrawQuad(g->srv, x, y, (float)g->w, (float)g->h, c);
    return (float)g->w;
}

void Overlay::MeasureText(const std::string& utf8, int px, float& w, float& h,
                          const char* font, bool bold) {
    const Glyph* g = GetGlyph(utf8, px, font, bold); if (g) { w = (float)g->w; h = (float)g->h; } else { w = h = 0; }
}

bool Overlay::HasImage(const std::string& key) const { return images_.find(key) != images_.end(); }

void Overlay::DrawImage(const std::string& key, float x, float y, float w, float h,
                        const uint32_t* rgba, int tw, int th, Color tint) {
    auto it = images_.find(key);
    if (it == images_.end()) {
        if (!rgba || tw <= 0 || th <= 0 || !dev_) return;
        D3D11_TEXTURE2D_DESC td{}; td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = rgba; srd.SysMemPitch = tw * 4;
        ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(dev_->CreateTexture2D(&td, &srd, &tex)) || !tex) return;
        HRESULT hr = dev_->CreateShaderResourceView(tex, nullptr, &srv); tex->Release();
        if (FAILED(hr)) return;
        it = images_.emplace(key, Image{ srv, tw, th }).first;
    }
    DrawQuad(it->second.srv, x, y, w, h, tint);
}

bool Overlay::Mouse(float& x, float& y) const {
    if (!gameWnd_) return false;
    POINT p{};
    if (!::GetCursorPos(&p) || !::ScreenToClient(gameWnd_, &p)) return false;
    x = (float)p.x; y = (float)p.y;
    return true;
}

long __stdcall Overlay::HookPresent(IDXGISwapChain* swap, unsigned sync, unsigned flags) {
    Get().DrawFrame(swap);
    return oPresent_(swap, sync, flags);
}

// Drive the game's OWN pause (what ESC calls) to match the hub state. FUN_1406cdf0c(uiCtx, 0, show) is the
// pause-menu SHOW/HIDE primitive (NOT a toggle): param_3 is an explicit flag — 1 activates the "PauseMenu"
// screen (its OnActivate pushes the SimulationTimeScale=0 request = real sim-freeze + frees the cursor), 0
// deactivates it. We command show vs hide directly from the hub state, so there is no toggle-desync. MUST run
// on the game message thread (this is WndProc). SEH-only helpers (no C++ objects) so __try is legal.
static void SafeSetCursorFlag(unsigned v) {   // SEH-only: kCursorCtrlDisable = v (engine cursor-control gate)
    __try { *reinterpret_cast<volatile unsigned*>(game::FromRVA(game::kCursorCtrlDisable)) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int ForceShowCursor() {
    int calls = 0;
    int count = -1;
    do {
        count = ::ShowCursor(TRUE);
        ++calls;
    } while (count < 0 && calls < 32);
    return calls;
}
static void RestoreShowCursor(int calls) {
    while (calls-- > 0) ::ShowCursor(FALSE);
}
static void UnlockHubCursor() {
    SafeSetCursorFlag(1);                              // engine: stop recenter/clip/hide-show routines
    ::ReleaseCapture();                                // Win32: drop any mouse capture held by the game window
    ::ClipCursor(nullptr);                             // Win32: clear any client-rect cursor clamp
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));    // Win32: keep a normal hardware arrow over the hub
}
[[maybe_unused]] static std::uintptr_t PauseUiCtx() {   // SEH-only: resolve uiCtx = *(*(engine) + 0xe38); 0 on fault
    __try {
        const std::uintptr_t eng = *reinterpret_cast<std::uintptr_t*>(game::FromRVA(game::kEngineSingleton));
        if (!eng) return 0;
        return *reinterpret_cast<std::uintptr_t*>(eng + game::kUiCtxOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
[[maybe_unused]] static bool CallPauseShow(std::uintptr_t uiCtx, bool show) {   // SEH-only: FUN_1406cdf0c(uiCtx, 0, show)
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, char)>(game::FromRVA(game::kPauseToggle))(
            reinterpret_cast<void*>(uiCtx), nullptr, show ? 1 : 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
[[maybe_unused]] static void SyncGamePause(bool wantPaused) {
    static bool s_paused = false;
    if (wantPaused == s_paused) return;
    const std::uintptr_t uiCtx = PauseUiCtx();
    { char b[128]; ::wsprintfA(b, "[pause] want=%d uiCtx=0x%p (show/hide PauseMenu)", (int)wantPaused, (void*)uiCtx);
      Log::Get().Line(b); }
    if (!uiCtx) return;
    if (CallPauseShow(uiCtx, wantPaused)) s_paused = wantPaused;
    else Log::Get().Line("[pause] show/hide FAULTED");
}

// Subclassed onto the GAME window: ImGui reads input from the game's own message stream (no separate
// window -> no focus split, no z-order/monitor problems). While the hub is open we swallow input so it
// doesn't leak to the game; when closed everything passes straight through.
LRESULT __stdcall Overlay::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Overlay& o = Get();
    // F8 toggles the hub (ignore auto-repeat: bit 30 of lParam set == key was already down).
    if (msg == WM_KEYDOWN && w == VK_F8 && (l & 0x40000000) == 0) {
        o.menuOpen_ = !o.menuOpen_;
        if (o.menuOpen_ && o.appState_ == AppState::InGame) {
            HagUI::Get().RunFirstOpenInSave();
        }
        o.SyncCursorState();   // apply immediately; don't wait for the next Present
        char b[112]; ::wsprintfA(b, "[F8] hub=%d app=%s", (int)o.menuOpen_,
                                 o.appState_ == AppState::InGame ? "ingame" : "main-menu");
        Log::Get().Line(b);
        // NOTE: game sim-freeze is NOT wired — the pause lever is unfound (the FUN_1406cdf0c uiCtx chain is
        // garbage and SoW doesn't freeze on focus loss; see shared/GameOffsets.h "PAUSE"). Camera is still
        // locked in a save (we eat WM_INPUT while open) and the cursor is freed there via the
        // in-game-only cursor handoff; freezing the world is a follow-up. SyncGamePause() is left
        // defined but intentionally not called until the real SimulationTimeScale lever is found.
    }
    // ESC closes the hub while it's open (cursor visibility is reconciled in DrawFrame).
    if (msg == WM_KEYDOWN && w == VK_ESCAPE && o.menuOpen_) {
        o.menuOpen_ = false;
        o.SyncCursorState();
    }

    if (o.imguiInit_) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        if (o.menuOpen_) {
            // Keep a crisp HARDWARE cursor (the ImGui software cursor lags at the menu's frame rate).
            if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            // Raw input (mouselook) drives the in-game camera — eat it so the camera is LOCKED while the
            // hub is open. WM_INPUT still needs DefWindowProc for cleanup; just don't let the game see it.
            if (msg == WM_INPUT) return ::DefWindowProcW(h, msg, w, l);
            switch (msg) {   // modal: keep these away from the game while the hub is up
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                    return 0;
            }
        }
    }
    // (Game sim-freeze intentionally not wired — see the F8 handler note above and GameOffsets.h "PAUSE".)
    return ::CallWindowProcW(o.origWndProc_, h, msg, w, l);
}

void Overlay::SyncCursorState() {
    if (!imguiInit_) return;

    // In the main menu the game is already in cursor/UI mode. The heavy cursor handoff is only needed
    // in a loaded save, where the engine owns mouselook, recentering, clipping, and cursor visibility.
    const bool needsGameCursorUnlock = menuOpen_ && appState_ == AppState::InGame;
    if (needsGameCursorUnlock) {
        UnlockHubCursor();
        // The game can leave the Win32 ShowCursor counter deeply negative; draw an overlay cursor
        // so the hub always has a visible pointer even when the hardware cursor stays hidden.
        ImGui::GetIO().MouseDrawCursor = true;
        if (!cursorShown_) {
            cursorShowBalance_ = ForceShowCursor();
            cursorShown_ = true;
        }
    } else if (cursorShown_) {
        SafeSetCursorFlag(0);                              // engine: resume cursor control (mouselook recapture)
        ImGui::GetIO().MouseDrawCursor = false;
        RestoreShowCursor(cursorShowBalance_);
        cursorShowBalance_ = 0;
        cursorShown_ = false;
    }
}

void Overlay::DrawFrame(IDXGISwapChain* swap) {
    if (!ctx_) {
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev_))) || !dev_) return;
        dev_->GetImmediateContext(&ctx_);
    }

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = dev_->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
    if (FAILED(hr) || !rtv) return;
    curW_ = (float)bd.Width; curH_ = (float)bd.Height;

    if (!imguiInit_) {
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(swap->GetDesc(&sd))) gameWnd_ = sd.OutputWindow;
        if (!gameWnd_) { rtv->Release(); return; }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;                              // no imgui.ini next to the game
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad; // don't fight the game's controller
        StyleHagUI();
        LoadFonts();                                          // real TTFs sized to this back buffer
        ImGui_ImplWin32_Init(gameWnd_);
        ImGui_ImplDX11_Init(dev_, ctx_);
        origWndProc_ = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(gameWnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Overlay::WndProc)));
        imguiInit_ = true;
        Loader::Get().OnRenderLive();                         // game window up + rendering -> open console
        Log::Get().Good("[overlay] ImGui online; WndProc subclassed on the game window (F8 opens the hub)");
    }

    SyncCursorState();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (!InSave()) DrawWatermark();   // watermark only at the menu; hidden once in a save
    if (menuOpen_) DrawHub();

    ImGui::Render();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);   // ImGui's DX11 backend saves/restores the rest
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    rtv->Release();
}

// ---- black + gold theme (the Manga-List palette shared with the Skyrim HagUI) ----
static ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
// ImGui 1.92 removed single-arg PushFont; push each TTF at the size it was loaded with (LegacySize).
static void PushF(ImFont* f) { ImGui::PushFont(f, f ? f->LegacySize : 0.0f); }

// Rounded rect with a vertical color gradient (AddRectFilledMultiColor can't round). Draw it as
// horizontal strips clipped analytically to the rounded shape; this avoids cap/middle seam lines.
static void RoundedVGrad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot, float rad) {
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    if (rad <= 0.5f || w <= 1.0f || h <= 1.0f) {
        dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
        return;
    }

    rad = std::min(rad, std::min(w, h) * 0.5f);
    const int bands = std::max(8, (int)std::ceil(h));
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

    auto colAt = [&](float y) {
        float t = (y - a.y) / h;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        auto c = [&](int sh) {
            const int A = (top >> sh) & 0xFF;
            const int B = (bot >> sh) & 0xFF;
            return (int)(A + (B - A) * t + 0.5f);
        };
        return IM_COL32(c(0), c(8), c(16), c(24));
    };
    auto insetAt = [&](float y) {
        if (y < a.y + rad) {
            const float dy = (a.y + rad) - y;
            return rad - std::sqrt(std::max(0.0f, rad * rad - dy * dy));
        }
        if (y > b.y - rad) {
            const float dy = y - (b.y - rad);
            return rad - std::sqrt(std::max(0.0f, rad * rad - dy * dy));
        }
        return 0.0f;
    };

    dl->PrimReserve(bands * 6, bands * 4);
    for (int i = 0; i < bands; ++i) {
        const float y0 = a.y + h * (float)i / (float)bands;
        const float y1 = a.y + h * (float)(i + 1) / (float)bands;
        const float x0 = insetAt(y0);
        const float x1 = insetAt(y1);
        const ImU32 c0 = colAt(y0);
        const ImU32 c1 = colAt(y1);
        const ImDrawIdx idx = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(ImVec2(a.x + x0, y0), uv, c0);
        dl->PrimWriteVtx(ImVec2(b.x - x0, y0), uv, c0);
        dl->PrimWriteVtx(ImVec2(b.x - x1, y1), uv, c1);
        dl->PrimWriteVtx(ImVec2(a.x + x1, y1), uv, c1);
        dl->PrimWriteIdx(idx); dl->PrimWriteIdx(idx + 1); dl->PrimWriteIdx(idx + 2);
        dl->PrimWriteIdx(idx); dl->PrimWriteIdx(idx + 2); dl->PrimWriteIdx(idx + 3);
    }
}

static ImU32 LerpCol(ImU32 a, ImU32 b, float t) {
    auto c = [&](int sh) { int A = (a >> sh) & 0xFF, B = (b >> sh) & 0xFF; return (int)(A + (B - A) * t); };
    return IM_COL32(c(0), c(8), c(16), c(24));
}

// Left accent, 1:1 with the Skyrim HagUI (HagUI_Root.as buildWelcome, "railG" + "rmask"):
//   glow: rect(cx, cy, glowW, ch), linear HORIZONTAL gradient gold alpha 26% -> 0
//   rail: rect(cx, cy, railW, ch), linear VERTICAL gradient accent -> accent-dim, on top
// Both are plain full-height bars MASKED by the card's own rounded-rect path rrPath(cx, cy, cw, ch, r),
// but INSET by `d` px so a uniform d-px gold border sliver shows between the card border and the rail
// EVERYWHERE — straight edges AND corners. ImGui has no masks, so the mask is applied analytically:
// a straight middle quad plus arc-following trapezoids in the corners, per-vertex colors sampled from
// the UNCLIPPED gradient boxes (the cut never shifts/squeezes the gradient, exactly like the Flash
// mask). The bars stay straight and get cut by the arc; they don't bend around it. The inset contour
// is concentric with the border: same corner centers (x0+r, yT+r) etc., radius r-d — so the rail's
// curve begins d px earlier at top/bottom than the border, keeping the gap constant through the arc.
static void AccentLeft(ImDrawList* dl, ImVec2 p0, float ch, float r, float railW, float glowW, float d) {
    const float x0 = p0.x, yT = p0.y, yB = p0.y + ch;
    const float R = r - d;                                          // inset corner radius (same centers)
    const ImU32 aTop = IM_COL32(0xE0, 0xB3, 0x4A, 255), aBot = IM_COL32(0xB8, 0x86, 0x2F, 255);
    // AS gradient boxes: boxM(cx, cy, 30, ch) horizontal / boxM(cx, cy, 6, ch, pi/2) vertical
    auto glowCol = [&](float x, float) {
        float t = (x - x0) / glowW; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return IM_COL32(0xE0, 0xB3, 0x4A, (int)(66.0f * (1.0f - t) + 0.5f));   // AS alpha 26/100
    };
    auto railCol = [&](float, float y) { return LerpCol(aTop, aBot, (y - yT) / (yB - yT)); };

    // trapezoid with vertical right edge + slanted left edge, colored by col(x, y)
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    auto quad = [&](float xlt, float xlb, float xr, float yt, float yb, auto&& col) {
        if (yb <= yt) return;
        xlt = std::min(xlt, xr); xlb = std::min(xlb, xr);
        if (xlt >= xr && xlb >= xr) return;
        dl->PrimReserve(6, 4);
        const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(ImVec2(xlt, yt), uv, col(xlt, yt));
        dl->PrimWriteVtx(ImVec2(xr,  yt), uv, col(xr,  yt));
        dl->PrimWriteVtx(ImVec2(xr,  yb), uv, col(xr,  yb));
        dl->PrimWriteVtx(ImVec2(xlb, yb), uv, col(xlb, yb));
        dl->PrimWriteIdx(i0);     dl->PrimWriteIdx(i0 + 1); dl->PrimWriteIdx(i0 + 2);
        dl->PrimWriteIdx(i0);     dl->PrimWriteIdx(i0 + 2); dl->PrimWriteIdx(i0 + 3);
    };

    // one full-height bar of width w, left edge = card contour inset by d (the AS mask, d px in)
    auto bar = [&](float w, auto&& col) {
        const float xr = x0 + w;
        quad(x0 + d, x0 + d, xr, yT + r, yB - r, col);             // straight middle (left edge x0+d)
        const int N = 12;                                          // arc slices per corner
        for (int i = 0; i < N; ++i) {
            const float t0 = 1.5707963f * i / N, t1 = 1.5707963f * (i + 1) / N;
            const float xa = (x0 + r) - R * std::sin(t0);          // inset arc x, pole side of the slice
            const float xb = (x0 + r) - R * std::sin(t1);          // inset arc x, equator side
            const float da = r - R * std::cos(t0), db = r - R * std::cos(t1);  // y offset from the edge
            quad(xa, xb, xr, yT + da, yT + db, col);               // top corner (starts at yT+d)
            quad(xb, xa, xr, yB - db, yB - da, col);               // bottom corner (mirrored, ends yB-d)
        }
    };

    bar(glowW, glowCol);   // AS draw order: glow rect first...
    bar(railW, railCol);   // ...bright rail on top, both under the same inset mask
}

// Draw text horizontally condensed by `xs` (0..1) around pos.x: emit the glyphs, then scale the new
// vertices' X toward the left edge. Gives a narrow/condensed look from any font (no condensed file needed).
static void AddTextCX(ImDrawList* dl, ImFont* f, float px, ImVec2 pos, ImU32 col, const char* t, float xs) {
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(f, px, pos, col, t);
    for (int i = v0; i < dl->VtxBuffer.Size; ++i)
        dl->VtxBuffer[i].pos.x = pos.x + (dl->VtxBuffer[i].pos.x - pos.x) * xs;
}

static void DrawFieldTextClipped(ImDrawList* dl, ImFont* f, float px,
                                 ImVec2 frameMin, ImVec2 frameMax,
                                 const char* text, ImU32 col,
                                 float padX, float padY,
                                 bool caret,
                                 const ImVec4* outerClip = nullptr) {
    if (!text) text = "";
    const float edgeGuard = 1.0f;
    ImVec4 clip(frameMin.x + padX + edgeGuard, frameMin.y + padY + edgeGuard,
                frameMax.x - padX - edgeGuard, frameMax.y - padY - edgeGuard);
    if (outerClip) {
        clip.x = std::max(clip.x, outerClip->x);
        clip.y = std::max(clip.y, outerClip->y);
        clip.z = std::min(clip.z, outerClip->z);
        clip.w = std::min(clip.w, outerClip->w);
    }
    if (clip.z <= clip.x || clip.w <= clip.y) return;

    const ImVec2 sz = f ? f->CalcTextSizeA(px, 3.4e38f, 0.0f, text) : ImGui::CalcTextSize(text);
    float y = frameMin.y + (frameMax.y - frameMin.y - sz.y) * 0.5f;
    y = std::max(clip.y, std::min(y, clip.w - sz.y));
    dl->AddText(f, px, ImVec2(clip.x, y), col, text, nullptr, 0.0f, &clip);

    if (caret && std::fmod(ImGui::GetTime(), 1.0) < 0.55) {
        const float cx = std::min(clip.x + sz.x + 1.0f, clip.z - 1.0f);
        dl->AddLine(ImVec2(cx, clip.y + 1.0f), ImVec2(cx, clip.w - 1.0f), col, 1.0f);
    }
}

static void DrawScrollArrowCaps(const char* id, ImDrawList* dl, float sx, float sy, float s, ImU32 accent) {
    (void)id;
    const float maxY = ImGui::GetScrollMaxY();
    if (maxY <= 0.0f) return;

    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const float barW = std::max(6.0f, ImGui::GetStyle().ScrollbarSize);
    const float capH = std::max(barW + 2.0f, 13.0f * sy);
    const float x0 = wp.x + ws.x - barW - 1.0f;
    const float x1 = wp.x + ws.x - 1.0f;
    const float y0 = wp.y + 1.0f;
    const float y1 = wp.y + ws.y - 1.0f;
    if (y1 - y0 < capH * 2.0f + 4.0f) return;

    const float lineStep = std::max(ImGui::GetTextLineHeightWithSpacing() * 3.0f, 24.0f * sy);
    const ImGuiIO& io = ImGui::GetIO();
    const bool winHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 upA(x0, y0), upB(x1, y0 + capH);
    const ImVec2 dnA(x0, y1 - capH), dnB(x1, y1);
    auto containsMouse = [&](ImVec2 a, ImVec2 b) {
        return winHovered && io.MousePos.x >= a.x && io.MousePos.x < b.x &&
               io.MousePos.y >= a.y && io.MousePos.y < b.y;
    };
    const bool upHover = containsMouse(upA, upB);
    const bool downHover = containsMouse(dnA, dnB);
    const bool upActive = upHover && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool downActive = downHover && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (upActive)
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - lineStep * 0.18f));
    if (downActive)
        ImGui::SetScrollY(std::min(maxY, ImGui::GetScrollY() + lineStep * 0.18f));

    const auto capColor = [](bool hover, bool active) {
        return active ? IM_COL32(0xB8, 0x86, 0x2F, 230)
                      : (hover ? IM_COL32(0x3A, 0x30, 0x1E, 250)
                               : IM_COL32(0x23, 0x1E, 0x16, 230));
    };
    dl->AddRectFilled(upA, upB, capColor(upHover, upActive), 3.0f * s);
    dl->AddRectFilled(dnA, dnB, capColor(downHover, downActive), 3.0f * s);
    dl->AddRect(upA, upB, IM_COL32(0xE0, 0xB3, 0x4A, 70), 3.0f * s, 0, 1.0f);
    dl->AddRect(dnA, dnB, IM_COL32(0xE0, 0xB3, 0x4A, 70), 3.0f * s, 0, 1.0f);

    const float cx = (x0 + x1) * 0.5f;
    const float triW = std::max(2.0f, barW * 0.32f);
    const float triH = std::max(3.0f, capH * 0.28f);
    const float uy = y0 + capH * 0.55f;
    const float dy = y1 - capH * 0.55f;
    dl->AddTriangleFilled(ImVec2(cx, uy - triH), ImVec2(cx - triW, uy + triH * 0.45f),
                          ImVec2(cx + triW, uy + triH * 0.45f), accent);
    dl->AddTriangleFilled(ImVec2(cx, dy + triH), ImVec2(cx - triW, dy - triH * 0.45f),
                          ImVec2(cx + triW, dy - triH * 0.45f), accent);
}

void Overlay::StyleHagUI() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6; s.ChildRounding = 4; s.FrameRounding = 4; s.TabRounding = 3;
    s.WindowBorderSize = 1; s.WindowPadding = ImVec2(28, 24);
    s.ItemSpacing = ImVec2(14, 12); s.FramePadding = ImVec2(16, 8);
    s.ScrollbarSize = 7.0f;
    s.ScrollbarRounding = 3.0f;

    const ImVec4 accent    = Rgb(0xE0, 0xB3, 0x4A);
    const ImVec4 accentDim = Rgb(0xB8, 0x86, 0x2F);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = Rgb(0x1A, 0x17, 0x12, 0.97f);
    c[ImGuiCol_ChildBg]         = Rgb(0x00, 0x00, 0x00, 0.00f);
    c[ImGuiCol_Border]          = Rgb(0xE0, 0xB3, 0x4A, 0.42f);
    c[ImGuiCol_Text]            = Rgb(0xEC, 0xE6, 0xDA);
    c[ImGuiCol_TextDisabled]    = Rgb(0x9C, 0x94, 0x86);
    c[ImGuiCol_Button]          = Rgb(0x23, 0x1E, 0x16);
    c[ImGuiCol_ButtonHovered]   = Rgb(0x3A, 0x2F, 0x18);
    c[ImGuiCol_ButtonActive]    = accentDim;
    c[ImGuiCol_FrameBg]         = Rgb(0x23, 0x1E, 0x16, 0.90f);
    c[ImGuiCol_FrameBgHovered]  = Rgb(0x3A, 0x2F, 0x18, 0.90f);
    c[ImGuiCol_FrameBgActive]   = Rgb(0x3A, 0x2F, 0x18);
    c[ImGuiCol_Tab]             = Rgb(0x1A, 0x17, 0x12, 0.00f);
    c[ImGuiCol_TabHovered]      = Rgb(0xE0, 0xB3, 0x4A, 0.18f);
    c[ImGuiCol_TabActive]       = Rgb(0x23, 0x1E, 0x16, 0.00f);
    c[ImGuiCol_TabUnfocused]        = Rgb(0x1A, 0x17, 0x12, 0.00f);
    c[ImGuiCol_TabUnfocusedActive]  = Rgb(0x23, 0x1E, 0x16, 0.00f);
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accentDim;
    c[ImGuiCol_SliderGrabActive]= accent;
    c[ImGuiCol_Separator]       = Rgb(0xE0, 0xB3, 0x4A, 0.20f);
    c[ImGuiCol_TitleBg]         = Rgb(0x1A, 0x17, 0x12);
    c[ImGuiCol_TitleBgActive]   = Rgb(0x1A, 0x17, 0x12);
    c[ImGuiCol_PopupBg]         = Rgb(0x1A, 0x17, 0x12, 0.98f);
    c[ImGuiCol_ScrollbarBg]          = Rgb(0x0A, 0x0A, 0x0C, 0.42f);
    c[ImGuiCol_ScrollbarGrab]        = Rgb(0xB8, 0x86, 0x2F, 0.62f);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0xE0, 0xB3, 0x4A, 0.78f);
    c[ImGuiCol_ScrollbarGrabActive]  = Rgb(0xE0, 0xB3, 0x4A, 0.95f);
}

void Overlay::LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Latin-1 + dashes (incl. em-dash U+2014) + smart quotes so punctuation renders (not '?').
    static const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x2010, 0x2015, 0x2018, 0x2019, 0x201C, 0x201D, 0 };
    // Base size scaled to the card (AS stage = 462-tall card). Fonts are DYNAMIC in ImGui 1.92, so a
    // single load per family renders crisply at any size we pass to AddText (AS_size * s).
    const float base = (curH_ > 100.0f ? curH_ : 1080.0f) * 0.64f / 462.0f * 18.0f;
    auto add = [&](const char* path) -> ImFont* {
        return io.Fonts->AddFontFromFileTTF(path, base, nullptr, kRanges);  // nullptr if missing (safe)
    };
    // Bahnschrift (Windows DIN) is a narrow/condensed sans — much tighter than Segoe UI.
    fBody_  = add("C:\\Windows\\Fonts\\bahnschrift.ttf");   // first == default font
    fKick_  = add("C:\\Windows\\Fonts\\bahnschrift.ttf");
    fTab_   = add("C:\\Windows\\Fonts\\bahnschrift.ttf");
    fSmall_ = add("C:\\Windows\\Fonts\\bahnschrift.ttf");
    fFoot_  = add("C:\\Windows\\Fonts\\bahnschrift.ttf");
    fWord_  = add("C:\\Windows\\Fonts\\georgiab.ttf");      // serif hero wordmark
    io.Fonts->Build();
}

void Overlay::DrawWatermark() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float sz = fSmall_ ? fSmall_->LegacySize : ImGui::GetFontSize();
    const char* wm = "SoWLoader - Hagryph";
    const char* hint = "F8  Menu";
    const ImVec2 wmSz = fSmall_ ? fSmall_->CalcTextSizeA(sz, 3.4e38f, 0.0f, wm) : ImGui::CalcTextSize(wm);
    dl->AddText(fSmall_, sz, ImVec2(disp.x - wmSz.x - 16.0f, 12.0f), IM_COL32(205, 205, 210, 170), wm);
    const ImVec2 hSz = fSmall_ ? fSmall_->CalcTextSizeA(sz, 3.4e38f, 0.0f, hint) : ImGui::CalcTextSize(hint);
    dl->AddText(fSmall_, sz, ImVec2(disp.x - hSz.x - 16.0f, 12.0f + sz + 2.0f), IM_COL32(224, 179, 74, 170), hint);
}

// Save-loaded state is pushed by GameHooks' world/menu transition hooks, not polled here.
bool Overlay::InSave() const { return appState_ == AppState::InGame; }

void Overlay::SetInGame(bool inGame) {
    const AppState next = inGame ? AppState::InGame : AppState::MainMenu;
    if (appState_ == next) return;
    appState_ = next;
    SyncCursorState();
}

void Overlay::DrawHub() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;

    // dim + block the game behind the modal card
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(6, 6, 10, 190));

    // The Skyrim card is 820x462; our card keeps that ~1.78 aspect, so one scale maps every AS
    // coordinate to screen (sx for x, sy for y, s for radii/sizes/fonts).
    const float cw = disp.x * 0.64f, ch = disp.y * 0.64f;
    const float sx = cw / 820.0f, sy = ch / 462.0f, s = sy;
    ImGui::SetNextWindowPos(ImVec2((disp.x - cw) * 0.5f, (disp.y - ch) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(cw, ch));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));   // we position everything in AS coords
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);     // AS card radius = 14
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);        // slightly thicker gold frame
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    const ImU32 uAccent = IM_COL32(0xE0, 0xB3, 0x4A, 255);
    const ImU32 uDim    = IM_COL32(0x9C, 0x94, 0x86, 255);
    const ImU32 uText   = IM_COL32(0xEC, 0xE6, 0xDA, 255);
    const ImU32 uFaint  = IM_COL32(0x6B, 0x64, 0x56, 255);

    if (ImGui::Begin("HagUI##hub", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const float r = 14.0f * s;
        const float XS = 0.80f;                              // horizontal condense for the sans text
        auto X = [&](float ax) { return p0.x + ax * sx; };   // AS x -> screen
        auto Y = [&](float ay) { return p0.y + ay * sy; };   // AS y -> screen
        auto txt = [&](ImFont* f, float px, float ax, float ay, ImU32 c, const char* t) {
            AddTextCX(dl, f, px, ImVec2(X(ax), Y(ay)), c, t, XS);   // condensed
        };
        auto measureW = [&](ImFont* f, float px, const char* t) {   // condensed width
            return (f ? f->CalcTextSizeA(px, 3.4e38f, 0.0f, t).x : ImGui::CalcTextSize(t).x) * XS;
        };

        // ---- left accent (AS: glow 30w gold 26->0 + rail 6w accent->dim, masked to the card path,
        //      inset 1px so a uniform 1px gold border sliver shows between border and rail) ----
        AccentLeft(dl, p0, ch, r, 6.0f * sx, 30.0f * sx, 1.0f);

        // ---- corner flourishes (AS: gold alpha 30, TL x30..56 y22, BR x cw-56..cw-30 y ch-22) ----
        const ImU32 uFl = IM_COL32(0xE0, 0xB3, 0x4A, 77);
        dl->AddLine(ImVec2(X(30), Y(22)), ImVec2(X(56), Y(22)), uFl, 1.0f * s);
        dl->AddLine(ImVec2(X(820 - 56), Y(462 - 22)), ImVec2(X(820 - 30), Y(462 - 22)), uFl, 1.0f * s);

        // ---- tabs (AS: nx=60 ny=28, pad16 gap12, bold size15, hairline + active underline at ny+34)
        //      WELCOME + one tab per MOD-REGISTERED page (HagUI cross-plugin API) — none hardcoded ----
        const auto& pages = HagUI::Get().Pages();
        // SCOPE gating: global tabs always show; save-local tabs show only once a save is loaded.
        const bool inSave = InSave();
        std::vector<int> vis;                                 // page indices visible right now
        for (int p = 0; p < (int)pages.size(); ++p)
            if (pages[p].scope != SOWMOD_LOCAL || inSave) vis.push_back(p);
        if (activeTab_ > (int)vis.size()) activeTab_ = 0;     // visible set can change between frames
        const float fTabPx = 15.0f * s, pad = 16.0f * sx, gap = 12.0f * sx, cellH = 35.0f * sy;
        const float navY = Y(28), hairY = Y(28 + 34);
        float cx = X(60);
        for (int i = 0; i <= (int)vis.size(); ++i) {
            std::string label = (i == 0) ? "WELCOME" : pages[vis[i - 1]].title;
            for (auto& lc : label) lc = (char)::toupper((unsigned char)lc);
            const bool active = activeTab_ == i;
            const float cellW = measureW(fTab_, fTabPx, label.c_str()) + pad * 2.0f;
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(ImVec2(cx, navY));
            ImGui::InvisibleButton("##tab", ImVec2(cellW, cellH));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) activeTab_ = i;
            ImGui::PopID();
            AddTextCX(dl, fTab_, fTabPx, ImVec2(cx + pad, navY + 6.0f * sy),
                      active ? uAccent : (hov ? uText : uDim), label.c_str(), XS);
            if (active) dl->AddLine(ImVec2(cx, hairY), ImVec2(cx + cellW, hairY), uAccent, 2.0f * s);
            cx += cellW + gap;
        }
        dl->AddLine(ImVec2(X(60), hairY), ImVec2(X(772), hairY), IM_COL32(0xE0, 0xB3, 0x4A, 41), 1.0f * s);

        // ---- active page ----
        if (activeTab_ == 0) {   // content origin AS: x=60 y=86
            txt(fKick_, 13.0f * s, 60, 86, uFaint, "W E L C O M E   T O");
            const float wy = 86 + 14;                                  // wordmark (AS x-2, y+14, size 64)
            dl->AddText(fWord_, 64.0f * s, ImVec2(X(58), Y(wy)), uText, "Hag");   // serif hero, not condensed
            const float hagW = fWord_ ? fWord_->CalcTextSizeA(64.0f * s, 3.4e38f, 0.0f, "Hag").x : 0.0f;
            dl->AddText(fWord_, 64.0f * s, ImVec2(X(58) + hagW, Y(wy)), uAccent, "UI");
            dl->AddRectFilledMultiColor(ImVec2(X(60), Y(198)), ImVec2(X(60) + 250.0f * sx, Y(198) + 2.0f * sy),
                IM_COL32(0xE0, 0xB3, 0x4A, 153), IM_COL32(0xE0, 0xB3, 0x4A, 0),
                IM_COL32(0xE0, 0xB3, 0x4A, 0),   IM_COL32(0xE0, 0xB3, 0x4A, 153));   // divider (x60 y198 w250)
            txt(fBody_, 18.0f * s, 60, 218, uDim, "Your private control room for every Hagryph mod \xE2\x80\x94");
            txt(fBody_, 18.0f * s, 60, 218 + 26, uDim, "configuration, tools, and more, gathered in one place.");
        } else {
            // registered page — 1:1 with the AS option page (buildOptionPage / makeCheckbox /
            // paintButton): header bold 21 at (60,86); rows from y+44, 40 apart.
            const HagUI::Page& pg = pages[vis[activeTab_ - 1]];
            AddTextCX(dl, fTab_, 21.0f * s, ImVec2(X(60), Y(86)), uText, pg.title.c_str(), XS);
            float ry = 86 + 44;
            for (int i = 0; i < (int)pg.widgets.size(); ++i) {
                const HagUI::Widget& wd = pg.widgets[i];
                ImGui::PushID(i);
                if (wd.type == HagUI::WToggle) {
                    // full-width clickable row (AS: invisible hit rect w x 30, flips the box)
                    ImGui::SetCursorScreenPos(ImVec2(X(60), Y(ry)));
                    ImGui::InvisibleButton("##row", ImVec2(712.0f * sx, 30.0f * sy));
                    const bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked() && wd.toggle) *wd.toggle = !*wd.toggle;
                    const bool on = wd.toggle && *wd.toggle;
                    // gold checkbox (AS paintCheckbox): 22x22 r5; border a42 (hover a82);
                    // fill a6 (checked a22); gold 2px check glyph (5,11)-(9,16)-(17,6) when on
                    const ImVec2 ka(X(60), Y(ry + 4)), kb(X(60) + 22.0f * sx, Y(ry + 4) + 22.0f * sy);
                    const float kr = 5.0f * s;
                    dl->AddRectFilled(ka, kb, IM_COL32(0xE0, 0xB3, 0x4A, on ? 56 : 15), kr);
                    dl->AddRect(ka, kb, IM_COL32(0xE0, 0xB3, 0x4A, hov ? 209 : 107), kr, 0, 1.0f * s);
                    if (on) {
                        const ImVec2 pts[3] = { ImVec2(X(60 + 5), Y(ry + 4 + 11)),
                                                ImVec2(X(60 + 9), Y(ry + 4 + 16)),
                                                ImVec2(X(60 + 17), Y(ry + 4 + 6)) };
                        dl->AddPolyline(pts, 3, uAccent, 0, 2.0f * s);
                    }
                    txt(fBody_, 17.0f * s, 60 + 36, ry + 3, uText, wd.text.c_str());
                } else if (wd.type == HagUI::WButton) {
                    // gold button (AS paintButton): rr7, vgrad a18->6 border a42; hover a34/14/78
                    const float lw = measureW(fTab_, 15.0f * s, wd.text.c_str());
                    const ImVec2 pa(X(60), Y(ry)), pb(X(60) + lw + 32.0f * sx, Y(ry) + 30.0f * sy);
                    ImGui::SetCursorScreenPos(pa);
                    ImGui::InvisibleButton("##btn", ImVec2(pb.x - pa.x, pb.y - pa.y));
                    const bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked() && wd.onClick) wd.onClick();
                    RoundedVGrad(dl, pa, pb, IM_COL32(0xE0, 0xB3, 0x4A, hov ? 87 : 46),
                                             IM_COL32(0xE0, 0xB3, 0x4A, hov ? 36 : 15), 7.0f * s);
                    dl->AddRect(pa, pb, IM_COL32(0xE0, 0xB3, 0x4A, hov ? 199 : 107), 7.0f * s, 0, 1.0f * s);
                    const float lh = fTab_ ? fTab_->CalcTextSizeA(15.0f * s, 3.4e38f, 0.0f, wd.text.c_str()).y : 15.0f * s;
                    AddTextCX(dl, fTab_, 15.0f * s,
                              ImVec2(pa.x + (pb.x - pa.x - lw) * 0.5f, pa.y + (pb.y - pa.y - lh) * 0.5f),
                              uAccent, wd.text.c_str(), XS);
                } else if (wd.type == HagUI::WList) {
                    // ---- search + MULTI-FACET filter + grouped, scrollable item list ----
                    // Clamped to the drawable band (below the tabs, above the BR corner marker @ AS 440).
                    // Row 1: search + Clear. Row 2: one multi-select dropdown per facet. Then a grouped
                    // list (header on facet 0, sub-header on facet 1). ImGui's child gives scrollbar/wheel.
                    const float listX = 60.0f, listW = 712.0f;
                    // extra breathing room above and below the filter row (search .. filter .. list)
                    const float searchAS = ry, filterAS = ry + 38.0f, listTopAS = ry + 80.0f, listBotAS = 434.0f;
                    ImGui::PushFont(fBody_, 15.0f * s);
                    // shape: rounded frames/popups, roomy padding, a visible gold scrollbar — applies to
                    // the search box, the facet combo buttons AND their popups (4 vars, popped at the end).
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  5.0f * s);
                    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,  5.0f * s);
                    // vertical padding must clear the glyph ascenders/descenders, or the search text
                    // pokes out the top and bottom of the field. Keep it generous relative to the font.
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(8.0f * sx, 7.0f * sy));
                    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,  7.0f);
                    // theme (black + gold): inputs, text, borders, selectable/header, buttons, scrollbar
                    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(0x23, 0x1E, 0x16, 235));  // 1
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0x2C, 0x25, 0x19, 235));  // 2
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(0x2C, 0x25, 0x19, 255));  // 3
                    ImGui::PushStyleColor(ImGuiCol_Text,           uText);                            // 4
                    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   uFaint);                           // 5
                    ImGui::PushStyleColor(ImGuiCol_Border,         IM_COL32(0xE0, 0xB3, 0x4A, 90));   // 6
                    ImGui::PushStyleColor(ImGuiCol_Header,         IM_COL32(0xE0, 0xB3, 0x4A, 56));   // 7
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  IM_COL32(0xE0, 0xB3, 0x4A, 82));   // 8
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   IM_COL32(0xE0, 0xB3, 0x4A, 110));  // 9
                    ImGui::PushStyleColor(ImGuiCol_CheckMark,      uAccent);                          // 10
                    ImGui::PushStyleColor(ImGuiCol_Button,         IM_COL32(0x23, 0x1E, 0x16, 235));  // 11
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  IM_COL32(0x3A, 0x30, 0x1E, 255));  // 12
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   IM_COL32(0x2C, 0x25, 0x19, 255));  // 13
                    ImGui::PushStyleColor(ImGuiCol_PopupBg,        IM_COL32(0x14, 0x12, 0x10, 250));  // 14
                    ImGui::PushStyleColor(ImGuiCol_ChildBg,              IM_COL32(0x12, 0x10, 0x13, 180)); // 15
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          IM_COL32(0x0A, 0x0A, 0x0C, 120)); // 16
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        IM_COL32(0xB8, 0x86, 0x2F, 160)); // 17
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0xE0, 0xB3, 0x4A, 200)); // 18
                    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  IM_COL32(0xE0, 0xB3, 0x4A, 240)); // 19

                    // any active filter/search? (drives the Clear button)
                    bool anyActive = wd.search[0] != '\0';
                    for (const auto& f : wd.facets) if (f.selCount() > 0) { anyActive = true; break; }

                    // --- row 1: search box + Clear ---
                    ImGui::SetCursorScreenPos(ImVec2(X(listX), Y(searchAS)));
                    const float searchW = listW * 0.74f * sx;
                    ImGui::SetNextItemWidth(searchW);
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(0, 0, 0, 0));
                    ImGui::InputTextWithHint("##search", "search\xE2\x80\xA6", wd.search, sizeof(wd.search));
                    ImGui::PopStyleColor(2);
                    {
                        const ImVec2 fa = ImGui::GetItemRectMin();
                        const ImVec2 fb = ImGui::GetItemRectMax();
                        const bool active = ImGui::IsItemActive();
                        const char* shown = wd.search[0] ? wd.search : "search...";
                        DrawFieldTextClipped(dl, fBody_, 15.0f * s, fa, fb, shown,
                                             wd.search[0] ? uText : uFaint,
                                             8.0f * sx, 3.0f * sy, active && wd.search[0]);
                    }
                    ImGui::SameLine(0.0f, 10.0f * sx);
                    if (!anyActive) ImGui::BeginDisabled();
                    if (ImGui::Button("Clear", ImVec2(listW * 0.22f * sx, 0))) {
                        wd.search[0] = '\0';
                        for (auto& f : wd.facets) std::fill(f.sel.begin(), f.sel.end(), (char)0);
                    }
                    if (!anyActive) ImGui::EndDisabled();

                    // --- row 2: one multi-select dropdown per (non-empty) facet ---
                    int nf = 0; for (const auto& f : wd.facets) if (!f.opts.empty()) ++nf;
                    const float fw = nf > 0 ? (listW / nf - 6.0f) * sx : listW * sx;
                    ImGui::SetCursorScreenPos(ImVec2(X(listX), Y(filterAS)));
                    bool firstF = true;
                    for (int fi = 0; fi < (int)wd.facets.size(); ++fi) {
                        const HagUI::Facet& F = wd.facets[fi];
                        if (F.opts.empty()) continue;
                        if (!firstF) ImGui::SameLine(0.0f, 6.0f * sx);
                        firstF = false;
                        const int sc = F.selCount();
                        std::string lbl = F.name;
                        if (sc > 0) lbl += " (" + std::to_string(sc) + ")";
                        ImGui::PushID(fi);
                        // bound the popup: exactly the button width, capped to ~8 rows so long facets
                        // (Set, Rarity) scroll inside the card instead of spilling past its frame.
                        const float maxPopupH = 8.0f * ImGui::GetTextLineHeightWithSpacing() + 12.0f * sy;
                        const float fieldH = ImGui::GetFrameHeight();
                        ImGui::InvisibleButton("##facet_button", ImVec2(fw, fieldH));
                        const bool comboHovered = ImGui::IsItemHovered();
                        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##facet_popup");
                        const bool comboOpen = ImGui::IsPopupOpen("##facet_popup");
                        {
                            const ImVec2 fa = ImGui::GetItemRectMin();
                            const ImVec2 fb = ImGui::GetItemRectMax();
                            const float rr = 5.0f * s;
                            const float arrowReserve = std::max(fieldH, 34.0f * sx);
                            const ImVec2 aa(std::max(fa.x, fb.x - arrowReserve), fa.y);
                            const ImVec2 ab = fb;
                            RoundedVGrad(dl, fa, fb,
                                         IM_COL32(0x23, 0x1E, 0x16, comboOpen || comboHovered ? 255 : 235),
                                         IM_COL32(0x1A, 0x16, 0x11, comboOpen || comboHovered ? 255 : 235),
                                         rr);
                            dl->AddRectFilled(aa, ab,
                                comboOpen ? IM_COL32(0x3A, 0x30, 0x1E, 245)
                                          : IM_COL32(0x31, 0x29, 0x19, 225),
                                rr);
                            dl->AddRect(fa, fb, IM_COL32(0xE0, 0xB3, 0x4A, comboHovered ? 135 : 90),
                                        rr, 0, 1.0f * s);
                            DrawFieldTextClipped(dl, fBody_, 15.0f * s, fa,
                                                 ImVec2(aa.x, fb.y),
                                                 lbl.c_str(), uText,
                                                 8.0f * sx, 3.0f * sy, false);
                            const float cxA = (aa.x + ab.x) * 0.5f;
                            const float cyA = (aa.y + ab.y) * 0.5f;
                            const float triW = std::max(3.0f * s, 4.0f);
                            const float triH = std::max(2.0f * s, 3.0f);
                            const ImU32 arrowCol = comboOpen ? uAccent : IM_COL32(0xE0, 0xB3, 0x4A, 190);
                            if (comboOpen) {
                                dl->AddTriangleFilled(ImVec2(cxA, cyA - triH),
                                                      ImVec2(cxA - triW, cyA + triH * 0.55f),
                                                      ImVec2(cxA + triW, cyA + triH * 0.55f),
                                                      arrowCol);
                            } else {
                                dl->AddTriangleFilled(ImVec2(cxA, cyA + triH),
                                                      ImVec2(cxA - triW, cyA - triH * 0.55f),
                                                      ImVec2(cxA + triW, cyA - triH * 0.55f),
                                                      arrowCol);
                            }
                        }
                        ImGui::SetNextWindowSizeConstraints(ImVec2(fw, 0.0f), ImVec2(fw, maxPopupH));
                        if (ImGui::BeginPopup("##facet_popup")) {
                            ImDrawList* comboDl = ImGui::GetWindowDrawList();
                            const ImVec2 popMin = ImGui::GetWindowPos();
                            const ImVec2 popMax(popMin.x + ImGui::GetWindowWidth(),
                                                popMin.y + ImGui::GetWindowHeight());
                            const ImVec4 popClip(popMin.x + 2.0f, popMin.y + 2.0f,
                                                 popMax.x - 2.0f, popMax.y - 2.0f);
                            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f * sy));
                            for (int j = 0; j < (int)F.opts.size(); ++j) {
                                const bool selected = F.sel[j] != 0;
                                ImGui::PushID(j);
                                const float optW = ImGui::GetContentRegionAvail().x;
                                const float optH = std::max(ImGui::GetTextLineHeightWithSpacing(),
                                                            24.0f * sy);
                                ImGui::InvisibleButton("##opt", ImVec2(optW, optH));
                                const bool hov = ImGui::IsItemHovered();
                                if (ImGui::IsItemClicked()) F.sel[j] = selected ? 0 : 1;
                                const ImVec2 oa = ImGui::GetItemRectMin();
                                const ImVec2 ob = ImGui::GetItemRectMax();
                                if (selected || hov) {
                                    comboDl->AddRectFilled(oa, ob,
                                        selected ? IM_COL32(0xE0, 0xB3, 0x4A, hov ? 58 : 42)
                                                 : IM_COL32(0xE0, 0xB3, 0x4A, 24),
                                        4.0f * s);
                                }
                                const float box = std::max(10.0f * s, std::min(14.0f * s, optH - 6.0f * sy));
                                const ImVec2 ca(oa.x + 7.0f * sx, oa.y + (optH - box) * 0.5f);
                                const ImVec2 cb(ca.x + box, ca.y + box);
                                comboDl->AddRectFilled(ca, cb,
                                    IM_COL32(0xE0, 0xB3, 0x4A, selected ? 50 : 12),
                                    3.0f * s);
                                comboDl->AddRect(ca, cb,
                                    IM_COL32(0xE0, 0xB3, 0x4A, hov ? 180 : 95),
                                    3.0f * s, 0, 1.0f * s);
                                if (selected) {
                                    const ImVec2 pts[3] = {
                                        ImVec2(ca.x + box * 0.24f, ca.y + box * 0.54f),
                                        ImVec2(ca.x + box * 0.43f, ca.y + box * 0.74f),
                                        ImVec2(ca.x + box * 0.78f, ca.y + box * 0.28f)
                                    };
                                    comboDl->AddPolyline(pts, 3, uAccent, 0, 2.0f * s);
                                }
                                DrawFieldTextClipped(comboDl, fBody_, 14.0f * s,
                                                     ImVec2(oa.x + 28.0f * sx, oa.y),
                                                     ImVec2(ob.x - 8.0f, ob.y), F.opts[j].c_str(),
                                                     selected ? uText : uDim,
                                                     2.0f * sx, 0.0f, false,
                                                     &popClip);
                                ImGui::PopID();
                            }
                            DrawScrollArrowCaps("facet_scroll", comboDl, sx, sy, s, uAccent);
                            ImGui::PopStyleVar();
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }

                    // --- filter + search over items ---
                    auto low = [](std::string v) {
                        for (auto& c : v) c = (char)::tolower((unsigned char)c); return v; };
                    const std::string q = low(wd.search);
                    std::vector<int> vis; vis.reserve(wd.items.size());
                    for (int k = 0; k < (int)wd.items.size(); ++k) {
                        bool ok = true;
                        for (int fi = 0; fi < (int)wd.facets.size() && ok; ++fi) {
                            const HagUI::Facet& F = wd.facets[fi];
                            if (F.selCount() == 0) continue;                 // facet inactive
                            const int idx = wd.itemFacetIdx[k][fi];          // OR within facet
                            if (idx < 0 || !F.sel[idx]) ok = false;          // AND across facets
                        }
                        if (!ok) continue;
                        if (!q.empty() && low(wd.items[k]).find(q) == std::string::npos) continue;
                        vis.push_back(k);
                    }

                    // --- grouped, scrolling list ---
                    bool openAddPopup = false;
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * sx, 6.0f * sy));
                    ImGui::SetCursorScreenPos(ImVec2(X(listX), Y(listTopAS)));
                    ImGui::BeginChild("##itemlist", ImVec2(listW * sx, (listBotAS - listTopAS) * sy),
                                      true, ImGuiWindowFlags_None);
                    const ImVec2 childMin = ImGui::GetWindowPos();
                    const ImVec2 childSize = ImGui::GetWindowSize();
                    const ImVec4 listClip(childMin.x + 1.0f, childMin.y + 1.0f,
                                          childMin.x + childSize.x - 1.0f,
                                          childMin.y + childSize.y - 1.0f);
                    ImDrawList* listDl = ImGui::GetWindowDrawList();
                    listDl->PushClipRect(ImVec2(listClip.x, listClip.y),
                                         ImVec2(listClip.z, listClip.w), true);
                    if (vis.empty()) {
                        ImGui::TextDisabled("no items match the current filters");
                    } else {
                        const bool haveG0 = !wd.facets.empty(), haveG1 = wd.facets.size() > 1;
                        int lastG0 = -2, lastG1 = -2;
                        for (int k : vis) {
                            // group header (facet 0) + sub-header (facet 1)
                            if (haveG0) {
                                const int g0 = wd.itemFacetIdx[k][0];
                                if (g0 != lastG0) {
                                    lastG0 = g0; lastG1 = -2;
                                    ImGui::Dummy(ImVec2(0, 3.0f * sy));
                                    ImGui::PushFont(fTab_, 15.0f * s);
                                    ImGui::PushStyleColor(ImGuiCol_Text, uAccent);
                                    ImGui::TextUnformatted(g0 >= 0 ? wd.facets[0].opts[g0].c_str() : "Other");
                                    ImGui::PopStyleColor(); ImGui::PopFont();
                                    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(0xE0, 0xB3, 0x4A, 60));
                                    ImGui::Separator();
                                    ImGui::PopStyleColor();
                                }
                            }
                            if (haveG1) {
                                const int g1 = wd.itemFacetIdx[k][1];
                                if (g1 >= 0 && g1 != lastG1) {
                                    lastG1 = g1;
                                    ImGui::PushStyleColor(ImGuiCol_Text, uDim);
                                    ImGui::TextUnformatted(("  " + wd.facets[1].opts[g1]).c_str());
                                    ImGui::PopStyleColor();
                                }
                            }
                            // right-aligned dim tag = the remaining facets (set / rarity / tier ...)
                            std::string tag;
                            for (int fi = 2; fi < (int)wd.facets.size(); ++fi) {
                                const int idx = wd.itemFacetIdx[k][fi];
                                if (idx >= 0) { if (!tag.empty()) tag += "  \xC2\xB7  "; tag += wd.facets[fi].opts[idx]; }
                            }
                            ImGui::PushID(k);
                            const float rowW = ImGui::GetContentRegionAvail().x;
                            const float rowH = ImGui::GetTextLineHeightWithSpacing();
                            ImGui::InvisibleButton("##itemrow", ImVec2(rowW, rowH));
                            const bool rowHovered = ImGui::IsItemHovered();
                            if (ImGui::IsItemClicked()) {
                                wd.listSel = k;
                                if (wd.onItemAdd && k < (int)wd.itemIds.size() && !wd.itemIds[k].empty()) {
                                    wd.actionSel = k;
                                    wd.actionCount = 1;
                                    openAddPopup = true;
                                }
                            }
                            {
                                const ImVec2 ra = ImGui::GetItemRectMin();
                                const ImVec2 rb = ImGui::GetItemRectMax();
                                if (wd.listSel == k || rowHovered) {
                                    const ImU32 bg = wd.listSel == k ? IM_COL32(0xE0, 0xB3, 0x4A, 42)
                                                                     : IM_COL32(0xE0, 0xB3, 0x4A, 24);
                                    listDl->AddRectFilled(ra, rb, bg, 4.0f * s);
                                }
                                const float rowW = rb.x - ra.x;
                                const float tagGap = 10.0f * sx;
                                float tagW = 0.0f;
                                if (!tag.empty()) {
                                    tagW = fBody_ ? fBody_->CalcTextSizeA(15.0f * s, 3.4e38f, 0.0f, tag.c_str()).x
                                                   : ImGui::CalcTextSize(tag.c_str()).x;
                                    tagW = std::min(tagW + 10.0f * sx, rowW * 0.42f);
                                }
                                const float tagLeft = rb.x - tagW;
                                const float itemRight = tag.empty() ? rb.x : std::max(ra.x, tagLeft - tagGap);
                                DrawFieldTextClipped(listDl, fBody_, 15.0f * s, ra, ImVec2(itemRight, rb.y),
                                                     wd.items[k].c_str(), uText, 6.0f * sx, 1.0f * sy, false,
                                                     &listClip);
                                if (!tag.empty()) {
                                    DrawFieldTextClipped(listDl, fBody_, 15.0f * s, ImVec2(tagLeft, ra.y), rb,
                                                         tag.c_str(), uFaint, 4.0f * sx, 1.0f * sy, false,
                                                         &listClip);
                                }
                            }
                            ImGui::PopID();
                        }
                    }
                    listDl->PopClipRect();
                    DrawScrollArrowCaps("item_list_scroll", listDl, sx, sy, s, uAccent);
                    ImGui::EndChild();
                    if (openAddPopup) {
                        ImGui::OpenPopup("Spawn item##count");
                    }
                    if (wd.onItemAdd) {
                        const float modalW = std::max(330.0f * sx, 300.0f);
                        const float modalH = std::max(186.0f * sy, 160.0f);
                        ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f),
                                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                        ImGui::SetNextWindowSize(ImVec2(modalW, modalH), ImGuiCond_Appearing);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * sx, 16.0f * sy));
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * s);
                        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * s);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * sx, 6.0f * sy));
                        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0x03, 0x03, 0x05, 165));
                        ImGui::PushStyleColor(ImGuiCol_WindowBg,         IM_COL32(0x14, 0x12, 0x10, 252));
                        ImGui::PushStyleColor(ImGuiCol_Border,           IM_COL32(0xE0, 0xB3, 0x4A, 125));
                        ImGui::PushStyleColor(ImGuiCol_FrameBg,          IM_COL32(0x23, 0x1E, 0x16, 245));
                        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   IM_COL32(0x2C, 0x25, 0x19, 255));
                        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    IM_COL32(0x32, 0x2A, 0x1C, 255));
                        ImGui::PushStyleColor(ImGuiCol_Button,           IM_COL32(0x25, 0x20, 0x17, 245));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    IM_COL32(0x3A, 0x30, 0x1E, 255));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,     IM_COL32(0xB8, 0x86, 0x2F, 210));
                        ImGui::PushStyleColor(ImGuiCol_Text,             uText);
                        if (ImGui::BeginPopupModal("Spawn item##count", nullptr,
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                            ImDrawList* popDl = ImGui::GetWindowDrawList();
                            const ImVec2 mp = ImGui::GetWindowPos();
                            const ImVec2 ms = ImGui::GetWindowSize();
                            const int k = wd.actionSel;
                            const char* itemName = (k >= 0 && k < (int)wd.items.size()) ? wd.items[k].c_str() : "";
                            AddTextCX(popDl, fTab_, 15.0f * s,
                                      ImVec2(mp.x + 18.0f * sx, mp.y + 15.0f * sy),
                                      uAccent, "SPAWN ITEM", XS);
                            popDl->AddLine(ImVec2(mp.x + 18.0f * sx, mp.y + 40.0f * sy),
                                           ImVec2(mp.x + ms.x - 18.0f * sx, mp.y + 40.0f * sy),
                                           IM_COL32(0xE0, 0xB3, 0x4A, 60), 1.0f * s);

                            const ImVec2 ia(mp.x + 18.0f * sx, mp.y + 50.0f * sy);
                            const ImVec2 ib(mp.x + ms.x - 18.0f * sx, ia.y + 34.0f * sy);
                            RoundedVGrad(popDl, ia, ib, IM_COL32(0xE0, 0xB3, 0x4A, 30),
                                                     IM_COL32(0xE0, 0xB3, 0x4A, 12), 5.0f * s);
                            popDl->AddRect(ia, ib, IM_COL32(0xE0, 0xB3, 0x4A, 75), 5.0f * s, 0, 1.0f * s);
                            DrawFieldTextClipped(popDl, fBody_, 15.0f * s, ia, ib, itemName,
                                                 uText, 9.0f * sx, 3.0f * sy, false);

                            ImGui::SetCursorScreenPos(ImVec2(mp.x + 18.0f * sx, mp.y + 96.0f * sy));
                            ImGui::TextColored(Rgb(0x9C, 0x94, 0x86), "Count");
                            ImGui::SameLine(0.0f, 12.0f * sx);
                            const float stepH = std::max(28.0f * sy, 24.0f);
                            const float stepW = std::max(30.0f * sx, 28.0f);
                            const float inputW = std::max(92.0f * sx, 78.0f);
                            if (ImGui::Button("-", ImVec2(stepW, stepH)) && wd.actionCount > 1)
                                --wd.actionCount;
                            ImGui::SameLine(0.0f, 6.0f * sx);
                            ImGui::SetNextItemWidth(inputW);
                            if (ImGui::InputInt("##count", &wd.actionCount, 0, 0,
                                                ImGuiInputTextFlags_CharsDecimal)) {
                                if (wd.actionCount < 1) wd.actionCount = 1;
                                if (wd.actionCount > 9999) wd.actionCount = 9999;
                            }
                            ImGui::SameLine(0.0f, 6.0f * sx);
                            if (ImGui::Button("+", ImVec2(stepW, stepH)) && wd.actionCount < 9999)
                                ++wd.actionCount;

                            if (wd.actionCount < 1) wd.actionCount = 1;
                            if (wd.actionCount > 9999) wd.actionCount = 9999;

                            const float btnW = (ms.x - 46.0f * sx) * 0.5f;
                            const float btnH = std::max(30.0f * sy, 26.0f);
                            ImGui::SetCursorScreenPos(ImVec2(mp.x + 18.0f * sx, mp.y + ms.y - 42.0f * sy));
                            if (ImGui::Button("SPAWN", ImVec2(btnW, btnH))) {
                                if (k >= 0 && k < (int)wd.itemIds.size())
                                    wd.onItemAdd(wd.itemIds[k].c_str(), wd.actionCount);
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SameLine(0.0f, 10.0f * sx);
                            if (ImGui::Button("CANCEL", ImVec2(btnW, btnH))) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopStyleColor(10);
                        ImGui::PopStyleVar(5);
                    }
                    ImGui::PopStyleVar();       // list child WindowPadding
                    ImGui::PopStyleVar(4);      // FrameRounding, PopupRounding, FramePadding, ScrollbarSize
                    ImGui::PopStyleColor(19);
                    ImGui::PopFont();
                    ry = listBotAS + 10.0f;
                } else {
                    txt(fBody_, 17.0f * s, 60, ry + 3, uDim, wd.text.c_str());
                }
                ImGui::PopID();
                ry += 40;
            }
            if (pg.widgets.empty())
                txt(fBody_, 17.0f * s, 60, ry, uDim, "This page has no options yet.");
        }

        // ---- CLOSE button (AS: x60 y376 w152 h40 r7; vgrad alpha 18->6, border 42; hover 34/14/78)
        //      EXCLUSIVE to the Welcome tab (AS showCloseButton(card, idx == 0)) ----
        if (activeTab_ == 0) {
            const ImVec2 ba(X(60), Y(376)), bb(X(60) + 152.0f * sx, Y(376) + 40.0f * sy);
            const float br = 7.0f * s;
            ImGui::SetCursorScreenPos(ba);
            ImGui::InvisibleButton("##close", ImVec2(bb.x - ba.x, bb.y - ba.y));
            const bool bhov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) menuOpen_ = false;
            RoundedVGrad(dl, ba, bb, IM_COL32(0xE0, 0xB3, 0x4A, bhov ? 87 : 46),
                                     IM_COL32(0xE0, 0xB3, 0x4A, bhov ? 36 : 15), br);
            dl->AddRect(ba, bb, IM_COL32(0xE0, 0xB3, 0x4A, bhov ? 199 : 107), br, 0, 1.0f * s);
            const float clPx = 15.0f * s;
            const float clW = measureW(fTab_, clPx, "CLOSE");
            const float clH = fTab_ ? fTab_->CalcTextSizeA(clPx, 3.4e38f, 0.0f, "CLOSE").y : clPx;
            AddTextCX(dl, fTab_, clPx, ImVec2(ba.x + (bb.x - ba.x - clW) * 0.5f, ba.y + (bb.y - ba.y - clH) * 0.5f),
                      uAccent, "CLOSE", XS);
            // hint (AS: x=230 y~387, regular size 14) — vertically centered to the button
            const float hintPx = 14.0f * s;
            const float hintH = fSmall_ ? fSmall_->CalcTextSizeA(hintPx, 3.4e38f, 0.0f, "or press  ESC").y : hintPx;
            AddTextCX(dl, fSmall_, hintPx, ImVec2(X(230), ba.y + (bb.y - ba.y - hintH) * 0.5f), uFaint, "or press  ESC", XS);
        }

        // ---- footer (AS: right edge x=cw-30, y=ch-40, bold size 11, right-aligned) — above the BR mark.
        //      WELCOME tab only: mod pages own the whole content band below the tabs. ----
        if (activeTab_ == 0) {
            const char* est = "HAGRYPH  \xC2\xB7  EST. MMXXVI";
            const float fPx = 11.0f * s;
            const float estW = measureW(fFoot_, fPx, est);
            AddTextCX(dl, fFoot_, fPx, ImVec2(X(820 - 30) - estW, Y(462 - 40)), uFaint, est, XS);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

}  // namespace sow
