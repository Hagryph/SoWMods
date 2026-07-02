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
    if (event != EVENT_OBJECT_CREATE || idObject != OBJID_WINDOW || !IsGameMainWindow(hwnd)) return;
    if (::InterlockedExchange(&Get().fired_, 1) != 0) return;   // one-shot
    char l[112];
    ::wsprintfA(l, "[winwatch] game window %p CREATED (window now exists) -> opening console", (void*)hwnd);
    Log::Get().Line(l);
    Loader::Get().OnRenderLive();   // opens the console non-activating (never the foreground window)
    Get().Stop();                    // nothing left to watch
}

DWORD WINAPI WindowWatch::ThreadThunk(LPVOID self) {
    static_cast<WindowWatch*>(self)->ThreadMain();
    return 0;
}

void WindowWatch::ThreadMain() {
    threadId_ = ::GetCurrentThreadId();
    // OUTOFCONTEXT: callbacks are posted to THIS thread's message queue, so the hook stays valid for
    // this thread's whole life. Filter to our own PID (dwEventThread 0 = all threads in the process).
    hook_ = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
                              nullptr, &WindowWatch::OnWinEvent,
                              ::GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    Log::Get().Line(hook_ ? "[winwatch] hook armed on dedicated thread (EVENT_OBJECT_CREATE, own pid)"
                          : "[winwatch] SetWinEventHook FAILED");
    if (!hook_) return;
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {   // pump until WM_QUIT (posted by Stop)
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

void WindowWatch::Install() {
    if (installed_) return;
    installed_ = true;
    thread_ = ::CreateThread(nullptr, 0, &WindowWatch::ThreadThunk, this, 0, &threadId_);
}

void WindowWatch::Stop() {
    if (hook_) { ::UnhookWinEvent(hook_); hook_ = nullptr; }
    if (threadId_) ::PostThreadMessageW(threadId_, WM_QUIT, 0, 0);   // ends the pump -> thread exits
}

}  // namespace sow
