#include "Tracer.h"
#include "Log.h"
#include "Config.h"

#include <d3d11.h>
#include <MinHook.h>

namespace sow {

Tracer& Tracer::Get() { static Tracer t; return t; }

// One log channel for the whole tracer.
static Logger& TL() { return Log::Channel("Trace"); }

// ---- rate cap: log the first `cap` calls of an API, then one "suppressed" line, then stay quiet ----
struct Cap {
    volatile LONG left;
    volatile LONG capped;   // 0/1, set once when we print the suppression notice
    bool Take(const char* api) {
        if (::InterlockedDecrement(&left) >= 0) return true;
        if (::InterlockedCompareExchange(&capped, 1, 0) == 0) {
            char l[96]; ::wsprintfA(l, "[%.40s] rate cap reached — further calls suppressed", api);
            TL().Line(l);
        }
        return false;
    }
};

// ===========================================================================
//  Windows calls — user32 window lifecycle (exported functions, easy targets).
//  CreateWindowExW is intentionally absent — WindowWatch owns that hook (console trigger).
// ===========================================================================
using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
using SetWinPosFn  = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SetFgWinFn   = BOOL(WINAPI*)(HWND);
using DestroyWinFn = BOOL(WINAPI*)(HWND);

static ShowWindowFn   oShowWindow   = nullptr;
static SetWinPosFn    oSetWindowPos = nullptr;
static SetFgWinFn     oSetFgWin     = nullptr;
static DestroyWinFn   oDestroyWin   = nullptr;

static Cap cShow, cPos, cFg, cDestroy;

static BOOL WINAPI HkShowWindow(HWND h, int cmd) {
    if (cShow.Take("ShowWindow")) { char l[96]; ::wsprintfA(l, "[ShowWindow] hwnd=%p nCmdShow=%d", h, cmd); TL().Line(l); }
    return oShowWindow(h, cmd);
}
static BOOL WINAPI HkSetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT f) {
    if (cPos.Take("SetWindowPos")) {
        char l[160]; ::wsprintfA(l, "[SetWindowPos] hwnd=%p after=%p %d,%d %dx%d flags=0x%X", h, after, x, y, w, hh, f);
        TL().Line(l);
    }
    return oSetWindowPos(h, after, x, y, w, hh, f);
}
static BOOL WINAPI HkSetForegroundWindow(HWND h) {
    if (cFg.Take("SetForegroundWindow")) { char l[80]; ::wsprintfA(l, "[SetForegroundWindow] hwnd=%p", h); TL().Line(l); }
    return oSetFgWin(h);
}
static BOOL WINAPI HkDestroyWindow(HWND h) {
    if (cDestroy.Take("DestroyWindow")) { char l[80]; ::wsprintfA(l, "[DestroyWindow] hwnd=%p", h); TL().Line(l); }
    return oDestroyWin(h);
}

// ===========================================================================
//  DirectX 11 — device resource creation + context calls (vtable hooks on the
//  GAME's real device/context, so they fire for every allocation/draw/map)
// ===========================================================================
using CreateBufferFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CreateTex2DFn   = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using MapFn           = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using DrawIndexedFn   = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn          = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);

static CreateBufferFn oCreateBuffer = nullptr;
static CreateTex2DFn  oCreateTex2D  = nullptr;
static MapFn          oMap          = nullptr;
static DrawIndexedFn  oDrawIndexed  = nullptr;
static DrawFn         oDraw         = nullptr;

static Cap cBuf, cTex, cMap, cDrawI, cDraw;

static HRESULT STDMETHODCALLTYPE HkCreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* d,
                                                const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    if (d && cBuf.Take("ID3D11Device::CreateBuffer")) {
        char l[200]; ::wsprintfA(l, "[gfxmem] CreateBuffer bytes=%u bind=0x%X usage=%d cpuAccess=0x%X (GPU alloc)",
                                 d->ByteWidth, d->BindFlags, (int)d->Usage, d->CPUAccessFlags);
        TL().Line(l);
    }
    return oCreateBuffer(self, d, init, out);
}
static HRESULT STDMETHODCALLTYPE HkCreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* d,
                                                   const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    if (d && cTex.Take("ID3D11Device::CreateTexture2D")) {
        char l[220]; ::wsprintfA(l, "[gfxmem] CreateTexture2D %ux%u fmt=%d mips=%u bind=0x%X usage=%d (GPU alloc)",
                                 d->Width, d->Height, (int)d->Format, d->MipLevels, d->BindFlags, (int)d->Usage);
        TL().Line(l);
    }
    return oCreateTex2D(self, d, init, out);
}
static HRESULT STDMETHODCALLTYPE HkMap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub,
                                       D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    if (cMap.Take("ID3D11DeviceContext::Map")) {
        char l[160]; ::wsprintfA(l, "[gfxmem] Map res=%p sub=%u type=%d (CPU<->GPU memory access)", (void*)res, sub, (int)type);
        TL().Line(l);
    }
    return oMap(self, res, sub, type, flags, out);
}
static void STDMETHODCALLTYPE HkDrawIndexed(ID3D11DeviceContext* self, UINT idx, UINT start, INT base) {
    if (cDrawI.Take("ID3D11DeviceContext::DrawIndexed")) {
        char l[128]; ::wsprintfA(l, "[draw] DrawIndexed indices=%u start=%u baseVtx=%d", idx, start, base); TL().Line(l);
    }
    oDrawIndexed(self, idx, start, base);
}
static void STDMETHODCALLTYPE HkDraw(ID3D11DeviceContext* self, UINT cnt, UINT start) {
    if (cDraw.Take("ID3D11DeviceContext::Draw")) {
        char l[128]; ::wsprintfA(l, "[draw] Draw verts=%u start=%u", cnt, start); TL().Line(l);
    }
    oDraw(self, cnt, start);
}

