#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "RingGrantGuard.h"
#include "GameOffsets.h"
#include "RingUnlockRecords.h"
#include <MinHook.h>
#include <cstring>

namespace inventory_editor {
namespace {
using AddOneFn = bool(__fastcall*)(std::uintptr_t, std::uintptr_t, int);
using HasItemFn = bool(__fastcall*)(std::uintptr_t, std::uintptr_t);
AddOneFn g_original = nullptr;

bool IsDuplicate(std::uintptr_t inventory, std::uintptr_t row, int quantity) {
    __try {
        if (!inventory || !row || quantity <= 0) return false;
        const char* name = *reinterpret_cast<const char**>(row + 0x20);
        if (!name) return false;
        int grant = -1;
        for (int i = 0; i < 2; ++i) {
            if (std::strcmp(name, game::ring::kGrants[i].item) == 0) { grant = i; break; }
        }
        if (grant < 0) return false;
        const auto descriptor = *reinterpret_cast<std::uintptr_t*>(
            game::FromRVA(game::kInventoryItemDescriptor));
        if (!descriptor || *reinterpret_cast<std::uintptr_t*>(row + 0x18) != descriptor) return false;
        const auto table = *reinterpret_cast<std::uintptr_t*>(descriptor + 0x38);
        const auto count = *reinterpret_cast<std::uint32_t*>(descriptor + 0x28);
        if (!table || count > 100000) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto candidate = table + i * 0x28;
            const auto key = *reinterpret_cast<const char**>(candidate + 0x20);
            if (key && std::strcmp(key, game::ring::kGrants[grant].unlocked) == 0) {
                const bool unlocked = reinterpret_cast<HasItemFn>(
                    game::FromRVA(game::kInventoryHasItem))(inventory, candidate);
                return game::ring::SkipDuplicate(grant, quantity, unlocked);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // An unreadable/unrecognized record must always follow the original path.
    }
    return false;
}

bool __fastcall AddOne(std::uintptr_t inventory, std::uintptr_t row, int quantity) {
    if (IsDuplicate(inventory, row, quantity)) {
        ::OutputDebugStringA("[InventoryEditor] skipped duplicate ring ability grant; returning success\n");
        return true;
    }
    return g_original(inventory, row, quantity);
}

bool ExpectedCode(const void* target) {
    const unsigned char expected[] = { 0x48, 0x8b, 0xc4, 0x53, 0x48, 0x83, 0xec, 0x40 };
    __try { return std::memcmp(target, expected, sizeof(expected)) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool InstallRingGrantGuard() {
    if (g_original) return true;
    void* target = reinterpret_cast<void*>(game::FromRVA(game::kInventoryAddOne));
    if (!ExpectedCode(target)) {
        ::OutputDebugStringA("[InventoryEditor] unsupported inventory grant code; early unlock unavailable\n");
        return false;
    }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&AddOne),
                      reinterpret_cast<void**>(&g_original)) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_original = nullptr;
        return false;
    }
    ::OutputDebugStringA("[InventoryEditor] save-aware duplicate grant guard installed\n");
    return true;
}
}  // namespace inventory_editor
