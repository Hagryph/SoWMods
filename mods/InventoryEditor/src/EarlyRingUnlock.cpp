#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "EarlyRingUnlock.h"
#include "RingGrantGuard.h"
#include "RingUnlockRecords.h"
#include "GameOffsets.h"
#include <cstring>
#include <cstdio>
#include <new>

namespace inventory_editor {
namespace {
HagUIAPI* g_ui = nullptr;
void (*g_log)(const char*) = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;

struct Snapshot {
    std::uint64_t session = 0;
    std::uintptr_t player = 0;
    std::uintptr_t inventory = 0;
    std::uintptr_t grants[2]{};
    std::uintptr_t unlocks[2]{};
    bool unlocked[2]{};
    bool valid = false;
};
Snapshot g_snapshot;
bool g_probePending = false;
bool g_unlockPending = false;
ULONGLONG g_lastProbe = 0;
struct Request { std::uint64_t session; bool unlock; };

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

void Execute(void* context) {
    const Request request = *static_cast<Request*>(context);
    delete static_cast<Request*>(context);
    Snapshot state{};
    state.session = request.session;
    bool ok = request.session != 0 && g_ui->SaveSession() == request.session && ReadState(state);
    if (ok && request.unlock) {
        ok = game::ring::GrantMissing(state,
            [&] { return g_ui->SaveSession() == request.session; }, &Give, &ReadState);
        if (g_log) g_log(ok
            ? "Early ring: native ability grants completed; button hidden."
            : "Early ring: native grant did not complete; button remains available when state can be verified.");
    }
    if (!ok || g_ui->SaveSession() != request.session) state.valid = false;
    ::AcquireSRWLockExclusive(&g_lock);
    const Snapshot previous = g_snapshot;
    g_snapshot = state;
    if (request.unlock) g_unlockPending = false;
    else g_probePending = false;
    ::ReleaseSRWLockExclusive(&g_lock);
    if (g_log && (previous.session != state.session || previous.valid != state.valid ||
        previous.unlocked[0] != state.unlocked[0] || previous.unlocked[1] != state.unlocked[1])) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
            "Early ring state: world=%llu eligible=%d brand=%d captains=%d button=%s",
            static_cast<unsigned long long>(state.session), state.valid,
            state.unlocked[0], state.unlocked[1],
            game::ring::ShouldOffer(state, g_ui->SaveSession(), false) ? "visible" : "hidden");
        g_log(message);
    }
}

void Queue(bool unlock) {
    const auto session = g_ui->SaveSession();
    if (!session) return;
    ::AcquireSRWLockExclusive(&g_lock);
    if ((unlock && g_unlockPending) || (!unlock && (g_probePending || g_unlockPending ||
        (g_snapshot.session == session && ::GetTickCount64() - g_lastProbe < 250)))) {
        ::ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // Reject a stale or already-unlocked click before queuing; Execute checks again.
    if (unlock && (g_snapshot.session != session || !g_snapshot.valid ||
                   game::ring::Complete(g_snapshot))) {
        ::ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (unlock) g_unlockPending = true;
    else g_probePending = true;
    g_lastProbe = ::GetTickCount64();
    ::ReleaseSRWLockExclusive(&g_lock);
    auto* request = new (std::nothrow) Request{session, unlock};
    if (!request || !g_ui->QueueGameTask(&Execute, request)) {
        delete request;
        ::AcquireSRWLockExclusive(&g_lock);
        if (unlock) g_unlockPending = false;
        else g_probePending = false;
        ::ReleaseSRWLockExclusive(&g_lock);
        if (unlock && g_log) g_log("Early ring: game task queue unavailable; no grant was made.");
    }
}

bool Visible() {
    const auto session = g_ui->SaveSession();
    Queue(false);
    ::AcquireSRWLockShared(&g_lock);
    const bool result = game::ring::ShouldOffer(g_snapshot, session, g_unlockPending);
    ::ReleaseSRWLockShared(&g_lock);
    return result;
}
void Unlock() { Queue(true); }
}

void AddEarlyRingUnlock(int page, HagUIAPI* ui, void (*log)(const char*)) {
    if (!ui || ui->abiVersion < 8) return;
    if (!InstallRingGrantGuard()) {
        if (log) log("Early ring unavailable: native duplicate-grant guard could not be installed.");
        return;
    }
    g_ui = ui;
    g_log = log;
    ui->AddHeaderButton(page, "Early Unlock Ring", &Unlock, &Visible);
    if (log) log("Early ring action registered; unlock state comes from the loaded save.");
}
}  // namespace inventory_editor
