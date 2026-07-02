#pragma once
#include "PCH.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace sow {

// In-DLL API tracer: MinHook-trampolines a curated set of Win32 + Direct3D 11 entry points and logs
// each call (args summarised) to its own channel (logs\Trace.log + [Trace] console lines). Because
// our proxy DLL is already loaded and Denuvo-tolerated, this gives the "what Windows/DirectX calls is
// the game making" trace WITHOUT an external debugger — no anti-debug to fight. Every hook is a
// trampoline that calls the original (mod-safe, never destructive).
//
// Three groups, each toggled in config\SoWLoader.ini [Trace] and rate-capped (first N calls per API,
// so hot paths like Map/Draw can't flood the log or tank the frame rate):
//   window  — user32 window lifecycle (CreateWindowExW, ShowWindow, SetWindowPos, ...)   [Windows calls]
//   gfxmem  — ID3D11Device::CreateBuffer/CreateTexture2D + Context::Map                  [graphics memory]
//   draws   — ID3D11DeviceContext::DrawIndexed/Draw                                      [DirectX draw calls]
class Tracer {
public:
    static Tracer& Get();

    void Install();                                       // export hooks (user32) — from the worker
    void OnDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx);   // D3D11 vtable hooks — from Overlay

    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;

private:
    Tracer() = default;

    bool enabled_ = true, window_ = true, gfxmem_ = true, draws_ = false;
    int  cap_ = 30;
    bool exportsDone_ = false, vtablesDone_ = false;
};

}  // namespace sow
