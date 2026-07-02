#include "WindowWatch.h"
#include "Log.h"
#include "Loader.h"

#include <MinHook.h>

namespace sow {

WindowWatch& WindowWatch::Get() { static WindowWatch w; return w; }

using CreateWinExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                     HWND, HMENU, HINSTANCE, LPVOID);
static CreateWinExWFn oCreateWinExW = nullptr;
static bool           g_consoleFired = false;

static bool IsStrW(LPCWSTR s) { return s && reinterpret_cast<uintptr_t>(s) > 0xFFFF; }

// user32!CreateWindowExW detour: create the window, and the instant the GAME's main window
// (top-level, class L"Shadow of War") comes into existence, open the console — then call original.
static HWND WINAPI HkCreateWinExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y,
                                  int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID p) {
    HWND hwnd = oCreateWinExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, p);
    if (!g_consoleFired && parent == nullptr && IsStrW(cls) && ::wcscmp(cls, L"Shadow of War") == 0) {
        g_consoleFired = true;
        char l[96]; ::wsprintfA(l, "[winwatch] game window %p created (CreateWindowExW) -> opening console", (void*)hwnd);
        Log::Get().Line(l);
        Loader::Get().OnRenderLive();   // opens the console right at window creation
    }
    return hwnd;
}

void WindowWatch::Install() {
    if (installed_) return;
    installed_ = true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { Log::Get().Line("[winwatch] MH_Initialize failed"); return; }
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    void* target = u32 ? (void*)::GetProcAddress(u32, "CreateWindowExW") : nullptr;
    if (target && MH_CreateHook(target, (void*)&HkCreateWinExW, (void**)&oCreateWinExW) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        Log::Get().Line("[winwatch] CreateWindowExW hooked (game-window creation -> console trigger)");
    } else {
        Log::Get().Line("[winwatch] hook CreateWindowExW FAILED");
    }
}

}  // namespace sow
