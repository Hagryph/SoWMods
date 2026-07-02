#include "WindowWatch.h"
#include "Log.h"
#include "Loader.h"

namespace sow {

WindowWatch& WindowWatch::Get() { static WindowWatch w; return w; }

// The game's main window class, exactly as the engine registers it (RE: kWinMainThread registers
// class L"Shadow of War" before CreateWindowExW; see shared/GameOffsets.h).
bool WindowWatch::IsGameMainWindow(HWND hwnd) {
    if (!hwnd || ::GetAncestor(hwnd, GA_ROOT) != hwnd) return false;   // top-level only
    wchar_t cls[64]{};
    if (!::GetClassNameW(hwnd, cls, 64)) return false;
    return ::wcscmp(cls, L"Shadow of War") == 0;
}

void CALLBACK WindowWatch::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !IsGameMainWindow(hwnd)) return;
    if (event == EVENT_OBJECT_CREATE) {
        char l[112];
        ::wsprintfA(l, "[winwatch] game window %p CREATED (starts to exist; console waits for foreground)",
                    (void*)hwnd);
        Log::Get().Line(l);
    } else if (event == EVENT_SYSTEM_FOREGROUND) {
        char l[112];
        ::wsprintfA(l, "[winwatch] game window %p is FOREGROUND -> opening console behind it", (void*)hwnd);
        Log::Get().Line(l);
        Loader::Get().OnRenderLive();   // opens the console; the game window keeps the front
        Get().Uninstall();              // one-shot: nothing left to watch
    }
}

void WindowWatch::Install() {
    if (installed_) return;
    installed_ = true;
    // WINEVENT_INCONTEXT requires the module that holds the callback; we are that module (the
    // steam_api64.dll proxy), already mapped in the only process we filter on (our own PID).
    const HMODULE self = ::GetModuleHandleW(L"steam_api64.dll");
    const DWORD   pid  = ::GetCurrentProcessId();
    create_ = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
                                self, &WindowWatch::OnWinEvent, pid, 0, WINEVENT_INCONTEXT);
    fg_     = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                self, &WindowWatch::OnWinEvent, pid, 0, WINEVENT_INCONTEXT);
    Log::Get().Line((create_ && fg_)
        ? "[winwatch] WinEvent hooks armed (window-create + foreground, in-context, own pid)"
        : "[winwatch] SetWinEventHook FAILED");
}

void WindowWatch::Uninstall() {
    if (create_) { ::UnhookWinEvent(create_); create_ = nullptr; }
    if (fg_)     { ::UnhookWinEvent(fg_);     fg_ = nullptr; }
}

}  // namespace sow
