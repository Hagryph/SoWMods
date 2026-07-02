#include "PCH.h"
#include "Log.h"

namespace hag {

namespace {
CRITICAL_SECTION g_cs;
bool             g_init = false;

std::wstring LogPath() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) { dir.assign(base, n); dir += L"\\SoWLoader"; }
    else { wchar_t tmp[MAX_PATH]{}; ::GetTempPathW(MAX_PATH, tmp); dir = tmp; dir += L"SoWLoader"; }
    ::CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\HagIPC.log";
}
}  // namespace

void Log::Init() {
    if (g_init) return;
    ::InitializeCriticalSection(&g_cs);
    g_init = true;
}

void Log::Line(const std::string& msg) {
    if (!g_init) Init();
    SYSTEMTIME t{}; ::GetLocalTime(&t);
    char stamp[32]{};
    const int slen = ::wsprintfA(stamp, "[%02d:%02d:%02d.%03d] ",
                                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    ::EnterCriticalSection(&g_cs);
    HANDLE h = ::CreateFileW(LogPath().c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        ::WriteFile(h, stamp, static_cast<DWORD>(slen), &w, nullptr);
        ::WriteFile(h, msg.c_str(), static_cast<DWORD>(msg.size()), &w, nullptr);
        ::WriteFile(h, "\r\n", 2, &w, nullptr);
        ::CloseHandle(h);
    }
    ::LeaveCriticalSection(&g_cs);
}

}  // namespace hag
