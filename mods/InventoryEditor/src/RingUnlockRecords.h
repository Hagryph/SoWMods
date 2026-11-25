#pragma once

namespace game::ring {
struct Grant {
    const char* item;
    const char* unlocked;
};
inline constexpr Grant kGrants[] = {
    { "ScriptedUnlock_SkillGrant_BrandGrunts", "PS_Brand_UNLOCK" },
    { "ScriptedUnlock_SkillGrant_DominateCaptains", "PS_DominateCaptains_UNLOCK" }
};

// Only a positive, repeated one-shot grant may be replaced with success.
inline bool SkipDuplicate(int grantIndex, int quantity, bool unlocked) {
    return grantIndex >= 0 && grantIndex < 2 && quantity > 0 && unlocked;
}

template <class State>
bool Complete(const State& state) {
    return state.valid && state.unlocked[0] && state.unlocked[1];
}

template <class State, class Session, class Give, class Read>
bool GrantMissing(State& state, Session sameSession, Give give, Read read) {
    if (!state.valid || !sameSession()) return false;
    for (int i = 0; i < 2; ++i) {
        if (state.unlocked[i]) continue;
        const auto player = state.player;
        const auto inventory = state.inventory;
        if (!sameSession() || !give(player, state.grants[i])) return false;
        State after{};
        after.session = state.session;
        if (!read(after) || after.player != player || after.inventory != inventory) return false;
        state = after;
        if (!state.unlocked[i]) return false;
    }
    return sameSession() && Complete(state);
}
}  // namespace game::ring
