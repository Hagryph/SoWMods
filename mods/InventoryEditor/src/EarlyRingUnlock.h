#pragma once
#include "HagUIAPI.h"
namespace inventory_editor {
bool RegisteredRingAction(bool unlock);
void AddEarlyRingUnlock(int page, HagUIAPI* ui, void (*log)(const char*));
}
