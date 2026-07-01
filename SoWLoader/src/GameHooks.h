#pragma once
#include "PCH.h"

namespace sow {

// Game-logic hooks (as opposed to the D3D/Overlay renderer hook). The first is the "start menu
// loaded" trigger: a MinHook trampoline on the LithTech ClientShell FrontEndLoadWorld function,
// whose run-once detour becomes our deterministic initialization point (install the overlay,
// and later any gameplay hooks) instead of a fragile fixed sleep.
class GameHooks {
public:
    static GameHooks& Get();

    // Install the start-menu trigger hook (CUIFrontEndRootLayer ctor). Call from the worker thread.
    void Install();

    // Install the overlay if no trigger has fired yet (worker-thread safety fallback). Idempotent.
    void InstallOverlayFallback();

    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

private:
    GameHooks() = default;
};

}  // namespace sow
