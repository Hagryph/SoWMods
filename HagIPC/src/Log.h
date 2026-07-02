#pragma once
#include "PCH.h"
#include <string>

// Minimal standalone logger for HagIPC. Writes to %LOCALAPPDATA%\SoWLoader\logs\HagIPC.log (the same
// place SoWLoader keeps its per-mod logs), so all Hagryph tooling logs land in one folder. HagIPC is
// injected as its own DLL, so it does NOT share SoWLoader's Log instance — this is self-contained.
namespace hag {

class Log {
public:
    static void Init();
    static void Line(const std::string& msg);
};

}  // namespace hag

// printf-free, iostream-free formatting kept trivial on purpose; callers build the string.
#define HAG_LOG(msg) ::hag::Log::Line(msg)
