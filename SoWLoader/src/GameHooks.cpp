#include "GameHooks.h"
#include "Log.h"
#include "Overlay.h"
#include "GameOffsets.h"   // ../shared : game::FromRVA + the hand-found RVAs

#include <MinHook.h>
#include <string>

namespace sow {

GameHooks& GameHooks::Get() { static GameHooks instance; return instance; }

// Claim-once so the overlay is installed exactly one time no matter which path gets there first
// (the front-end trigger on the game thread, or the worker-thread fallback).
static volatile LONG s_overlayClaimed = 0;
static bool          s_ctorLogged     = false;

static void EnsureOverlay(const char* who) {
    if (::InterlockedCompareExchange(&s_overlayClaimed, 1, 0) == 0) {
        Log::Get().Line(std::string("[init] installing overlay (trigger: ") + who + ")");
        Overlay::Get().Install();
    }
}

// CUIFrontEndRootLayer::CUIFrontEndRootLayer(this, ...): constructs the front-end menu root UI
// layer and returns `this`. The factory passes up to 3 register args; we forward all of them.
using RootLayerCtorFn = void* (__fastcall*)(void*, void*, void*);
static RootLayerCtorFn oRootLayerCtor = nullptr;

static void* __fastcall HookRootLayerCtor(void* self, void* a2, void* a3) {
    if (!s_ctorLogged) {
        s_ctorLogged = true;
        Log::Get().Line("[frontend] CUIFrontEndRootLayer ctor hit — START MENU UI created");
    }
    EnsureOverlay("frontend-rootlayer-ctor");
    return oRootLayerCtor(self, a2, a3);   // forward to original (trampoline; never destructive)
}

void GameHooks::Install() {
    auto& log = Log::Get();

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { log.Line("[gamehooks] MH_Initialize failed"); return; }

    void* target = reinterpret_cast<void*>(game::FromRVA(game::kCUIFrontEndRootLayerCtor));
    char addr[32]{}; ::wsprintfA(addr, "0x%p", target);

    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookRootLayerCtor),
                      reinterpret_cast<void**>(&oRootLayerCtor)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        log.Line(std::string("[gamehooks] CUIFrontEndRootLayer ctor hooked @ ") + addr + " — awaiting start menu");
    } else {
        log.Line(std::string("[gamehooks] MinHook create/enable CUIFrontEndRootLayer ctor FAILED @ ") + addr);
    }
}

void GameHooks::InstallOverlayFallback() {
    EnsureOverlay("worker-fallback");
}

}  // namespace sow
