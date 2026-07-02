#include "Overlay.h"
#include "Log.h"
#include "HagUI.h"
#include "Loader.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <vector>
#include <algorithm>

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

// Subclassed onto the GAME window: ImGui reads input from the game's own message stream (no separate
// window -> no focus split, no z-order/monitor problems). While the hub is open we swallow input so it
// doesn't leak to the game; when closed everything passes straight through.
LRESULT __stdcall Overlay::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Overlay& o = Get();
    // F8 toggles the hub (ignore auto-repeat: bit 30 of lParam set == key was already down).
    if (msg == WM_KEYDOWN && w == VK_F8 && (l & 0x40000000) == 0) o.menuOpen_ = !o.menuOpen_;
    // ESC closes the hub while it's open (cursor visibility is reconciled in DrawFrame).
    if (msg == WM_KEYDOWN && w == VK_ESCAPE && o.menuOpen_) o.menuOpen_ = false;

    if (o.imguiInit_) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        if (o.menuOpen_) {
            // Keep a crisp HARDWARE cursor (the ImGui software cursor lags at the menu's frame rate).
            if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            switch (msg) {   // modal: keep these away from the game while the hub is up
                case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                    return 0;
            }
        }
    }
    return ::CallWindowProcW(o.origWndProc_, h, msg, w, l);
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

    ImGui::GetIO().MouseDrawCursor = false;   // use the smooth OS hardware cursor, not ImGui's software one
    // Reconcile the OS cursor with the hub state (balanced ShowCursor +1/-1), covering every close
    // path (F8, ESC, CLOSE button) from one place.
    if (menuOpen_ != cursorShown_) { ::ShowCursor(menuOpen_); cursorShown_ = menuOpen_; }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawWatermark();
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

