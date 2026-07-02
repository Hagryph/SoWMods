#pragma once
#include "PCH.h"
#include <string>
#include <cstdint>

namespace hag::ipc {

// Scan all committed, readable memory for occurrences of `value` (compared as u64 or u32), at 8-byte
// alignment (object vtable/pointer fields are 8-aligned). In-process, so it's fast (seconds over a
// few GB) where 4KB TCP readb would be hopeless. Returns a single-line "ok hits=N" + 0x1f-separated
// hit addresses (matches the client's 0x1f handling). Primary use: find every object of a class by
// scanning for its (fixed, no-ASLR) vtable address -> each hit is that object's +0x00.
std::string ScanValue(std::uint64_t value, int size, std::size_t maxHits);

}  // namespace hag::ipc
