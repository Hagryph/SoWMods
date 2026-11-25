#include "RecruitmentHooks.h"
#include "RecruitmentPolicy.h"
#include "Settings.h"
#include "GameOffsets.h"
#include <MinHook.h>
#include <cstdio>
#include <cstring>

namespace uruk {
namespace {
using OptionFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uintptr_t);
using LevelFn = std::uint32_t(__fastcall*)(std::uintptr_t);
using BrandFn = void(__fastcall*)(std::uintptr_t, unsigned char);
using SetLevelFn = void(__fastcall*)(std::uintptr_t, std::uint32_t);
OptionFn g_option = nullptr;
LevelFn g_level = nullptr;
BrandFn g_brand = nullptr;
thread_local bool g_recruitmentComparison = false;

struct World {
    std::uintptr_t player = 0;
    std::uintptr_t inventory = 0;
    std::uintptr_t xpManager = 0;
    std::uint32_t level = 0;
};
struct Member {
    std::uintptr_t character = 0;
    std::uint32_t level = 0;
    bool follower = false;
};

bool ReadWorld(World& out) {
    __try {
        const auto engine = *reinterpret_cast<std::uintptr_t*>(game::FromRVA(game::kEngineSingleton));
        const auto systems = *reinterpret_cast<std::uintptr_t*>(game::FromRVA(game::kGameSystemsSingleton));
        if (!engine || !systems) return false;
        out.player = *reinterpret_cast<std::uintptr_t*>(engine + 0x888);
        out.xpManager = *reinterpret_cast<std::uintptr_t*>(systems + 0x6d20);
        if (!out.player || !out.xpManager) return false;
        const auto component = *reinterpret_cast<std::uintptr_t*>(out.player + 0x26a8);
        if (!component) return false;
        out.inventory = *reinterpret_cast<std::uintptr_t*>(out.player + 0x2348);
        out.level = *reinterpret_cast<std::uint32_t*>(component + 0x568);
        return out.inventory != 0 && out.level < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadMember(std::uintptr_t member, Member& out) {
    __try {
        if (!member) return false;
        out.character = *reinterpret_cast<std::uintptr_t*>(member + 0x48);
        out.level = *reinterpret_cast<std::uint32_t*>(member + 0x4b8);
        out.follower = (*reinterpret_cast<std::uint64_t*>(member + 0x508) & (1ull << 27)) != 0;
        return out.character != 0 && out.level < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::uint64_t __fastcall Option(std::uintptr_t record, std::uintptr_t inventory, std::uintptr_t target) {
    World world;
    const bool previous = g_recruitmentComparison;
    g_recruitmentComparison = IgnoreRecruitmentLevel() && ReadWorld(world) &&
        world.inventory == inventory && target != 0 && target != world.player;
    const auto result = g_option(record, inventory, target);
    g_recruitmentComparison = previous;
    return result;
}

std::uint32_t __fastcall PlayerLevel(std::uintptr_t manager) {
    const auto real = g_level(manager);
    World world;
    // Both positive and negative CanRecruitTarget predicates see the same comparison:
    // recruit becomes eligible, while the "too high level" fallback becomes ineligible.
    return RecruitmentComparisonLevel(real, g_recruitmentComparison &&
        ReadWorld(world) && manager == world.xpManager);
}

bool SetLevel(std::uintptr_t member, std::uint32_t level) {
    __try {
        reinterpret_cast<SetLevelFn>(game::FromRVA(game::kFactionMemberSetXPLevel))(member, level);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __fastcall Brand(std::uintptr_t member, unsigned char recruit) {
    World beforeWorld;
    Member before;
    const bool enabled = IgnoreRecruitmentLevel();
    const bool valid = enabled && recruit && ReadWorld(beforeWorld) && ReadMember(member, before) &&
        before.character != beforeWorld.player;
    g_brand(member, recruit);
    World afterWorld;
    Member after;
    const bool current = valid && ReadWorld(afterWorld) && ReadMember(member, after) &&
        beforeWorld.player == afterWorld.player && before.character == after.character;
    if (!ShouldNormalizeRecruit(enabled, current, before.follower, after.follower,
                                recruit != 0, before.level, afterWorld.level)) return;
    // The original transition has finished its recruitment bonuses and notifications.
    // Use its native level setter so save state, traits, stats, and the live actor refresh together.
    if (!SetLevel(member, afterWorld.level) || !ReadMember(member, after)) {
        Log("Recruitment succeeded but captain level adjustment failed");
        return;
    }
    char message[160]{};
    std::snprintf(message, sizeof(message), "Recruitment completed: captain level %u -> %u; player level %u",
        before.level + 1, after.level + 1, afterWorld.level + 1);
    Log(message);
    if (after.level != afterWorld.level) Log("Native level constraints prevented exact player-level matching");
}

bool CodeMatches(std::uintptr_t address, const unsigned char* code, std::size_t size) {
    __try { return std::memcmp(reinterpret_cast<void*>(game::FromRVA(address)), code, size) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool InstallRecruitmentHooks() {
    const unsigned char optionCode[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18};
    const unsigned char levelCode[] = {0x48,0x83,0xec,0x28,0x48,0x8b,0x91,0xc8,0,0,0};
    const unsigned char brandCode[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10};
    const unsigned char setterCode[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
    if (!CodeMatches(game::kRecruitmentOptionPredicate, optionCode, sizeof(optionCode)) ||
        !CodeMatches(game::kPlayerXPLevelIndex, levelCode, sizeof(levelCode)) ||
        !CodeMatches(game::kFactionMemberSetBranded, brandCode, sizeof(brandCode)) ||
        !CodeMatches(game::kFactionMemberSetXPLevel, setterCode, sizeof(setterCode))) {
        Log("Unsupported recruitment code signatures; no hooks installed");
        return false;
    }
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    struct Hook { void* target; void* detour; void** original; };
    const Hook hooks[] = {
        {reinterpret_cast<void*>(game::FromRVA(game::kRecruitmentOptionPredicate)), reinterpret_cast<void*>(&Option), reinterpret_cast<void**>(&g_option)},
        {reinterpret_cast<void*>(game::FromRVA(game::kPlayerXPLevelIndex)), reinterpret_cast<void*>(&PlayerLevel), reinterpret_cast<void**>(&g_level)},
        {reinterpret_cast<void*>(game::FromRVA(game::kFactionMemberSetBranded)), reinterpret_cast<void*>(&Brand), reinterpret_cast<void**>(&g_brand)}
    };
    int created = 0;
    for (const auto& hook : hooks) {
        if (MH_CreateHook(hook.target, hook.detour, hook.original) != MH_OK) break;
        ++created;
    }
    bool ok = created == 3;
    if (ok) {
        for (const auto& hook : hooks) {
            if (MH_EnableHook(hook.target) != MH_OK) { ok = false; break; }
        }
    }
    if (!ok) {
        for (int i = 0; i < created; ++i) {
            MH_DisableHook(hooks[i].target);
            MH_RemoveHook(hooks[i].target);
        }
        g_option = nullptr; g_level = nullptr; g_brand = nullptr;
        Log("Recruitment hook setup failed; rolled back all UrukEditor hooks");
        return false;
    }
    Log("Installed recruitment option level bypass and post-recruit level normalization");
    return true;
}
}
