#include "RingUnlockRecords.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct State {
    int session = 1, player = 2, inventory = 3;
    int grants[2] = {10, 11};
    bool unlocked[2]{};
    bool valid = true;
};
struct FakeGame {
    State saved;
    int calls[2]{};
    bool active = true, failGive = false, failRead = false, changePlayer = false;
    int expireAfter = 0;
    bool Run(State& state) {
        return game::ring::GrantMissing(state, [&] { return active; },
            [&](int player, int row) {
                Check(player == 2, "Wrong player passed to native grant");
                const int i = row - 10;
                Check(i >= 0 && i < 2, "Unrelated item was granted");
                ++calls[i];
                if (failGive) return false;
                saved.unlocked[i] = true;
                if (changePlayer) saved.player = 99;
                if (expireAfter && calls[0] + calls[1] >= expireAfter) active = false;
                return true;
            }, [&](State& after) { after = saved; return !failRead; });
    }
};
}

int main() {
    FakeGame game;
    State state;
    Check(game.Run(state), "Initial unlock failed");
    Check(game.calls[0] == 1 && game.calls[1] == 1, "Expected exactly one grant per ability");
    Check(game::ring::Complete(state), "Successful state should hide the button");
    Check(game.Run(state) && game.calls[0] == 1 && game.calls[1] == 1, "Repeated press granted duplicates");

    FakeGame natural;
    natural.saved.unlocked[0] = natural.saved.unlocked[1] = true;
    state = natural.saved;
    Check(natural.Run(state) && natural.calls[0] == 0 && natural.calls[1] == 0, "Natural unlock should not grant again");

    FakeGame partial;
    partial.saved.unlocked[0] = true;
    state = partial.saved;
    Check(partial.Run(state) && partial.calls[0] == 0 && partial.calls[1] == 1, "Partial unlock should grant only the missing ability");

    FakeGame switched;
    switched.active = false;
    state = {};
    Check(!switched.Run(state) && switched.calls[0] == 0, "Stale save task was not rejected");
    switched.active = true;
    switched.expireAfter = 1;
    Check(!switched.Run(state) && switched.calls[1] == 0, "World transition should stop further grants");

    FakeGame failed;
    failed.failGive = true;
    state = {};
    Check(!failed.Run(state) && !game::ring::Complete(state), "Failed native call must not mark unlocked");
    failed.failGive = false;
    failed.failRead = true;
    Check(!failed.Run(state), "Unverified native grant must fail closed");

    FakeGame replaced;
    replaced.changePlayer = true;
    state = {};
    Check(!replaced.Run(state) && replaced.calls[1] == 0, "Player replacement should stop further grants");

    for (int i = -1; i <= 2; ++i) {
        Check(game::ring::SkipDuplicate(i, 1, true) == (i == 0 || i == 1), "Guard affected unrelated grant");
        Check(!game::ring::SkipDuplicate(i, 1, false), "Guard suppressed first grant");
        Check(!game::ring::SkipDuplicate(i, 0, true), "Guard suppressed zero-quantity call");
        Check(!game::ring::SkipDuplicate(i, -1, true), "Guard suppressed removal");
    }
    std::puts("Ring unlock policy tests passed");
}
