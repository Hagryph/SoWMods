#include "PCH.h"
#include "ipc/CallExec.h"

// POD-only SEH frames (see MemAccess.cpp note).
namespace hag::exec {

using Fn0 = std::uint64_t (*)();
using Fn1 = std::uint64_t (*)(std::uint64_t);
using Fn2 = std::uint64_t (*)(std::uint64_t, std::uint64_t);
using Fn3 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);
using Fn4 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Fn5 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Fn6 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Fn7 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
using Fn8 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

bool Call(std::uintptr_t addr, const std::uint64_t* a, int n, std::uint64_t& out) {
    __try {
        switch (n) {
            case 0: out = reinterpret_cast<Fn0>(addr)(); return true;
            case 1: out = reinterpret_cast<Fn1>(addr)(a[0]); return true;
            case 2: out = reinterpret_cast<Fn2>(addr)(a[0], a[1]); return true;
            case 3: out = reinterpret_cast<Fn3>(addr)(a[0], a[1], a[2]); return true;
            case 4: out = reinterpret_cast<Fn4>(addr)(a[0], a[1], a[2], a[3]); return true;
            case 5: out = reinterpret_cast<Fn5>(addr)(a[0], a[1], a[2], a[3], a[4]); return true;
            case 6: out = reinterpret_cast<Fn6>(addr)(a[0], a[1], a[2], a[3], a[4], a[5]); return true;
            case 7: out = reinterpret_cast<Fn7>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); return true;
            case 8: out = reinterpret_cast<Fn8>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); return true;
            default: return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ExecBlob(const std::uint8_t* code, std::size_t len, std::uint64_t& out) {
    void* memv = ::VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!memv) return false;
    std::memcpy(memv, code, len);
    ::FlushInstructionCache(::GetCurrentProcess(), memv, len);

    bool ok = false;
    __try {
        out = reinterpret_cast<std::uint64_t (*)()>(memv)();
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    ::VirtualFree(memv, 0, MEM_RELEASE);
    return ok;
}

}  // namespace hag::exec
