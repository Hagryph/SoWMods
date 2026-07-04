#pragma once
#include "PCH.h"
#include "HagUIAPI.h"

namespace sow {

bool QueueGameTask(HagUI_GameTaskFn fn, void* ctx);
void DrainGameTasks(int maxTasks);

}  // namespace sow
