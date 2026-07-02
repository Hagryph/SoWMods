#include "WatchProbe.h"
#include "Log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <mutex>
#include <vector>
#include <sstream>
#include <cstdio>

namespace hag::ipc {

namespace {

std::mutex                 g_mx;
std::uintptr_t             g_addr  = 0;
int                        g_len   = 1;
bool                       g_armed = false;
PVOID                      g_veh   = nullptr;
std::vector<std::uint64_t> g_hits;          // captured writer RIPs (instruction after the write)
std::uint32_t              g_total = 0;      // total write hits seen (may exceed g_hits cap)

// DR7 LEN encoding: 00=1 byte, 01=2 bytes, 11=4 bytes, 10=8 bytes.
int LenBits(int len) { switch (len) { case 2: return 0x1; case 8: return 0x2; case 4: return 0x3; default: return 0x0; } }

// VEH: a hardware data breakpoint fires a single-step exception with the hit flagged in DR6. We record
// the RIP (points just past the writing instruction) and let execution continue so the write stands.
LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == (DWORD)EXCEPTION_SINGLE_STEP) {
        CONTEXT* c = ep->ContextRecord;
        if (c->Dr6 & 0x1ull) {                       // DR0 (our write watch) triggered
            std::lock_guard<std::mutex> lk(g_mx);
            ++g_total;
            if (g_hits.size() < 4096) g_hits.push_back(static_cast<std::uint64_t>(c->Rip));
            c->Dr6 &= ~0xFull;                       // clear debug status, keep the watch armed
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Apply (arm) or clear DR0/DR7 on every thread of this process except the caller (the socket thread).
void ForThreads(bool arm) {
    const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                  FALSE, te.th32ThreadID);
            if (!h) continue;
            SuspendThread(h);
            alignas(16) CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)) {
                c.Dr7 &= ~((DWORD64)0xF << 16);      // clear RW0/LEN0
                c.Dr7 &= ~(DWORD64)0x1;              // clear L0
                if (arm) {
                    c.Dr0  = g_addr;
                    c.Dr7 |= 0x1;                    // L0 local enable
                    c.Dr7 |= ((DWORD64)0x1 << 16);   // RW0 = 01 (break on write)
                    c.Dr7 |= ((DWORD64)LenBits(g_len) << 18);
                } else {
                    c.Dr0 = 0;
                }
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(h, &c);
            }
            ResumeThread(h);
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

std::string Hex(std::uint64_t v) { char b[20]; std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); return b; }

}  // namespace

WatchProbe& WatchProbe::Get() { static WatchProbe w; return w; }

std::string WatchProbe::Arm(std::uintptr_t addr, int len) {
    if (len != 1 && len != 2 && len != 4 && len != 8) return "err len must be 1|2|4|8";
    if (addr % (std::uintptr_t)len) return "err addr must be aligned to len";
    std::lock_guard<std::mutex> lk(g_mx);
    g_addr = addr; g_len = len; g_hits.clear(); g_total = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    if (!g_veh) return "err AddVectoredExceptionHandler failed";
    ForThreads(true);
    g_armed = true;
    hag::Log::Line("[watch] armed write bp @" + Hex(addr) + " len=" + std::to_string(len));
    return "ok watch @" + Hex(addr) + " len=" + std::to_string(len) + " (transition, then 'watchlog')";
}

std::string WatchProbe::Disarm() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_armed) return "ok (not armed)";
    ForThreads(false);
    g_armed = false;
    hag::Log::Line("[watch] disarmed (" + std::to_string(g_total) + " write(s) seen)");
    return "ok watch disarmed (" + std::to_string(g_total) + " write(s) seen)";
}

std::string WatchProbe::Drain(std::size_t maxEntries) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_hits.empty()) return "ok (no writes captured, total=" + std::to_string(g_total) + ")";
    std::size_t n = g_hits.size();
    if (maxEntries && n > maxEntries) n = maxEntries;
    std::ostringstream o;
    o << "ok writes=" << g_total << " shown=" << n;
    for (std::size_t k = 0; k < n; ++k) o << '\x1f' << "rip=" << Hex(g_hits[k]);
    g_hits.clear();
    return o.str();
}

}  // namespace hag::ipc
