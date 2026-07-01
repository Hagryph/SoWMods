#pragma once
#include "PCH.h"

namespace sow {

// Tiny dependency-free file logger (Win32 only, so it is safe to touch from DllMain).
// Writes next to the game exe: SoWLoader.log. Singleton; RAII handle.
class Log {
public:
    static Log& Get();

    void Line(const std::string& msg);

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

private:
    Log();
    ~Log();

    // Writable location (%LOCALAPPDATA%\SoWLoader\SoWLoader.log). The game's own folder is
    // under Program Files and a non-elevated x64 process cannot write there (and x64 gets no
    // UAC VirtualStore redirect), which is why logging next to the exe silently produced nothing.
    static std::wstring LogPath();

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION cs_{};
};

}  // namespace sow
