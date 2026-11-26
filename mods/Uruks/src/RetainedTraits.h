#pragma once
#include <cstdint>
#include <set>

namespace uruk::retained {
bool Install();
bool WeaknessWaived(std::uintptr_t entry,std::uintptr_t context);
bool Remember(std::uintptr_t member,const std::set<std::uintptr_t>& outputs);
int RefreshLoaded();
}
