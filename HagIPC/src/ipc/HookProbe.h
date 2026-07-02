#pragma once
#include "PCH.h"
#include <string>
#include <cstdint>

namespace hag::ipc {

// Runtime logging hooks for live RE (the enabler for "hook the deserializer and watch items load").
// A fixed pool of hook slots; installing one MinHook-trampolines the target so that EVERY call
// records its first four register arguments (MS x64 ABI: RCX, RDX, R8, R9) into a ring buffer and
// then tail-calls the original. Non-destructive (always calls original), safe to add/remove live.
//
// CAVEAT: only the 4 register args are captured/forwarded, so this is correct for target functions
// that take <= 4 integer/pointer args (the deserialization read primitives take 3; most item funcs
// <= 4). Do NOT hook >4-arg or float-arg functions with this — the extra/stack args aren't modeled.
class HookProbe {
public:
    static HookProbe& Get();

    bool        Init();                                 // MH_Initialize once (idempotent)
    std::string Install(std::uintptr_t target);         // "ok slot=N @VA" | "err ..."
    std::string Remove(std::uintptr_t targetOrSlot);    // by runtime VA or slot index
    std::string RemoveAll();
    std::string Drain(std::size_t maxEntries);          // pop+format ring-buffer entries as text

    HookProbe(const HookProbe&) = delete;
    HookProbe& operator=(const HookProbe&) = delete;

private:
    HookProbe() = default;
    bool inited_ = false;
};

}  // namespace hag::ipc
