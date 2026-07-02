#pragma once
#include "PCH.h"

namespace sow {

// Console trigger: a direct MinHook trampoline on user32!CreateWindowExW. When the engine creates
// its main window (class L"Shadow of War", top-level), the detour opens the mod console RIGHT THERE,
// then calls the original. This is the deterministic "the window just came into existence" point —
// reliable and in-process (the earlier SetWinEventHook approach was flaky and sometimes never fired).
// Owns the CreateWindowExW hook exclusively (the Tracer must NOT also hook it — one hook per target).
class WindowWatch {
public:
    static WindowWatch& Get();
    void Install();   // arm the CreateWindowExW hook (idempotent; call from the worker)

    WindowWatch(const WindowWatch&) = delete;
    WindowWatch& operator=(const WindowWatch&) = delete;

private:
    WindowWatch() = default;
    bool installed_ = false;
};

}  // namespace sow
