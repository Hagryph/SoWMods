#include "GameHooks.h"
#include "Log.h"
#include "Overlay.h"
#include "Loader.h"        // Loader::OnGameWindow -> open console
#include "GameOffsets.h"   // ../shared : game::FromRVA + the hand-found RVAs

#include <MinHook.h>
#include <tlhelp32.h>   // thread enumeration for arming the hardware breakpoint on every thread
#include <string>
#include <cstdint>

namespace sow {

// ---- game main-window creation (RE'd 2026-07-02, kept in shared/GameOffsets.h) ----
// kWinMainThread (FUN_140c24db0) registers + creates THE game window (class L"Shadow of War")
// through the engine's own resolved-API table (NOT the IAT), and kPostWindowInit (0x1411798ac)
// is the first engine call after CreateWindowExW + ShowWindow. Trampolining kPostWindowInit fired
// correctly but proved TOO EARLY for the console: at that instant the window is an unactivated
// 336x239 stub, so the console still won the foreground and became the "main" window. The console
// trigger now lives in WindowWatch (SetWinEventHook in-context): the console opens when the game
// window BECOMES FOREGROUND — the only moment that guarantees it drops behind the game.

GameHooks& GameHooks::Get() { static GameHooks instance; return instance; }

// Claim-once so the overlay is installed exactly one time no matter which path gets there first
// (the front-end trigger on the game thread, or the worker-thread fallback).
static volatile LONG s_overlayClaimed = 0;
static bool          s_ctorLogged     = false;

// EVENT-DRIVEN in-save signal. A HagIPC hardware write breakpoint pinned the state variable: front-end
// root layer + 0x2c is NON-ZERO while the menu is shown and 0 once a save is loaded (SoW builds the menu
// once and toggles visibility — there is no dtor/lifecycle event). We arm a hardware WRITE breakpoint
// (debug register DR0) on that member, so the CPU traps the transition and our VEH refreshes g_inSave —
// no polling, no per-frame work. InSave() is then a trivial bool read. See reference_sow-gamestate-signal.
static void* volatile  g_frontEnd     = nullptr;   // captured front-end root layer instance
static constexpr unsigned kActiveOff  = 0x2c;      // != 0 at the menu, 0 in a save
static volatile LONG   g_inSave       = 0;         // 1 = in a save, 0 = at the menu
static std::uintptr_t  g_watchAddr    = 0;         // = g_frontEnd + kActiveOff
static PVOID           g_stateVeh     = nullptr;
static bool            g_watchArmed   = false;

// SEH-only helper (no C++ objects, so __try is legal here): read an int, fault -> fallback.
static int SafeReadI32(std::uintptr_t addr, int fallback) {
    __try { return *reinterpret_cast<volatile int*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

static LONG CALLBACK StateVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == (DWORD)EXCEPTION_SINGLE_STEP &&
        (ep->ContextRecord->Dr6 & 0x1ull) && g_watchAddr) {
        const int v = SafeReadI32(g_watchAddr, 0);                         // read the new state value
        ::InterlockedExchange(&g_inSave, v == 0 ? 1 : 0);
        ep->ContextRecord->Dr6 &= ~0xFull;                                 // clear status, stay armed
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Arm DR0 (write, 4 bytes) on every thread of this process except the caller. Runs once, from the
// front-end ctor (game thread) — the thread that writes the flag already exists by then.
static void ArmStateWatch(std::uintptr_t addr) {
    if (g_watchArmed) return;
    g_watchAddr = addr;
    ::InterlockedExchange(&g_inSave, SafeReadI32(addr, 0) == 0 ? 1 : 0);   // seed from current value
    if (!g_stateVeh) g_stateVeh = ::AddVectoredExceptionHandler(1, &StateVeh);
    if (!g_stateVeh) return;

    const DWORD pid = ::GetCurrentProcessId(), me = ::GetCurrentThreadId();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int armed = 0;
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                    FALSE, te.th32ThreadID);
            if (!h) continue;
            ::SuspendThread(h);
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (::GetThreadContext(h, &c)) {
                c.Dr0 = addr;
                c.Dr7 &= ~((DWORD64)0xF << 16);      // clear RW0/LEN0
                c.Dr7 |= 0x1;                        // L0 enable
                c.Dr7 |= ((DWORD64)0x1 << 16);       // RW0 = 01 (write)
                c.Dr7 |= ((DWORD64)0x3 << 18);       // LEN0 = 11 (4 bytes)
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (::SetThreadContext(h, &c)) ++armed;
            }
            ::ResumeThread(h);
            ::CloseHandle(h);
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    g_watchArmed = true;
    char l[128]; ::wsprintfA(l, "[state] in-save write bp armed @0x%p on %d thread(s) (event-driven)",
                             (void*)addr, armed);
    Log::Get().Line(l);
}

static void EnsureOverlay(const char* who) {
    if (::InterlockedCompareExchange(&s_overlayClaimed, 1, 0) == 0) {
        Log::Get().Line(std::string("[init] installing overlay (trigger: ") + who + ")");
        Overlay::Get().Install();
    }
}

// CUIFrontEndRootLayer::CUIFrontEndRootLayer(this, ...): constructs the front-end menu root UI layer.
// This is our start-menu trigger: install the overlay + arm the event-driven in-save breakpoint.
using RootLayerCtorFn = void* (__fastcall*)(void*, void*, void*);
static RootLayerCtorFn oRootLayerCtor = nullptr;

static void* __fastcall HookRootLayerCtor(void* self, void* a2, void* a3) {
    if (!s_ctorLogged) {
        s_ctorLogged = true;
        Log::Get().Line("[frontend] CUIFrontEndRootLayer ctor hit — START MENU UI created");
    }
    void* r = oRootLayerCtor(self, a2, a3);            // forward FIRST so the member is fully initialised
    g_frontEnd = self;                                 // capture the instance
    ArmStateWatch(reinterpret_cast<std::uintptr_t>(self) + kActiveOff);  // event-driven in-save signal
    EnsureOverlay("frontend-rootlayer-ctor");
    return r;
}

// Event-driven: g_inSave is refreshed by the DR0 write-breakpoint handler the instant front-end+0x2c
// changes, so this is a trivial bool read — no per-frame memory access, no polling.
bool GameHooks::InSave() { return g_inSave != 0; }

void GameHooks::Install() {
    auto& log = Log::Get();

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { log.Line("[gamehooks] MH_Initialize failed"); return; }

    // The one game hook we keep: the front-end root layer ctor (start-menu trigger; installs the overlay
    // and arms the in-save write breakpoint). Everything else was RE scaffolding and has been removed.
    void* target = reinterpret_cast<void*>(game::FromRVA(game::kCUIFrontEndRootLayerCtor));
    char addr[32]{}; ::wsprintfA(addr, "0x%p", target);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookRootLayerCtor),
                      reinterpret_cast<void**>(&oRootLayerCtor)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        log.Line(std::string("[gamehooks] front-end ctor hooked @ ") + addr + " — awaiting start menu");
    } else {
        log.Line(std::string("[gamehooks] front-end ctor hook FAILED @ ") + addr);
    }
}

}  // namespace sow
