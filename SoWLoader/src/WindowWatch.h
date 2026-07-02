#pragma once
#include "PCH.h"

namespace sow {

// Console trigger: a direct MinHook trampoline on user32!ShowWindow. Opening the console at window
// CREATION (CreateWindowExW) was too early — the game window is still a hidden 336x239 stub with no
// taskbar button, so the console became the process's "main"/foreground window. The taskbar button
// is created when the window is first SHOWN, so we trigger there instead: when ShowWindow makes the
// game's main window (top-level, class L"Shadow of War") visible, its taskbar button now exists and
// the game claims foreground a few ms later, so the console opens as a secondary window — never the
// main one. Still well before the start menu. Owns the ShowWindow hook exclusively.
class WindowWatch {
public:
    static WindowWatch& Get();
    void Install();   // arm the ShowWindow hook (idempotent; call from the worker)

    WindowWatch(const WindowWatch&) = delete;
    WindowWatch& operator=(const WindowWatch&) = delete;

private:
    WindowWatch() = default;
    bool installed_ = false;
};

}  // namespace sow
