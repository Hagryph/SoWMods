#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "EarlyRingUnlock.h"
#include "RingGrantGuard.h"
#include "RingUnlockRecords.h"
#include "GameOffsets.h"
#include <cstring>
#include <cstdio>
#include "ModRuntime.h"

namespace inventory_editor {
namespace {
bool guardReady = false;

struct Snapshot {
    std::uint64_t session = 0;
    std::uintptr_t player = 0;
    std::uintptr_t inventory = 0;
    std::uintptr_t grants[2]{};
    std::uintptr_t unlocks[2]{};
    bool unlocked[2]{};
    bool valid = false;
};

bool ReadState(Snapshot& state) {
    using HasFn = bool(__fastcall*)(std::uintptr_t, std::uintptr_t);
    __try {
        const auto engine = *reinterpret_cast<std::uintptr_t*>(game::FromRVA(game::kEngineSingleton));
        if (!engine) return false;
        state.player = *reinterpret_cast<std::uintptr_t*>(engine + 0x888);
        if (!state.player) return false;
        state.inventory = *reinterpret_cast<std::uintptr_t*>(state.player + 0x2348);
        const auto descriptor = *reinterpret_cast<std::uintptr_t*>(game::FromRVA(game::kInventoryItemDescriptor));
        if (!state.inventory || !descriptor) return false;
        const auto count = *reinterpret_cast<std::uint32_t*>(descriptor + 0x28);
        const auto table = *reinterpret_cast<std::uintptr_t*>(descriptor + 0x38);
        if (!table || count > 100000) return false;
        std::uintptr_t starterRing = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto row = table + i * 0x28;
            const auto name = *reinterpret_cast<const char**>(row + 0x20);
            if (!name) continue;
            for (int g = 0; g < 2; ++g) {
                if (std::strcmp(name, game::ring::kGrants[g].item) == 0) state.grants[g] = row;
                if (std::strcmp(name, game::ring::kGrants[g].unlocked) == 0) state.unlocks[g] = row;
            }
            if (std::strcmp(name, "Primary_6Ring_1Default") == 0) starterRing = row;
        }
        const auto has = reinterpret_cast<HasFn>(game::FromRVA(game::kInventoryHasItem));
        if (!starterRing || !state.grants[0] || !state.grants[1] || !state.unlocks[0] || !state.unlocks[1]) return false;
        for (int i = 0; i < 2; ++i) state.unlocked[i] = has(state.inventory, state.unlocks[i]);
        // Do not offer Talion's progression grants in the Baranor/Eltariel campaigns.
        state.valid = has(state.inventory, starterRing);
        return state.valid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Give(std::uintptr_t player, std::uintptr_t row) {
    using GiveFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
    __try {
        reinterpret_cast<GiveFn>(game::FromRVA(game::kCharacterAddInventoryItem))(player, row);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}

bool RegisteredRingAction(bool unlock) {
    if(!guardReady)return false;
    Snapshot state{};state.session=sowmod::SaveSession();
    if(!state.session || !ReadState(state))return false;
    if(unlock && !game::ring::Complete(state))
        return game::ring::GrantMissing(state,[&]{return state.session==sowmod::SaveSession();},&Give,&ReadState);
    return game::ring::Complete(state);
}
bool InitEarlyRingUnlock() { return guardReady = InstallRingGrantGuard(); }
bool CanUnlockRing(){
    Snapshot state{};state.session=sowmod::SaveSession();
    return guardReady && state.session && ReadState(state) && !game::ring::Complete(state);
}
}  // namespace inventory_editor
