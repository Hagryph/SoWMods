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

    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

private:
    GameHooks() = default;
};

}  // namespace sow
