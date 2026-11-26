#pragma once
#include <Windows.h>
#include <cstdint>

namespace sowmod {
inline std::uint64_t SaveSession() {
    using Query = std::uint64_t (*)();
    static const auto query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "SoWMod_SaveSessionV1"));
    return query ? query() : 0;
}
}
