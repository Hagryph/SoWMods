#include "Overlay.h"
#include "Log.h"
#include "HagUI.h"
#include "Loader.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <MinHook.h>

#include <vector>
#include <algorithm>

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
        log.Line("[overlay] Present hooked (vtable slot 8) — used only to detect the game window (no drawing)");
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

void Overlay::DrawFrame(IDXGISwapChain* swap) {
    if (!ctx_) {
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev_))) || !dev_) return;
        dev_->GetImmediateContext(&ctx_);
    }
    if (!resTried_) { resTried_ = true; resReady_ = BuildResources(dev_); }
    if (!resReady_) return;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = dev_->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
    if (FAILED(hr) || !rtv) return;
    curW_ = (float)bd.Width; curH_ = (float)bd.Height;

    if (firstFrame_) {
        firstFrame_ = false;
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(swap->GetDesc(&sd))) gameWnd_ = sd.OutputWindow;
        Loader::Get().OnRenderLive();     // game window up + rendering -> open the console
    }

    // -------- save the game's pipeline state --------
    constexpr UINT kVP = 16;
    struct S {
        UINT scN = kVP; D3D11_RECT sc[kVP]; UINT vpN = kVP; D3D11_VIEWPORT vp[kVP];
        ID3D11RasterizerState* rs = nullptr; ID3D11BlendState* bs = nullptr; FLOAT bf[4]{}; UINT bmask = 0;
        ID3D11DepthStencilState* dss = nullptr; UINT sref = 0;
        ID3D11ShaderResourceView* srv = nullptr; ID3D11SamplerState* samp = nullptr;
        ID3D11Buffer* cb = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11VertexShader* vs = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topo{}; ID3D11Buffer* vb = nullptr; UINT stride = 0, off = 0;
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibf{}; UINT iboff = 0; ID3D11InputLayout* il = nullptr;
        ID3D11RenderTargetView* rtvs[8]{}; ID3D11DepthStencilView* dsv = nullptr;
    } s;
    ctx_->RSGetScissorRects(&s.scN, s.sc); ctx_->RSGetViewports(&s.vpN, s.vp); ctx_->RSGetState(&s.rs);
    ctx_->OMGetBlendState(&s.bs, s.bf, &s.bmask); ctx_->OMGetDepthStencilState(&s.dss, &s.sref);
    ctx_->PSGetShaderResources(0, 1, &s.srv); ctx_->PSGetSamplers(0, 1, &s.samp);
    ctx_->PSGetConstantBuffers(0, 1, &s.cb); ctx_->PSGetShader(&s.ps, nullptr, nullptr);
    ctx_->VSGetShader(&s.vs, nullptr, nullptr); ctx_->IAGetPrimitiveTopology(&s.topo);
    ctx_->IAGetVertexBuffers(0, 1, &s.vb, &s.stride, &s.off); ctx_->IAGetIndexBuffer(&s.ib, &s.ibf, &s.iboff);
    ctx_->IAGetInputLayout(&s.il); ctx_->OMGetRenderTargets(8, s.rtvs, &s.dsv);

    // -------- set our state --------
    D3D11_VIEWPORT vp{}; vp.Width = curW_; vp.Height = curH_; vp.MaxDepth = 1.0f;
    const UINT stride = sizeof(Vtx), off = 0; const FLOAT bf[4] = { 0, 0, 0, 0 };
    ctx_->OMSetRenderTargets(1, &rtv, nullptr); ctx_->RSSetViewports(1, &vp); ctx_->RSSetState(raster_);
    ctx_->OMSetBlendState(blend_, bf, 0xFFFFFFFF); ctx_->OMSetDepthStencilState(depthOff_, 0);
    ctx_->IASetInputLayout(layout_); ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &off);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_, nullptr, 0); ctx_->PSSetShader(ps_, nullptr, 0); ctx_->PSSetSamplers(0, 1, &samp_);

    // -------- draw: watermark ONLY (the HagUI hub is now the external WebView2 overlay) --------
    {
        const std::string wm = "SoWLoader - Hagryph";
        float tw, th; MeasureText(wm, 16, tw, th);
        DrawText(curW_ - tw - 14.0f, 12.0f, wm, 16, { 0.80f, 0.80f, 0.82f, 0.70f });
        const std::string hint = "F8  Menu";
        float hw, hh; MeasureText(hint, 13, hw, hh);
        DrawText(curW_ - hw - 14.0f, 12.0f + th - 2.0f, hint, 13, { 0.88f, 0.70f, 0.29f, 0.65f });
    }
    if (!loggedDraw_) { Log::Get().Line("[overlay] watermark drawn (HagUI hub is the WebView2 overlay)"); loggedDraw_ = true; }

    // -------- restore --------
    ctx_->RSSetScissorRects(s.scN, s.sc); ctx_->RSSetViewports(s.vpN, s.vp);
    ctx_->RSSetState(s.rs); if (s.rs) s.rs->Release();
    ctx_->OMSetBlendState(s.bs, s.bf, s.bmask); if (s.bs) s.bs->Release();
    ctx_->OMSetDepthStencilState(s.dss, s.sref); if (s.dss) s.dss->Release();
    ctx_->PSSetShaderResources(0, 1, &s.srv); if (s.srv) s.srv->Release();
    ctx_->PSSetSamplers(0, 1, &s.samp); if (s.samp) s.samp->Release();
    ctx_->PSSetConstantBuffers(0, 1, &s.cb); if (s.cb) s.cb->Release();
    ctx_->PSSetShader(s.ps, nullptr, 0); if (s.ps) s.ps->Release();
    ctx_->VSSetShader(s.vs, nullptr, 0); if (s.vs) s.vs->Release();
    ctx_->IASetPrimitiveTopology(s.topo);
    ctx_->IASetVertexBuffers(0, 1, &s.vb, &s.stride, &s.off); if (s.vb) s.vb->Release();
    ctx_->IASetIndexBuffer(s.ib, s.ibf, s.iboff); if (s.ib) s.ib->Release();
    ctx_->IASetInputLayout(s.il); if (s.il) s.il->Release();
    ctx_->OMSetRenderTargets(8, s.rtvs, s.dsv);
    for (auto* r : s.rtvs) if (r) r->Release();
    if (s.dsv) s.dsv->Release();
    rtv->Release();
}

}  // namespace sow
