#include "HookProbe.h"
#include "Log.h"

#include <MinHook.h>
#include <mutex>
#include <array>
#include <utility>
#include <sstream>
#include <cstdio>
#include <intrin.h>   // _ReturnAddress

namespace hag::ipc {

namespace {

constexpr int kSlots = 16;      // max simultaneous hooks
constexpr int kRing  = 8192;    // captured-call ring buffer size

// One captured call: which slot fired + its 4 register args + the CALLER (return address, which
// pinpoints the calling function — e.g. the item-template deserializer among all callers of a shared
// read-primitive), in fire order (seq).
struct Entry { std::uint32_t seq; int slot; std::uint64_t ret; std::uint64_t a0, a1, a2, a3; };

std::mutex    g_mx;
Entry         g_ring[kRing];
std::uint32_t g_head = 0;       // next write index
std::uint32_t g_count = 0;      // valid entries (<= kRing)
std::uint32_t g_seq = 0;        // monotonic call counter (also counts dropped-if-overwritten)

using Fn4 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
void*         g_target[kSlots] = {};   // runtime target VA (nullptr = free slot)
Fn4           g_tramp [kSlots] = {};   // MinHook trampoline (call-original) per slot

void Record(int slot, std::uint64_t ret, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_ring[g_head] = { ++g_seq, slot, ret, a0, a1, a2, a3 };
    g_head = (g_head + 1) % kRing;
    if (g_count < kRing) ++g_count;
}

// On x64 there is a single calling convention: args arrive in RCX,RDX,R8,R9. A plain 4-arg function
// receives exactly those. Each slot gets its OWN instantiation so `Slot` is baked in (no runtime
// codegen) and it can reference its own trampoline. Tail-call the original -> transparent + preserves
// the return value in RAX.
template <int Slot>
std::uint64_t DetourImpl(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3) {
    // _ReturnAddress() = where this detour returns to = the game caller's return address (the
    // `call target` pushed it before MinHook's jmp), i.e. an address inside the CALLING function.
    const std::uint64_t ret = reinterpret_cast<std::uint64_t>(_ReturnAddress());
    Record(Slot, ret, a0, a1, a2, a3);
    return g_tramp[Slot](a0, a1, a2, a3);
}

template <int... I>
constexpr std::array<Fn4, sizeof...(I)> MakeDetours(std::integer_sequence<int, I...>) {
    return { &DetourImpl<I>... };
}
const std::array<Fn4, kSlots> g_detours = MakeDetours(std::make_integer_sequence<int, kSlots>{});

std::string Hex(std::uint64_t v) { char b[20]; std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); return b; }

}  // namespace

HookProbe& HookProbe::Get() { static HookProbe h; return h; }

bool HookProbe::Init() {
    if (inited_) return true;
    MH_STATUS s = MH_Initialize();
    inited_ = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED);
    return inited_;
}

std::string HookProbe::Install(std::uintptr_t target) {
    if (!Init()) return "err MH_Initialize failed";
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < kSlots; ++i) if (g_target[i] == reinterpret_cast<void*>(target))
        return "err already hooked in slot " + std::to_string(i);
    int slot = -1;
    for (int i = 0; i < kSlots; ++i) if (!g_target[i]) { slot = i; break; }
    if (slot < 0) return "err no free hook slots (max " + std::to_string(kSlots) + ")";

    void* tgt = reinterpret_cast<void*>(target);
    if (MH_CreateHook(tgt, reinterpret_cast<void*>(g_detours[slot]),
                      reinterpret_cast<void**>(&g_tramp[slot])) != MH_OK)
        return "err MH_CreateHook failed";
    if (MH_EnableHook(tgt) != MH_OK) { MH_RemoveHook(tgt); return "err MH_EnableHook failed"; }
    g_target[slot] = tgt;
    hag::Log::Line("[hook] slot " + std::to_string(slot) + " -> " + Hex(target));
    return "ok slot=" + std::to_string(slot) + " @" + Hex(target);
}

std::string HookProbe::Remove(std::uintptr_t targetOrSlot) {
    std::lock_guard<std::mutex> lk(g_mx);
    int slot = -1;
    if (targetOrSlot < kSlots) {                     // small value = slot index
        slot = static_cast<int>(targetOrSlot);
    } else {
        for (int i = 0; i < kSlots; ++i) if (g_target[i] == reinterpret_cast<void*>(targetOrSlot)) { slot = i; break; }
    }
    if (slot < 0 || slot >= kSlots || !g_target[slot]) return "err no such hook";
    MH_DisableHook(g_target[slot]);
    MH_RemoveHook(g_target[slot]);
    g_target[slot] = nullptr; g_tramp[slot] = nullptr;
    return "ok removed slot " + std::to_string(slot);
}

std::string HookProbe::RemoveAll() {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = 0;
    for (int i = 0; i < kSlots; ++i) if (g_target[i]) {
        MH_DisableHook(g_target[i]); MH_RemoveHook(g_target[i]);
        g_target[i] = nullptr; g_tramp[i] = nullptr; ++n;
    }
    return "ok removed " + std::to_string(n) + " hook(s)";
}

std::string HookProbe::Drain(std::size_t maxEntries) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_count == 0) return "ok (empty)";
    std::size_t n = g_count;
    if (maxEntries && n > maxEntries) n = maxEntries;
    // oldest of the n we return
    std::uint32_t start = (g_head + kRing - g_count) % kRing;
    std::ostringstream o;
    // Single-line response with 0x1f field separators (the client turns 0x1f into newlines) — a
    // real '\n' here would break the one-line-per-response protocol and desync the stream.
    o << "ok entries=" << n << (n < g_count ? " (more buffered)" : "");
    for (std::size_t k = 0; k < n; ++k) {
        const Entry& e = g_ring[(start + k) % kRing];
        o << '\x1f' << e.seq << " slot" << e.slot << " ret=" << Hex(e.ret)
          << " a0=" << Hex(e.a0) << " a1=" << Hex(e.a1)
          << " a2=" << Hex(e.a2) << " a3=" << Hex(e.a3);
    }
    // consume the drained entries
    g_count -= static_cast<std::uint32_t>(n);
    return o.str();
}

}  // namespace hag::ipc
