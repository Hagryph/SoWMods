#include "Settings.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
    // This executable is built in its own test directory, so its cfg is isolated
    // from both the installed game's settings and the source default configuration.
    const auto module = ::GetModuleHandleW(nullptr);
    Check(uruk::LoadSettings(module), "Could not create isolated test config");
    uruk::SetIgnoreRecruitmentLevel(false);
    Check(!uruk::IgnoreRecruitmentLevel(), "Disable did not apply");
    uruk::SetIgnoreRecruitmentLevel(true);
    Check(uruk::IgnoreRecruitmentLevel(), "Enable did not apply");
    Check(uruk::LoadSettings(module) && uruk::IgnoreRecruitmentLevel(), "Enabled setting did not survive reload");
    uruk::SetIgnoreRecruitmentLevel(false);
    Check(uruk::LoadSettings(module) && !uruk::IgnoreRecruitmentLevel(), "Disabled setting did not survive reload");
    std::puts("UrukEditor settings persistence tests passed");
}
