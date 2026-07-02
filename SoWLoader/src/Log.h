#pragma once
#include "PCH.h"
#include <string>

namespace sow {

class Console;

// Severity — drives console colouring; the file log records the level tag.
enum class Lv { Info, Good, Warn, Error, Accent };

// One logging channel. Every mod gets its own channel = its own file at
// %LOCALAPPDATA%\SoWLoader\logs\<name>.log (always written), plus an optional mirror into the
// shared mod console ([name]-prefixed, colour by severity) that the channel can switch off.
class Logger {
public:
    void Line(const std::string& msg)   { Emit(Lv::Info,   msg); }
    void Good(const std::string& msg)   { Emit(Lv::Good,   msg); }
    void Warn(const std::string& msg)   { Emit(Lv::Warn,   msg); }
    void Error(const std::string& msg)  { Emit(Lv::Error,  msg); }
    void Accent(const std::string& msg) { Emit(Lv::Accent, msg); }

    // File output is unconditional; this only controls the console mirror.
    void SetConsoleOutput(bool on) { console_ = on; }
    bool ConsoleOutput() const     { return console_; }
    const std::string& Name() const { return name_; }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    friend class Log;
    explicit Logger(const std::string& name);
    ~Logger();
    void Emit(Lv lv, const std::string& msg);

    std::string name_;
    HANDLE      handle_  = INVALID_HANDLE_VALUE;
    bool        console_ = true;
};

// The central logging service (the Loader owns setup). Channels are created on demand and live
// for the process. The console attaches LATE (once the game has initialized); lines emitted
// before that are kept in a replay buffer so the console still shows the full boot sequence.
class Log {
public:
    static Logger& Get();                             // the loader's own channel ("SoWLoader")
    static Logger& Channel(const std::string& mod);   // a mod's channel (one file per mod)
    static void    AttachConsole(Console* c);         // shared sink; replays buffered lines
};

}  // namespace sow
