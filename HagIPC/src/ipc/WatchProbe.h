#pragma once
#include "PCH.h"
#include <string>
#include <cstdint>

namespace hag::ipc {

// Hardware WRITE watchpoint (x64 debug register DR0 + a vectored exception handler). Arming sets a
// write breakpoint on <addr> across every game thread; each write to that address raises a single-step
// exception whose RIP is captured (the instruction right AFTER the writing instruction) into a ring,
// drained via 'watchlog'. This finds "who writes this heap variable" when static xref can't — the write
// targets a heap address through indirect dispatch, so there is no static reference to follow.
// Non-destructive: the write completes normally; we only record the writer's RIP.
class WatchProbe {
public:
    static WatchProbe& Get();

    std::string Arm(std::uintptr_t addr, int len);   // len = 1|2|4|8, addr must be len-aligned
    std::string Disarm();                            // clear the breakpoint on all threads
    std::string Drain(std::size_t maxEntries);       // pop+format captured writer RIPs

    WatchProbe(const WatchProbe&) = delete;
    WatchProbe& operator=(const WatchProbe&) = delete;

private:
    WatchProbe() = default;
};

}  // namespace hag::ipc
