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
    // The overlay installs from that ctor only (real menu-load) — no timer, no fallback.
    void Install();

    // Game-state signals for scope-gated mods (see shared/SoWModAPI.h).
    //  MenuHeartbeat() ticks every time the front-end menu updates; it FREEZES once a save is loaded
    //    and gameplay takes over, so a stalled heartbeat == in-game. MenuEverShown() is true once the
    //    front-end has appeared at least once (so we don't read "in-game" during the boot splash).
    static unsigned long long MenuHeartbeat();
    static bool               MenuEverShown();

    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

private:
    GameHooks() = default;
};

}  // namespace sow
