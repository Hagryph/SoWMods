#include "PCH.h"
#include "ipc/MemAccess.h"

// This TU mixes SEH with C++: every __try function below holds only POD locals (no objects needing
// unwinding), which is what MSVC requires.
namespace hag::mem {

bool ReadChain(std::uintptr_t start, const std::uintptr_t* chain, std::size_t chainLen,
               void* out, std::size_t size, std::uintptr_t* finalAddr) {
    __try {
        std::uintptr_t p = start;
        for (std::size_t i = 0; i < chainLen; ++i) {
            p = *reinterpret_cast<std::uintptr_t*>(p) + chain[i];  // deref, then add
        }
        if (finalAddr) { *finalAddr = p; }
        std::memcpy(out, reinterpret_cast<const void*>(p), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteChain(std::uintptr_t start, const std::uintptr_t* chain, std::size_t chainLen,
                const void* src, std::size_t size, std::uintptr_t* finalAddr) {
    __try {
        std::uintptr_t p = start;
        for (std::size_t i = 0; i < chainLen; ++i) {
            p = *reinterpret_cast<std::uintptr_t*>(p) + chain[i];
        }
        if (finalAddr) { *finalAddr = p; }
        std::memcpy(reinterpret_cast<void*>(p), src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace hag::mem
