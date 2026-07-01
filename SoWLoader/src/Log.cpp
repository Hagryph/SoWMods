#include "Log.h"

namespace sow {

Log& Log::Get() {
    static Log instance;
    return instance;
}

std::wstring Log::LogPath() {
    // %LOCALAPPDATA%\SoWLoader\ — always writable by the launching user (unlike Program Files).
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) {
        dir.assign(base, n);
        dir += L"\\SoWLoader";
    } else {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);   // ends with a backslash
        dir = tmp;
        dir += L"SoWLoader";
    }
    ::CreateDirectoryW(dir.c_str(), nullptr);   // harmless if it already exists
    return dir + L"\\SoWLoader.log";
}

Log::Log() {
    ::InitializeCriticalSection(&cs_);
    handle_ = ::CreateFileW(LogPath().c_str(), FILE_APPEND_DATA,  // NOLINT
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

Log::~Log() {
    if (handle_ != INVALID_HANDLE_VALUE) { ::CloseHandle(handle_); }
    ::DeleteCriticalSection(&cs_);
}

void Log::Line(const std::string& msg) {
    if (handle_ == INVALID_HANDLE_VALUE) { return; }

    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    char stamp[32]{};
    const int len = ::wsprintfA(stamp, "[%02d:%02d:%02d.%03d] ",
                                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    ::EnterCriticalSection(&cs_);
    DWORD written = 0;
    ::WriteFile(handle_, stamp, static_cast<DWORD>(len), &written, nullptr);
    ::WriteFile(handle_, msg.c_str(), static_cast<DWORD>(msg.size()), &written, nullptr);
    ::WriteFile(handle_, "\r\n", 2, &written, nullptr);
    ::LeaveCriticalSection(&cs_);
}

}  // namespace sow
