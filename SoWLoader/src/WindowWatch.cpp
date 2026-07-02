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

// user32!ShowWindow detour. The FIRST time the game's main window is made visible (nCmdShow !=
// SW_HIDE) — its taskbar button is being created — we:
//   1. call the ORIGINAL first, so the game window is actually shown + gets its taskbar button
//      (opening the console before this left the console as the only visible window => "main window");
//   2. open the console;
//   3. force the console BEHIND the game window in z-order and hand foreground back to the game,
//      so the game stays the main/foreground window and the console is a secondary taskbar entry.
static BOOL WINAPI HkShowWindow(HWND h, int cmd) {
    const bool trigger = !g_consoleFired && cmd != SW_HIDE && IsGameMainWindow(h);
    BOOL r = oShowWindow(h, cmd);            // (1) show the game window FIRST
    if (trigger) {
        g_consoleFired = true;
        char l[112]; ::wsprintfA(l, "[winwatch] game window %p shown (taskbar-registered) -> opening console", (void*)h);
        Log::Get().Line(l);
        Loader::Get().OnRenderLive();        // (2) open the console
        if (HWND cw = ::GetConsoleWindow())  // (3) console just below the game window, don't activate
            ::SetWindowPos(cw, h, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::SetForegroundWindow(h);            // game stays the foreground/main window
    }
    return r;
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
