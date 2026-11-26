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
sowmod::Option<bool> g_freeGarrison;
}

static HANDLE OpenLog(const wchar_t* filename) {
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return INVALID_HANDLE_VALUE;
    const std::wstring root = std::wstring(base, n) + L"\\SoWLoader";
    ::CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring logs = root + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    return ::CreateFileW((logs + L"\\" + filename).c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
bool WriteGenerationLog(const char* report) {
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    char date[64]{};
    std::snprintf(date,sizeof(date),"\r\n[%04u-%02u-%02u %02u:%02u:%02u] Save Uruk\r\n",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    std::string block=std::string(date)+report;
    HANDLE file=OpenLog(L"Uruks-generation.log");
    if (file==INVALID_HANDLE_VALUE) return false;
    DWORD written=0;
    bool ok=::WriteFile(file,block.data(),DWORD(block.size()),&written,nullptr) && written==block.size();
    ::CloseHandle(file);return ok;
}
void Log(const char* message) {
    ::OutputDebugStringA("[Uruks] ");
    ::OutputDebugStringA(message);
    ::OutputDebugStringA("\n");
    HANDLE file=OpenLog(L"Uruks.log");
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
        g_freeGarrison = g_config.Register<bool>("Garrison", "FreeGarrisonPlacement", false);
        Log(IgnoreRecruitmentLevel() ? "Loader config loaded: recruitment bypass enabled" : "Loader config loaded: recruitment bypass disabled");
        return true;
    } catch (...) { return false; }
}

bool IgnoreRecruitmentLevel() {
    try { return g_config.Read(g_ignoreLevel); }
    catch (...) { return false; }
}

bool FreeGarrisonPlacement() {
    try { return g_config.Read(g_freeGarrison); } catch (...) { return false; }
}
bool SaveFreeGarrisonPlacement(bool enabled) { return g_config.Update(g_freeGarrison, enabled); }

void SetIgnoreRecruitmentLevel(bool enabled) {
    if (!g_config.Update(g_ignoreLevel, enabled)) {
        Log("Could not save mods/cfg/Uruks.ini; setting unchanged");
        return;
    }
    Log(enabled ? "Ignore Recruitment Level enabled" : "Ignore Recruitment Level disabled");
}
}
