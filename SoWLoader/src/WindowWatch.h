#pragma once
#include "PCH.h"

namespace sow {

// In-process Window/System hook (SetWinEventHook, WINEVENT_INCONTEXT, own PID only) watching the
// GAME's window lifecycle — our DLL already lives in the process, so the callbacks run
// synchronously on the thread that generates the event: no polling, no message pump, no RVA.
//  - EVENT_OBJECT_CREATE     : the instant the game main window STARTS TO EXIST (logged; the
//                              window is a 336x239 unactivated stub here — too early for the
//                              console, which would win the foreground and become the "main"
//                              window; that is exactly what we are fixing).
//  - EVENT_SYSTEM_FOREGROUND : the game main window has BECOME THE FOREGROUND window — the only
//                              moment that guarantees the console opens BEHIND it. Console opens
//                              here, then the hooks self-remove (one-shot).
class WindowWatch {
public:
    static WindowWatch& Get();

    void Install();     // arm both WinEvent hooks (idempotent; call from the worker at startup)
    void Uninstall();   // drop the hooks (called automatically once the console has opened)

    WindowWatch(const WindowWatch&) = delete;
    WindowWatch& operator=(const WindowWatch&) = delete;

private:
    WindowWatch() = default;

    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                    LONG idObject, LONG idChild, DWORD tid, DWORD time);
    static bool IsGameMainWindow(HWND hwnd);

    HWINEVENTHOOK create_ = nullptr;   // EVENT_OBJECT_CREATE
    HWINEVENTHOOK fg_     = nullptr;   // EVENT_SYSTEM_FOREGROUND
    bool installed_ = false;
};

}  // namespace sow
