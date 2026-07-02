#include "WindowWatch.h"
#include "Log.h"
#include "Loader.h"

#include <MinHook.h>

namespace sow {

WindowWatch& WindowWatch::Get() { static WindowWatch w; return w; }

using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
static ShowWindowFn oShowWindow    = nullptr;
static bool         g_consoleFired = false;

// The game's main window: top-level, class L"Shadow of War" (registered by the engine before it
// creates the window). Checked by class at ShowWindow time (the window exists, so GetClassName works).
static bool IsGameMainWindow(HWND h) {
    if (!h || ::GetAncestor(h, GA_ROOT) != h) return false;   // top-level only
    wchar_t cls[64]{};
    if (!::GetClassNameW(h, cls, 64)) return false;
    return ::wcscmp(cls, L"Shadow of War") == 0;
}

// user32!ShowWindow detour: show the window, and the FIRST time the game's main window is made
// visible (nCmdShow != SW_HIDE) — i.e. its taskbar button is being created — open the console, then
// call original. The game calls SetForegroundWindow on itself right after, so it stays the main
// window and the console drops to a secondary taskbar entry.
static BOOL WINAPI HkShowWindow(HWND h, int cmd) {
    if (!g_consoleFired && cmd != SW_HIDE && IsGameMainWindow(h)) {
        g_consoleFired = true;
        char l[112]; ::wsprintfA(l, "[winwatch] game window %p shown (taskbar-registered) -> opening console", (void*)h);
        Log::Get().Line(l);
        Loader::Get().OnRenderLive();   // opens the console at the taskbar-registration moment
    }
    return oShowWindow(h, cmd);
}

void WindowWatch::Install() {
    if (installed_) return;
    installed_ = true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { Log::Get().Line("[winwatch] MH_Initialize failed"); return; }
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    void* target = u32 ? (void*)::GetProcAddress(u32, "ShowWindow") : nullptr;
    if (target && MH_CreateHook(target, (void*)&HkShowWindow, (void**)&oShowWindow) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        Log::Get().Line("[winwatch] ShowWindow hooked (game-window taskbar registration -> console trigger)");
    } else {
        Log::Get().Line("[winwatch] hook ShowWindow FAILED");
    }
}

}  // namespace sow
