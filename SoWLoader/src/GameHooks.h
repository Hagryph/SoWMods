#pragma once
#include "PCH.h"

namespace sow {

// Game-logic hooks (as opposed to the D3D/Overlay renderer hook). The front-end root-layer ctor is
// the menu/overlay setup trigger; OnWorldLoad sets the in-save latch; save-to-front-end teardown
// clears it when returning to the main menu.
class GameHooks {
public:
    static GameHooks& Get();

    // Install the menu and world-load hooks. Call from the worker thread.
    void Install();

    // Game-state signal for scope-gated mods (see shared/SoWModAPI.h). Event-latched, NOT polled:
    // front-end ctor and save-to-menu teardown clear it, OnWorldLoad sets it.
    static bool InSave();

    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

private:
    GameHooks() = default;
};

}  // namespace sow
