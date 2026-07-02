#pragma once
#include "PCH.h"
#include "GameOffsets.h"  // shared raw offset table (single source of truth). Only file to include it.

// Per-module base + RVA helpers for HagIPC, re-exported from shared/GameOffsets.h. HagIPC itself
// needs no named offsets (it's a generic RVA debugger); it only needs Base()/FromRVA so the socket
// protocol can resolve a file address (off 0x140000000, e.g. 0x141976838) to a live runtime VA.
namespace hag::offsets {

inline constexpr std::uintptr_t kImageBase = game::kImageBase;

inline std::uintptr_t Base()                       { return game::Base(); }
inline std::uintptr_t FromRVA(std::uintptr_t addr) { return game::FromRVA(addr); }  // full 0x140.. addr -> VA

}  // namespace hag::offsets