// ---- helpers ----
static bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (!target) { TL().Line(std::string("[trace] no target for ") + name); return false; }
    if (MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK) {
        TL().Good(std::string("[trace] hooked ") + name);
        return true;
    }
    TL().Error(std::string("[trace] FAILED to hook ") + name);
    return false;
}
static void* Vt(void* iface, int idx) { return (*reinterpret_cast<void***>(iface))[idx]; }

void Tracer::Install() {
    auto& cfg = Config::For("SoWLoader");
    enabled_ = cfg.GetBool("Trace", "enabled", true);
    window_  = cfg.GetBool("Trace", "window",  true);
    gfxmem_  = cfg.GetBool("Trace", "gfxmem",  true);
    draws_   = cfg.GetBool("Trace", "draws",   false);
    cap_     = cfg.GetInt ("Trace", "cap",     30);
    if (cap_ < 1) cap_ = 1;
    if (!enabled_) { TL().Line("[trace] disabled via config"); return; }

    // seed caps (set fields directly — the members are volatile)
    auto seed = [&](Cap& c) { c.left = cap_; c.capped = 0; };
    seed(cShow); seed(cPos); seed(cFg); seed(cDestroy);
    seed(cBuf); seed(cTex); seed(cMap); seed(cDrawI); seed(cDraw);

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { TL().Error("[trace] MH_Initialize failed"); return; }

    if (exportsDone_ || !window_) { if (!window_) TL().Line("[trace] window group off"); return; }
    exportsDone_ = true;
    if (HMODULE u32 = ::GetModuleHandleW(L"user32.dll")) {
        // NOTE: CreateWindowExW is hooked by WindowWatch (the console trigger) — one hook per target,
        // so the tracer must NOT hook it here. WindowWatch logs the game window's creation instead.
        Hook((void*)::GetProcAddress(u32, "ShowWindow"),          (void*)&HkShowWindow,          (void**)&oShowWindow,   "user32!ShowWindow");
        Hook((void*)::GetProcAddress(u32, "SetWindowPos"),        (void*)&HkSetWindowPos,        (void**)&oSetWindowPos, "user32!SetWindowPos");
        Hook((void*)::GetProcAddress(u32, "SetForegroundWindow"), (void*)&HkSetForegroundWindow, (void**)&oSetFgWin,     "user32!SetForegroundWindow");
        Hook((void*)::GetProcAddress(u32, "DestroyWindow"),       (void*)&HkDestroyWindow,       (void**)&oDestroyWin,   "user32!DestroyWindow");
    }
    TL().Good("[trace] window-call tracing armed (Windows calls)");
}

void Tracer::OnDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!enabled_ || vtablesDone_ || (!gfxmem_ && !draws_) || !dev || !ctx) return;
    vtablesDone_ = true;
    // ID3D11Device : IUnknown           -> 3 CreateBuffer, 5 CreateTexture2D.
    // ID3D11DeviceContext : ID3D11DeviceChild (which adds GetDevice/GetPrivateData/SetPrivateData/
    // SetPrivateDataInterface at 3..6 on top of IUnknown), so the context methods start at 7:
    //   12 DrawIndexed, 13 Draw, 14 Map, 15 Unmap. (Off-by-4 here = hooking PSSetSamplers -> crash.)
    if (gfxmem_) {
        Hook(Vt(dev, 3),  (void*)&HkCreateBuffer,    (void**)&oCreateBuffer, "ID3D11Device::CreateBuffer");
        Hook(Vt(dev, 5),  (void*)&HkCreateTexture2D, (void**)&oCreateTex2D,  "ID3D11Device::CreateTexture2D");
        Hook(Vt(ctx, 14), (void*)&HkMap,             (void**)&oMap,          "ID3D11DeviceContext::Map");
    }
    if (draws_) {
        Hook(Vt(ctx, 12), (void*)&HkDrawIndexed, (void**)&oDrawIndexed, "ID3D11DeviceContext::DrawIndexed");
        Hook(Vt(ctx, 13), (void*)&HkDraw,        (void**)&oDraw,        "ID3D11DeviceContext::Draw");
    }
    TL().Good("[trace] D3D11 vtable tracing armed (DirectX calls + graphics memory)");
}

}  // namespace sow
