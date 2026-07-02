#include "Log.h"
#include "Console.h"

#include <map>
#include <vector>

namespace sow {

// ---------------------------------------------------------------------------
//  shared state (single lock guards the registry, the console, and the buffer)
// ---------------------------------------------------------------------------
namespace {
struct LogState {
    CRITICAL_SECTION cs;
    std::map<std::string, Logger*> channels;
    Console* console = nullptr;
    struct Pending { Lv lv; std::string line; };
    std::vector<Pending> pending;                    // lines emitted before the console attached
    LogState() { ::InitializeCriticalSection(&cs); }
};
LogState& State() { static LogState s; return s; }

std::wstring BaseDir() {
    // %LOCALAPPDATA%\SoWLoader\ — always writable by the launching user (unlike Program Files).
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) { dir.assign(base, n); dir += L"\\SoWLoader"; }
    else { wchar_t tmp[MAX_PATH]{}; ::GetTempPathW(MAX_PATH, tmp); dir = tmp; dir += L"SoWLoader"; }
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

constexpr size_t kMaxPending = 512;
}  // namespace

// ---------------------------------------------------------------------------
//  Logger
// ---------------------------------------------------------------------------
Logger::Logger(const std::string& name) : name_(name) {
    const std::wstring dir = BaseDir() + L"\\logs";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t wname[128]{};
    ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 128);
    handle_ = ::CreateFileW((dir + L"\\" + wname + L".log").c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

Logger::~Logger() {
    if (handle_ != INVALID_HANDLE_VALUE) { ::CloseHandle(handle_); }
}

void Logger::Emit(Lv lv, const std::string& msg) {
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    char stamp[32]{};
    const int slen = ::wsprintfA(stamp, "[%02d:%02d:%02d.%03d] ",
                                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    auto& s = State();
    ::EnterCriticalSection(&s.cs);
    if (handle_ != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(handle_, stamp, static_cast<DWORD>(slen), &written, nullptr);
        ::WriteFile(handle_, msg.c_str(), static_cast<DWORD>(msg.size()), &written, nullptr);
        ::WriteFile(handle_, "\r\n", 2, &written, nullptr);
    }
    if (console_) {
        const std::string line = std::string(stamp, slen) + "[" + name_ + "] " + msg;
        if (s.console) { s.console->Write(lv, line); }
        else if (s.pending.size() < kMaxPending) { s.pending.push_back({ lv, line }); }
    }
    ::LeaveCriticalSection(&s.cs);
}

// ---------------------------------------------------------------------------
//  Log (registry + console attach)
// ---------------------------------------------------------------------------
Logger& Log::Get() { return Channel("SoWLoader"); }

Logger& Log::Channel(const std::string& mod) {
    auto& s = State();
    ::EnterCriticalSection(&s.cs);
    auto it = s.channels.find(mod);
    if (it == s.channels.end()) {
        it = s.channels.emplace(mod, new Logger(mod)).first;   // channels live for the process
    }
    Logger& ch = *it->second;
    ::LeaveCriticalSection(&s.cs);
    return ch;
}

void Log::AttachConsole(Console* c) {
    auto& s = State();
    ::EnterCriticalSection(&s.cs);
    s.console = c;
    if (c) {
        for (const auto& p : s.pending) { c->Write(p.lv, p.line); }   // replay the boot sequence
        s.pending.clear();
    }
    ::LeaveCriticalSection(&s.cs);
}

}  // namespace sow
