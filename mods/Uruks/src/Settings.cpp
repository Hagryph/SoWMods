#include "Settings.h"
#include "ModConfig.h"
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <string>

namespace uruk {
namespace {
sowmod::Config g_config;
sowmod::Option<bool> g_ignoreLevel;
}

void Log(const char* message) {
    ::OutputDebugStringA("[Uruks] ");
    ::OutputDebugStringA(message);
    ::OutputDebugStringA("\n");
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const std::wstring root = std::wstring(base, n) + L"\\SoWLoader";
    ::CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring logs = root + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    HANDLE file = ::CreateFileW((logs + L"\\Uruks.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    char line[1024]{};
    const int length = std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\r\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message);
    DWORD written = 0;
    if (length > 0) ::WriteFile(file, line,
        static_cast<DWORD>((std::min)(length, static_cast<int>(sizeof(line) - 1))), &written, nullptr);
    ::CloseHandle(file);
}

bool LoadSettings(int page) {
    try {
        g_config = sowmod::Config(page);
        g_ignoreLevel = g_config.Register<bool>("Recruitment", "IgnoreRecruitmentLevel", false);
        Log(IgnoreRecruitmentLevel() ? "Loader config loaded: recruitment bypass enabled" : "Loader config loaded: recruitment bypass disabled");
        return true;
    } catch (...) { return false; }
}

bool IgnoreRecruitmentLevel() {
    try { return g_config.Read(g_ignoreLevel); }
    catch (...) { return false; }
}

void SetIgnoreRecruitmentLevel(bool enabled) {
    if (!g_config.Update(g_ignoreLevel, enabled)) {
        Log("Could not save mods/cfg/Uruks.ini; setting unchanged");
        return;
    }
    Log(enabled ? "Ignore Recruitment Level enabled" : "Ignore Recruitment Level disabled");
}
}
