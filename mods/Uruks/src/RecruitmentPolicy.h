#pragma once
#include <cstdint>
#include <limits>

namespace uruk {
inline std::uint32_t RecruitmentComparisonLevel(std::uint32_t real, bool localScope) {
    return localScope ? (std::numeric_limits<std::uint32_t>::max)() : real;
}
inline bool ShouldNormalizeRecruit(bool enabled, bool valid, bool wasFollower,
    bool isFollower, bool requestedRecruit, std::uint32_t before, std::uint32_t player) {
    return enabled && valid && requestedRecruit && !wasFollower && isFollower && before > player;
}
}