// Rounded rect with a vertical color gradient (AddRectFilledMultiColor can't round). Straight
// gradient middle + rounded caps top/bottom; caps are clipped to their zone so the radius isn't
// clamped by a short rect. Colors are continuous, so it reads as one smooth gradient.
static void RoundedVGrad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot, float rad) {
    if (rad <= 0.5f || b.y - a.y <= 2.0f * rad) { dl->AddRectFilledMultiColor(a, b, top, top, bot, bot); return; }
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + rad), ImVec2(b.x, b.y - rad), top, top, bot, bot);
    dl->PushClipRect(ImVec2(a.x, a.y), ImVec2(b.x, a.y + rad), true);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + 2.0f * rad), top, rad, ImDrawFlags_RoundCornersTop);
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(a.x, b.y - rad), ImVec2(b.x, b.y), true);
    dl->AddRectFilled(ImVec2(a.x, b.y - 2.0f * rad), b, bot, rad, ImDrawFlags_RoundCornersBottom);
    dl->PopClipRect();
}
void Overlay::StyleHagUI() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6; s.ChildRounding = 4; s.FrameRounding = 4; s.TabRounding = 3;
    s.WindowBorderSize = 1; s.WindowPadding = ImVec2(28, 24);
    s.ItemSpacing = ImVec2(14, 12); s.FramePadding = ImVec2(16, 8);

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
    fBody_  = add("C:\\Windows\\Fonts\\segoeui.ttf");    // first == default font
    fKick_  = add("C:\\Windows\\Fonts\\segoeuib.ttf");
    fTab_   = add("C:\\Windows\\Fonts\\segoeuib.ttf");
    fSmall_ = add("C:\\Windows\\Fonts\\segoeui.ttf");
    fFoot_  = add("C:\\Windows\\Fonts\\segoeuib.ttf");
    fWord_  = add("C:\\Windows\\Fonts\\georgiab.ttf");
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
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
        auto X = [&](float ax) { return p0.x + ax * sx; };   // AS x -> screen
        auto Y = [&](float ay) { return p0.y + ay * sy; };   // AS y -> screen
        auto txt = [&](ImFont* f, float px, float ax, float ay, ImU32 c, const char* t) {
            dl->AddText(f, px, ImVec2(X(ax), Y(ay)), c, t);
        };
        auto measure = [&](ImFont* f, float px, const char* t) {
            return f ? f->CalcTextSizeA(px, 3.4e38f, 0.0f, t) : ImGui::CalcTextSize(t);
        };

        // ---- left accent: glow (x0..30, alpha 26->0) + rail (x0..6, vgrad accent->accent-dim),
        //      both MASKED to the rounded card (draw card-wide rounded-left, clip to the band width) ----
        auto glowLayer = [&](float wpx, int a) {
            dl->PushClipRect(p0, ImVec2(p0.x + wpx, p0.y + ch), true);
            dl->AddRectFilled(p0, ImVec2(p0.x + cw, p0.y + ch), IM_COL32(0xE0, 0xB3, 0x4A, a), r, ImDrawFlags_RoundCornersLeft);
            dl->PopClipRect();
        };
        glowLayer(30.0f * sx, 10);
        glowLayer(20.0f * sx, 16);
        glowLayer(12.0f * sx, 24);
        dl->PushClipRect(p0, ImVec2(p0.x + 6.0f * sx, p0.y + ch), true);
        RoundedVGrad(dl, p0, ImVec2(p0.x + cw, p0.y + ch),
                     IM_COL32(0xE0, 0xB3, 0x4A, 255), IM_COL32(0xB8, 0x86, 0x2F, 255), r);
        dl->PopClipRect();

        // ---- corner flourishes (AS: gold alpha 30, TL x30..56 y22, BR x cw-56..cw-30 y ch-22) ----
        const ImU32 uFl = IM_COL32(0xE0, 0xB3, 0x4A, 77);
        dl->AddLine(ImVec2(X(30), Y(22)), ImVec2(X(56), Y(22)), uFl, 1.0f * s);
        dl->AddLine(ImVec2(X(820 - 56), Y(462 - 22)), ImVec2(X(820 - 30), Y(462 - 22)), uFl, 1.0f * s);

        // ---- tabs (AS: nx=60 ny=28, pad16 gap12, bold size15, hairline + active underline at ny+34) ----
        static const char* kTabs[] = { "WELCOME", "GENERAL" };
        const float fTabPx = 15.0f * s, pad = 16.0f * sx, gap = 12.0f * sx, cellH = 35.0f * sy;
        const float navY = Y(28), hairY = Y(28 + 34);
        float cx = X(60);
        for (int i = 0; i < 2; ++i) {
            const bool active = activeTab_ == i;
            const float cellW = measure(fTab_, fTabPx, kTabs[i]).x + pad * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(cx, navY));
            ImGui::InvisibleButton(kTabs[i], ImVec2(cellW, cellH));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) activeTab_ = i;
            dl->AddText(fTab_, fTabPx, ImVec2(cx + pad, navY + 6.0f * sy),
                        active ? uAccent : (hov ? uText : uDim), kTabs[i]);
            if (active) dl->AddLine(ImVec2(cx, hairY), ImVec2(cx + cellW, hairY), uAccent, 2.0f * s);
            cx += cellW + gap;
        }
        dl->AddLine(ImVec2(X(60), hairY), ImVec2(X(772), hairY), IM_COL32(0xE0, 0xB3, 0x4A, 41), 1.0f * s);

        // ---- active page ----
        if (activeTab_ == 0) {   // content origin AS: x=60 y=86
            txt(fKick_, 13.0f * s, 60, 86, uFaint, "W E L C O M E   T O");
            const float wy = 86 + 14;                                  // wordmark (AS x-2, y+14, size 64)
            txt(fWord_, 64.0f * s, 58, wy, uText, "Hag");
            const float hagW = measure(fWord_, 64.0f * s, "Hag").x;
            dl->AddText(fWord_, 64.0f * s, ImVec2(X(58) + hagW, Y(wy)), uAccent, "UI");
            dl->AddRectFilledMultiColor(ImVec2(X(60), Y(198)), ImVec2(X(60) + 250.0f * sx, Y(198) + 2.0f * sy),
                IM_COL32(0xE0, 0xB3, 0x4A, 153), IM_COL32(0xE0, 0xB3, 0x4A, 0),
                IM_COL32(0xE0, 0xB3, 0x4A, 0),   IM_COL32(0xE0, 0xB3, 0x4A, 153));   // divider (x60 y198 w250)
            txt(fBody_, 18.0f * s, 60, 218, uDim, "Your private control room for every Hagryph mod \xE2\x80\x94");
            txt(fBody_, 18.0f * s, 60, 218 + 26, uDim, "configuration, tools, and more, gathered in one place.");
        } else {
            txt(fBody_, 18.0f * s, 60, 100, uDim, "General settings will appear here.");
        }

        // ---- CLOSE button (AS: x60 y376 w152 h40 r7; vgrad alpha 18->6, border 42; hover 34/14/78) ----
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
        const ImVec2 clSz = measure(fTab_, clPx, "CLOSE");
        dl->AddText(fTab_, clPx, ImVec2(ba.x + (bb.x - ba.x - clSz.x) * 0.5f, ba.y + (bb.y - ba.y - clSz.y) * 0.5f),
                    uAccent, "CLOSE");
        // hint (AS: x=230 y~387, regular size 14) — vertically centered to the button
        const float hintPx = 14.0f * s;
        const float hintH = measure(fSmall_, hintPx, "or press  ESC").y;
        dl->AddText(fSmall_, hintPx, ImVec2(X(230), ba.y + (bb.y - ba.y - hintH) * 0.5f), uFaint, "or press  ESC");

        // ---- footer (AS: right edge x=cw-30, y=ch-40, bold size 11, right-aligned) — above the BR mark ----
        const char* est = "HAGRYPH  \xC2\xB7  EST. MMXXVI";
        const float fPx = 11.0f * s;
        const float estW = measure(fFoot_, fPx, est).x;
        dl->AddText(fFoot_, fPx, ImVec2(X(820 - 30) - estW, Y(462 - 40)), uFaint, est);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

}  // namespace sow
