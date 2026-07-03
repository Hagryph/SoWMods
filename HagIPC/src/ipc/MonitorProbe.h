#pragma once
#include "PCH.h"
#include <string>
#include <cstdint>

namespace hag::ipc {

// Sampling call-edge monitor. While running, a background thread periodically suspends each game thread,
// reads a slice of its stack, and records every DIRECT call edge found on it: a stack value V that points
// into .text and has 0xE8 (call rel32) at V-5 means "the site at V-5 called V + *(int32*)(V-4)" — an
// exact caller->callee pair read straight from the call bytes (no symbols, no unwinding). Unique edges
// are accumulated with hit counts. Activate it, perform the action (e.g. pause), stop it, and the action's
// call graph is the set of captured edges.
//
// Safety: threads are suspended one at a time and resumed immediately (only a guarded stack COPY happens
// while suspended — no allocation, no RtlLookupFunctionEntry/unwind lock). The shared read snapshot is
// published only at loop top when NO thread is suspended, so a `monitorlog` can never deadlock the sampler.
class MonitorProbe {
public:
    static MonitorProbe& Get();

    std::string Start(unsigned intervalMs);      // begin sampling (ms between passes; 0 => default)
    std::string Stop();                          // stop + join, return a summary
    std::string Drain(std::size_t maxEntries);   // "callerRVA->calleeRVA xHits" edges, most-hit first

    MonitorProbe(const MonitorProbe&) = delete;
    MonitorProbe& operator=(const MonitorProbe&) = delete;

private:
    MonitorProbe() = default;
};

}  // namespace hag::ipc
