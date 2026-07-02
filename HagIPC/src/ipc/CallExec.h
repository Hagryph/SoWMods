#pragma once
#include "PCH.h"

// Call a game function or run a raw code blob. Both are SEH-guarded: a fault returns false instead
// of a guaranteed crash (best-effort — a blob that corrupts the stack can still bring the game down).
// NOTE: these run on the socket thread (inline). Fine for RE probing of load-path/query functions
// from the menu, as in the Skyrim tool; functions that MUST run on the game's main/render thread
// need frame marshaling (a later addition once we hook a SoW per-frame point).
namespace hag::exec {

// Call the function at runtime address 'addr' with n (<=8) integer/pointer args (Microsoft x64:
// RCX,RDX,R8,R9 then stack). Return value = RAX. Float args/returns are NOT supported.
bool Call(std::uintptr_t addr, const std::uint64_t* args, int n, std::uint64_t& out);

// Run a position-independent machine-code blob (entry at byte 0, ends with `ret`, MS x64 ABI).
// Returns RAX.
bool ExecBlob(const std::uint8_t* code, std::size_t len, std::uint64_t& out);

}  // namespace hag::exec
