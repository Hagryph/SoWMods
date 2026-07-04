#include "GameHooks.h"
#include "Log.h"
#include "Overlay.h"
#include "GameOffsets.h"   // ../shared : game::FromRVA + the hand-found RVAs
#include "GameTaskQueue.h"

#include <MinHook.h>
#include <string>

namespace sow {

// ---- game main-window creation (RE'd 2026-07-02, kept in shared/GameOffsets.h) ----
// kWinMainThread (FUN_140c24db0) registers + creates THE game window (class L"Shadow of War")
// through the engine's own resolved-API table (NOT the IAT), and kPostWindowInit (0x1411798ac)
// is the first engine call after CreateWindowExW + ShowWindow. Trampolining kPostWindowInit fired
// correctly but proved TOO EARLY for the console: at that instant the window is an unactivated
// 336x239 stub, so the console still won the foreground and became the "main" window. The console
// trigger now lives in WindowWatch (SetWinEventHook in-context): the console opens when the game
// window BECOMES FOREGROUND - the only moment that guarantees it drops behind the game.

GameHooks& GameHooks::Get() { static GameHooks instance; return instance; }

// Claim-once so the overlay is installed exactly one time no matter which path gets there first.
static volatile LONG s_overlayClaimed = 0;
static bool          s_ctorLogged     = false;
static bool          s_worldLogged    = false;

// Event-driven in-save signal. Front-end ctor clears it for the initial menu; OnWorldLoad sets it
// once a save/world is registered. The old save-to-front-end clear hook is only a candidate signal:
// death/respawn can hit it too, so the actual save->menu clear waits for the front-end menu refresh.
// There is no hardware breakpoint, no ticker, and no per-frame read.
static volatile LONG g_inSave = 0;     // 1 = save/world loaded, 0 = menu/not in save
static volatile LONG g_pendingMenuClear = 0;
static volatile LONG g_nonRootFrontEndLogged = 0;

template <class T>
static bool ReadGameMem(std::uintptr_t addr, T& out) {
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::uintptr_t FindFrontEndItem(std::uintptr_t collection, const char* name) {
    using FindFn = std::uintptr_t(__fastcall*)(std::uintptr_t, const char*);
    __try {
        return reinterpret_cast<FindFn>(game::FromRVA(game::kFrontEndFindItem))(collection, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool IsRootFrontEndMenu(void* layer) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(layer);
    std::uintptr_t collection = 0;
    std::uint32_t count = 0;
    if (!base || !ReadGameMem(base + 0x53f8, collection) || !ReadGameMem(base + 0x5400, count)) {
        return false;
    }
    if (!collection || count == 0) return false;
    return FindFrontEndItem(collection, "FrontEnd_Start") != 0 &&
           FindFrontEndItem(collection, "FrontEnd_Quit") != 0;
}

static void EnsureOverlay(const char* who) {
    if (::InterlockedCompareExchange(&s_overlayClaimed, 1, 0) == 0) {
        Log::Get().Line(std::string("[init] installing overlay (trigger: ") + who + ")");
        Overlay::Get().Install();
    }
}

// CUIFrontEndRootLayer::CUIFrontEndRootLayer(this, ...): constructs the front-end menu root UI layer.
// This is our start-menu trigger: install the overlay and mark menu/not-in-save.
using RootLayerCtorFn = void* (__fastcall*)(void*, void*, void*);
static RootLayerCtorFn oRootLayerCtor = nullptr;

static void* __fastcall HookRootLayerCtor(void* self, void* a2, void* a3) {
    if (!s_ctorLogged) {
        s_ctorLogged = true;
        Log::Get().Line("[frontend] CUIFrontEndRootLayer ctor hit - START MENU UI created");
    }
    void* r = oRootLayerCtor(self, a2, a3);     // forward first so the object is fully initialized
    ::InterlockedExchange(&g_inSave, 0);
    Overlay::Get().SetInGame(false);
    EnsureOverlay("frontend-rootlayer-ctor");
    return r;
}

// OnWorldLoad(worldManager, worldInfo, faction): fires once during save load after leaving the
// front-end flow. Live-verified to stay quiet in title/main menu/save selection.
using OnWorldLoadFn = void (__fastcall*)(void*, void*, void*);
static OnWorldLoadFn oOnWorldLoad = nullptr;

static void __fastcall HookOnWorldLoad(void* self, void* worldInfo, void* faction) {
    oOnWorldLoad(self, worldInfo, faction);
    ::InterlockedExchange(&g_inSave, 1);
    ::InterlockedExchange(&g_pendingMenuClear, 0);
    ::InterlockedExchange(&g_nonRootFrontEndLogged, 0);
    Overlay::Get().SetInGame(true);
    if (!s_worldLogged) {
        s_worldLogged = true;
        Log::Get().Line("[state] OnWorldLoad hit - save/world loaded");
    }
}

using FrontEndItemRefreshFn = std::uint64_t (__fastcall*)(void*);
static FrontEndItemRefreshFn oFrontEndItemRefresh = nullptr;

static std::uint64_t __fastcall HookFrontEndItemRefresh(void* self) {
    const std::uint64_t r = oFrontEndItemRefresh(self);
    if (IsRootFrontEndMenu(self)) {
        if (::InterlockedExchange(&g_inSave, 0) != 0 ||
            ::InterlockedExchange(&g_pendingMenuClear, 0) != 0) {
            Log::Get().Line("[state] root front-end refresh hit - menu/not-in-save");
        }
        Overlay::Get().SetInGame(false);
    } else if (g_pendingMenuClear != 0 &&
               ::InterlockedCompareExchange(&g_nonRootFrontEndLogged, 1, 0) == 0) {
        Log::Get().Line("[state] non-root front-end refresh hit - keeping in-save");
    }
    return r;
}

// Character/faction relationship clear. This was the old reverse latch, but it also fires during
// death/respawn, so it only records that a menu clear may be coming. The front-end refresh hook above
// is the confirmation that the game is actually back in the menu.
using SaveToFrontEndClearFn = void (__fastcall*)(void*);
static SaveToFrontEndClearFn oSaveToFrontEndClear = nullptr;

static void __fastcall HookSaveToFrontEndClear(void* self) {
    oSaveToFrontEndClear(self);
    if (g_inSave != 0) ::InterlockedExchange(&g_pendingMenuClear, 1);
}

using CursorRecenterFn = std::uint64_t (__fastcall*)();
static CursorRecenterFn oCursorRecenter = nullptr;

static std::uint64_t __fastcall HookCursorRecenter() {
    const std::uint64_t r = oCursorRecenter();
    if (g_inSave != 0) DrainGameTasks(1);
    return r;
}

void GameHooks::Install() {
    auto& log = Log::Get();

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        log.Line("[gamehooks] MH_Initialize failed");
        return;
    }

    void* menuTarget = reinterpret_cast<void*>(game::FromRVA(game::kCUIFrontEndRootLayerCtor));
    char menuAddr[32]{};
    ::wsprintfA(menuAddr, "0x%p", menuTarget);
    if (MH_CreateHook(menuTarget, reinterpret_cast<void*>(&HookRootLayerCtor),
                      reinterpret_cast<void**>(&oRootLayerCtor)) == MH_OK &&
        MH_EnableHook(menuTarget) == MH_OK) {
        log.Line(std::string("[gamehooks] front-end ctor hooked @ ") + menuAddr + " - awaiting start menu");
    } else {
        log.Line(std::string("[gamehooks] front-end ctor hook FAILED @ ") + menuAddr);
    }

    void* worldTarget = reinterpret_cast<void*>(game::FromRVA(game::kOnWorldLoad));
    char worldAddr[32]{};
    ::wsprintfA(worldAddr, "0x%p", worldTarget);
    if (MH_CreateHook(worldTarget, reinterpret_cast<void*>(&HookOnWorldLoad),
                      reinterpret_cast<void**>(&oOnWorldLoad)) == MH_OK &&
        MH_EnableHook(worldTarget) == MH_OK) {
        log.Line(std::string("[gamehooks] OnWorldLoad hooked @ ") + worldAddr + " - awaiting save/world load");
    } else {
        log.Line(std::string("[gamehooks] OnWorldLoad hook FAILED @ ") + worldAddr);
    }

    void* returnTarget = reinterpret_cast<void*>(game::FromRVA(game::kSaveToFrontEndClear));
    char returnAddr[32]{};
    ::wsprintfA(returnAddr, "0x%p", returnTarget);
    if (MH_CreateHook(returnTarget, reinterpret_cast<void*>(&HookSaveToFrontEndClear),
                      reinterpret_cast<void**>(&oSaveToFrontEndClear)) == MH_OK &&
        MH_EnableHook(returnTarget) == MH_OK) {
        log.Line(std::string("[gamehooks] save-to-front-end clear hooked @ ") + returnAddr +
                 " - menu-clear candidate only");
    } else {
        log.Line(std::string("[gamehooks] save-to-front-end clear hook FAILED @ ") + returnAddr);
    }

    void* frontEndRefreshTarget = reinterpret_cast<void*>(game::FromRVA(game::kFrontEndItemRefresh));
    char frontEndRefreshAddr[32]{};
    ::wsprintfA(frontEndRefreshAddr, "0x%p", frontEndRefreshTarget);
    if (MH_CreateHook(frontEndRefreshTarget, reinterpret_cast<void*>(&HookFrontEndItemRefresh),
                      reinterpret_cast<void**>(&oFrontEndItemRefresh)) == MH_OK &&
        MH_EnableHook(frontEndRefreshTarget) == MH_OK) {
        log.Line(std::string("[gamehooks] front-end item refresh hooked @ ") +
                 frontEndRefreshAddr + " - confirms menu state");
    } else {
        log.Line(std::string("[gamehooks] front-end item refresh hook FAILED @ ") + frontEndRefreshAddr);
    }

    void* cursorTarget = reinterpret_cast<void*>(game::FromRVA(game::kCursorRecenter));
    char cursorAddr[32]{};
    ::wsprintfA(cursorAddr, "0x%p", cursorTarget);
    if (MH_CreateHook(cursorTarget, reinterpret_cast<void*>(&HookCursorRecenter),
                      reinterpret_cast<void**>(&oCursorRecenter)) == MH_OK &&
        MH_EnableHook(cursorTarget) == MH_OK) {
        log.Line(std::string("[gamehooks] cursor recenter hooked @ ") + cursorAddr +
                 " - queued game tasks drain here");
    } else {
        log.Line(std::string("[gamehooks] cursor recenter hook FAILED @ ") + cursorAddr);
    }
}

}  // namespace sow
