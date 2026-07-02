#include "MemScan.h"

#include <vector>
#include <sstream>
#include <cstdio>

namespace hag::ipc {

namespace {

// Raw, SEH-guarded scan. No C++ objects with destructors live inside the __try scope (the vector is
// the caller's), so SEH unwinding is safe. Fills out[] up to maxHits and returns the count found.
static std::size_t ScanRaw(std::uint64_t value, int size, std::uintptr_t* out, std::size_t maxHits) {
    std::size_t n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    std::uintptr_t addr = 0x10000;                     // skip the null region
    const std::uintptr_t limit = 0x7FFFFFFF0000ULL;    // user-mode ceiling
    while (addr < limit && ::VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.RegionSize == 0) break;                // no progress -> stop
        if (mbi.State == MEM_COMMIT) {
            const DWORD pr = mbi.Protect & 0xFF;
            const bool readable = pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READ ||
                                  pr == PAGE_EXECUTE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_WRITECOPY;
            if (readable && !(mbi.Protect & PAGE_GUARD)) {
                __try {
                    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                    for (std::uintptr_t p = base; p + 8 <= regionEnd; p += 8) {
                        const std::uint64_t v = *reinterpret_cast<const std::uint64_t*>(p);
                        const std::uint64_t cmp = (size == 8) ? v : (v & 0xFFFFFFFFULL);
                        if (cmp == value) { out[n++] = p; if (n >= maxHits) return n; }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { /* region changed under us -> skip it */ }
            }
        }
        addr = regionEnd;
    }
    return n;
}

}  // namespace

std::string ScanValue(std::uint64_t value, int size, std::size_t maxHits) {
    if (maxHits == 0 || maxHits > 65536) maxHits = 8192;
    std::vector<std::uintptr_t> hits(maxHits);
    const std::size_t n = ScanRaw(value, size, hits.data(), maxHits);

    std::ostringstream o;
    o << "ok hits=" << n << (n >= maxHits ? " (capped)" : "");
    for (std::size_t i = 0; i < n; ++i) {
        char b[20]; std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(hits[i]));
        o << '\x1f' << b;
    }
    return o.str();
}

}  // namespace hag::ipc
