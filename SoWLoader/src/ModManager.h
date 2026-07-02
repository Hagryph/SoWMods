#pragma once
#include "PCH.h"

namespace sow {

// Loads external mod DLLs. Only the loader core + HagUI live in steam_api64.dll; everything else is
// a standalone DLL in  <game>\x64\mods\ . ModManager enumerates that folder in filename order,
// LoadLibrary()s each, and calls the mod's optional SoWMod_Init export (see shared/SoWModAPI.h) on
// the worker thread, outside the loader lock. Called once from Loader::Worker after the hooks arm.
class ModManager {
public:
    static ModManager& Get();
    void LoadAll();

    ModManager(const ModManager&) = delete;
    ModManager& operator=(const ModManager&) = delete;

private:
    ModManager() = default;
    bool loaded_ = false;
};

}  // namespace sow
