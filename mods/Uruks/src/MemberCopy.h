#pragma once
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace uruk::engine::membercopy {
struct Region { unsigned offset, size; };
// Steam 1.21: regions belonging to the existing Army member, not its build.
// The first region includes both intrusive nodes and the live actor handle.
inline constexpr Region excluded[]={
    {0,0x50},{0x168,8},{0x174,0x14},{0x190,8},{0x268,8},
    {0x2c0,0x18},{0x2e8,0x30},{0x4b8,4},{0x4d0,0x58}
};
constexpr bool excludedRegion(Region r) {
    for (auto e:excluded)
        if (r.offset<e.offset+e.size && e.offset<r.offset+r.size) return true;
    return false;
}
// Same-offset value assignments in the native member copier. Kept separate
// from the policy: an unnamed record pointer copied by the game is still data.
// Owned containers are NOT represented by their pointer/length bytes here.
inline constexpr Region nativeValues[]={
    {0x168,8},{0x174,4},{0x178,8},{0x180,8},{0x188,8},{0x190,8},{0x268,8},
    {0x270,8},{0x278,8},{0x280,8},{0x288,8},{0x290,8},{0x2c0,8},{0x2c8,16},
    {0x318,8},{0x320,8},{0x328,8},{0x330,8},{0x338,8},{0x340,8},{0x348,8},
    {0x370,8},{0x390,8},{0x3d0,16},{0x3e0,16},{0x3f0,16},{0x400,16},{0x410,16},
    {0x460,8},{0x468,4},{0x46c,4},{0x4b8,4},{0x4c0,4},{0x4c4,4},
    {0x4f8,4},{0x4fc,4},{0x500,4},{0x508,8},{0x510,2},{0x512,2},{0x514,2},
    {0x516,2},{0x518,2},{0x51a,1},{0x51b,1},{0x51e,1}
};
inline void exchangeValues(std::byte* dst,std::byte* src) {
    for (auto r:nativeValues) if (!excludedRegion(r))
        std::swap_ranges(dst+r.offset,dst+r.offset+r.size,src+r.offset);
}
enum class Outcome { Applied, Restored, RollbackFailed };
// Exchange is reversible and does not call the engine. apply and restore
// report native copy/readback faults as false.
template<class Exchange,class Apply,class Restore>
Outcome commit(Exchange exchange,Apply apply,Restore restore) {
    exchange();
    if (apply()) return Outcome::Applied;
    exchange();
    return restore()?Outcome::Restored:Outcome::RollbackFailed;
}
}
