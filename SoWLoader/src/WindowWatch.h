#pragma once
#include "PCH.h"

namespace sow {

// In-process Window/System hook watching the GAME's own window lifecycle. Our DLL already lives in
// the process, so a SetWinEventHook filtered to our PID reports the exact moment the game's main
// window STARTS TO EXIST (EVENT_OBJECT_CREATE) — no RVA, no engine internals, just the OS telling us
// the window was created. That is our console-open trigger (the console opens non-activating, so it
// never becomes the foreground/taskbar "main" window).
//
// CRITICAL: a WinEvent hook is bound to the thread that installs it and dies when that thread exits.
// The first attempt armed it on the Loader worker (which returns immediately), so the hook was torn
// down seconds later and never fired. This version owns a DEDICATED thread that installs the hook
// and runs a message loop (WINEVENT_OUTOFCONTEXT delivers callbacks to that thread), so the hook
// lives until we stop it.
class WindowWatch {
public:
    static WindowWatch& Get();

    void Install();   // spawn the watcher thread + arm the hook (idempotent)
    void Stop();      // unhook + end the watcher thread (auto-called once the window is caught)

    WindowWatch(const WindowWatch&) = delete;
    WindowWatch& operator=(const WindowWatch&) = delete;

private:
    WindowWatch() = default;

    static DWORD WINAPI ThreadThunk(LPVOID self);
    void ThreadMain();
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                    LONG idObject, LONG idChild, DWORD tid, DWORD time);
    static bool IsGameMainWindow(HWND hwnd);

    HANDLE        thread_   = nullptr;
    DWORD         threadId_ = 0;
    HWINEVENTHOOK hook_     = nullptr;
    bool          installed_ = false;
    volatile LONG fired_    = 0;   // one-shot guard (callback thread + Stop)
};

}  // namespace sow
