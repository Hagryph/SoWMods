#include "MonitorProbe.h"
#include "Log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace hag::ipc {

namespace {

std::atomic<bool>  g_run{false};
std::thread        g_thread;
std::uintptr_t     g_base = 0, g_imgEnd = 0;   // game module [base, base+SizeOfImage)
std::map<std::uint64_t, std::uint32_t> g_edges;      // sampler-private: key=(callSiteRva<<32)|calleeRva -> hits
std::map<std::uint64_t, std::uint32_t> g_snapshot;   // published read copy (guarded by g_snapMx)
std::mutex         g_snapMx;
unsigned           g_intervalMs = 2;
std::uint64_t      g_samples = 0;

// SEH-only helpers (no C++ objects) — reading arbitrary memory can fault; return a miss instead.
bool SafeCopy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int SafeByte(std::uintptr_t a) {
    __try { return *reinterpret_cast<volatile unsigned char*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
int SafeI32(std::uintptr_t a) {
    __try { return *reinterpret_cast<volatile int*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline bool InText(std::uintptr_t a) { return a >= g_base && a < g_imgEnd; }

// Scan a copied stack for direct-call return addresses and record their edges. Runs AFTER the sampled
// thread is resumed (map allocation must not happen while a thread is suspended).
void ProcessStack(const unsigned char* buf, size_t n) {
    for (size_t off = 0; off + 8 <= n; off += 8) {
        std::uintptr_t v; std::memcpy(&v, buf + off, sizeof(v));
        if (!InText(v)) continue;                         // not a code pointer
        if (SafeByte(v - 5) != 0xE8) continue;            // not preceded by `call rel32`
        const std::uintptr_t callSite = v - 5;
        const std::uintptr_t callee   = static_cast<std::uintptr_t>(static_cast<long long>(v) + SafeI32(v - 4));
        if (!InText(callee)) continue;
        const std::uint64_t key = (static_cast<std::uint64_t>(callSite - g_base) << 32)
                                | static_cast<std::uint32_t>(callee - g_base);
        auto it = g_edges.find(key);
        if (it != g_edges.end()) ++it->second;
        else if (g_edges.size() < 300000) g_edges.emplace(key, 1u);
    }
}

void SampleOnce() {
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    static unsigned char buf[8192];   // sampler thread only -> not reentrant
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            size_t copied = 0;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c) && InText(c.Rip)) {
                    size_t want = sizeof(buf);            // copy a stack slice from RSP (guarded)
                    if (!SafeCopy(buf, reinterpret_cast<void*>(c.Rsp), want)) {
                        want = 1024;
                        if (!SafeCopy(buf, reinterpret_cast<void*>(c.Rsp), want)) want = 0;
                    }
                    copied = want;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
            if (copied) ProcessStack(buf, copied);        // process only after resume (no thread suspended)
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    ++g_samples;
}

void SamplerLoop() {
    while (g_run.load()) {
        { std::lock_guard<std::mutex> lk(g_snapMx); g_snapshot = g_edges; }  // publish; no thread suspended here
        SampleOnce();
        Sleep(g_intervalMs);
    }
    { std::lock_guard<std::mutex> lk(g_snapMx); g_snapshot = g_edges; }
}

}  // namespace

MonitorProbe& MonitorProbe::Get() { static MonitorProbe m; return m; }

std::string MonitorProbe::Start(unsigned intervalMs) {
    if (g_run.load()) return "err monitor already running (monitor stop first)";
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return "err GetModuleHandle(NULL) failed";
    g_base = reinterpret_cast<std::uintptr_t>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    g_imgEnd = g_base + nt->OptionalHeader.SizeOfImage;
    g_edges.clear();
    { std::lock_guard<std::mutex> lk(g_snapMx); g_snapshot.clear(); }
    g_samples = 0;
    g_intervalMs = intervalMs ? intervalMs : 2;
    g_run.store(true);
    g_thread = std::thread(SamplerLoop);
    hag::Log::Line("[monitor] started (interval " + std::to_string(g_intervalMs) + "ms)");
    return "ok monitor started (interval " + std::to_string(g_intervalMs) + "ms) — do the action, then 'monitor stop'";
}

std::string MonitorProbe::Stop() {
    if (!g_run.load()) return "ok (monitor not running)";
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lk(g_snapMx);
    hag::Log::Line("[monitor] stopped: " + std::to_string(g_snapshot.size()) + " edges / "
                   + std::to_string(g_samples) + " samples");
    return "ok monitor stopped: " + std::to_string(g_snapshot.size()) + " unique edges over "
           + std::to_string(g_samples) + " samples (use 'monitorlog')";
}

std::string MonitorProbe::Drain(std::size_t maxEntries) {
    std::map<std::uint64_t, std::uint32_t> snap;
    { std::lock_guard<std::mutex> lk(g_snapMx); snap = g_snapshot; }
    if (snap.empty()) return "ok (no edges captured)";
    std::vector<std::pair<std::uint64_t, std::uint32_t>> v(snap.begin(), snap.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::size_t n = v.size();
    if (maxEntries && n > maxEntries) n = maxEntries;
    std::ostringstream o;
    o << "ok edges=" << snap.size() << " shown=" << n;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t key = v[i].first;
        const std::uintptr_t caller = 0x140000000ull + (key >> 32);
        const std::uintptr_t callee = 0x140000000ull + static_cast<std::uint32_t>(key);
        char b[80];
        std::snprintf(b, sizeof(b), "0x%llx->0x%llx x%u",
                      (unsigned long long)caller, (unsigned long long)callee, v[i].second);
        o << '\x1f' << b;
    }
    return o.str();
}

}  // namespace hag::ipc
