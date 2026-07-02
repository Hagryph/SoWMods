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

    // Game-state signal for scope-gated mods (see shared/SoWModAPI.h). Event-latched, NOT polled: the
    // front-end root layer's ctor (enter menu) and dtor (menu torn down for gameplay) drive it, so it
    // is stable regardless of render cadence. True once a save is loaded and gameplay is running.
    static bool InSave();

    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

private:
    GameHooks() = default;
};

}  // namespace sow
