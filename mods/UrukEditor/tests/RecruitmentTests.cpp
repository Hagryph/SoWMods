#include "RecruitmentPolicy.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
bool LevelPredicate(unsigned target, unsigned player, int requirement, bool scoped) {
    if (requirement == 0) return true;
    return (target <= uruk::RecruitmentComparisonLevel(player, scoped)) == (requirement == 1);
}
}

int main() {
    Check(uruk::RecruitmentComparisonLevel(9, false) == 9, "Unrelated player-level reads changed");
    Check(!LevelPredicate(30, 9, 1, false), "Vanilla high-level restriction was lost");
    Check(LevelPredicate(30, 9, 1, true), "High-level recruitment option remains blocked");
    Check(!LevelPredicate(30, 9, 2, true), "Too-high-level fallback remains selectable");
    Check(LevelPredicate(30, 9, 0, true), "Unconditional option was blocked");
    Check(LevelPredicate(4, 9, 1, false), "Vanilla lower-level recruitment changed");

    Check(uruk::ShouldNormalizeRecruit(true, true, false, true, true, 30, 9), "Over-level successful recruit was not normalized");
    Check(!uruk::ShouldNormalizeRecruit(false, true, false, true, true, 30, 9), "Disabled setting changed a recruit");
    Check(!uruk::ShouldNormalizeRecruit(true, false, false, true, true, 30, 9), "Invalid or replaced player changed a recruit");
    Check(!uruk::ShouldNormalizeRecruit(true, true, true, true, true, 30, 9), "Existing follower was normalized");
    Check(!uruk::ShouldNormalizeRecruit(true, true, false, false, true, 30, 9), "Failed recruitment changed level");
    Check(!uruk::ShouldNormalizeRecruit(true, true, false, true, false, 30, 9), "Unbranding changed level");
    Check(!uruk::ShouldNormalizeRecruit(true, true, false, true, true, 9, 9), "Equal-level recruit changed");
    Check(!uruk::ShouldNormalizeRecruit(true, true, false, true, true, 4, 9), "Lower-level recruit lost vanilla bonuses");
    Check(uruk::ShouldNormalizeRecruit(true, true, false, true, true, 1, 0), "Level-one player was treated as missing");
    std::puts("UrukEditor recruitment policy tests passed");
}
