#pragma once
#include "PCH.h"

namespace hag::mem {

// Follow a pointer chain from 'start': for each offset c in chain, p = *(uintptr_t*)p + c;
// then copy 'size' bytes at the final p into 'out'. SEH-guarded: any access violation
// (e.g. a bad offset or a freed object) returns false instead of crashing the game.
// On success, *finalAddr (if non-null) receives the resolved address.
bool ReadChain(std::uintptr_t start, const std::uintptr_t* chain, std::size_t chainLen,
               void* out, std::size_t size, std::uintptr_t* finalAddr);

// Same chain resolution, then write 'size' bytes from 'src' at the final address. SEH-guarded.
bool WriteChain(std::uintptr_t start, const std::uintptr_t* chain, std::size_t chainLen,
                const void* src, std::size_t size, std::uintptr_t* finalAddr);

}  // namespace hag::mem
