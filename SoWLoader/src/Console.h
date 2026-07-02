#pragma once
#include "Log.h"   // Lv
#include <string>

namespace sow {

// The mod console: a real console window (AllocConsole) the Loader opens when enabled. Log mirrors
// every line here with severity colouring. Hidden via SoWLoader.ini `console=0` or the `-sowsilent`
// launch option (see Config).
class Console {
public:
    static Console& Get();

    bool Open();                                   // AllocConsole + banner; idempotent
    void Write(Lv lv, const std::string& line);    // colour by severity
    bool IsOpen() const { return open_; }

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

private:
    Console() = default;
    HANDLE           out_    = nullptr;
    bool             open_   = false;
    bool             csInit_ = false;
    CRITICAL_SECTION cs_{};
};

}  // namespace sow
