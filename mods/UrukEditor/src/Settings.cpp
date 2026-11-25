#include "Settings.h"
#include <atomic>
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <string>

namespace uruk {
namespace {
std::wstring g_path;
std::atomic<bool> g_ignoreLevel{false};
}

void Log(const char* message) {
    ::OutputDebugStringA("[UrukEditor] ");
    ::OutputDebugStringA(message);
    ::OutputDebugStringA("\n");
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const std::wstring root = std::wstring(base, n) + L"\\SoWLoader";
    ::CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring logs = root + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    HANDLE file = ::CreateFileW((logs + L"\\UrukEditor.log").c_str(), FILE_APPEND_DATA,
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

bool LoadSettings(HMODULE module) {
    wchar_t path[32768]{};
    const DWORD count = ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!count || count >= std::size(path)) return false;
    const std::wstring full(path, count);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    const auto folder = full.substr(0, slash) + L"\\cfg";
    if (!::CreateDirectoryW(folder.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
    g_path = folder + L"\\UrukEditor.ini";
    if (::GetFileAttributesW(g_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!::WritePrivateProfileStringW(L"Recruitment", L"IgnoreRecruitmentLevel", L"0", g_path.c_str())) return false;
    }
    g_ignoreLevel.store(::GetPrivateProfileIntW(L"Recruitment", L"IgnoreRecruitmentLevel", 0, g_path.c_str()) == 1,
                        std::memory_order_release);
    Log(g_ignoreLevel.load() ? "Ignore Recruitment Level loaded: enabled" : "Ignore Recruitment Level loaded: disabled");
    return true;
}

bool IgnoreRecruitmentLevel() { return g_ignoreLevel.load(std::memory_order_acquire); }

void SetIgnoreRecruitmentLevel(bool enabled) {
    if (g_path.empty() || !::WritePrivateProfileStringW(L"Recruitment", L"IgnoreRecruitmentLevel",
        enabled ? L"1" : L"0", g_path.c_str())) {
        Log("Could not save mods/cfg/UrukEditor.ini; setting unchanged");
        return;
    }
    g_ignoreLevel.store(enabled, std::memory_order_release);
    Log(enabled ? "Ignore Recruitment Level enabled" : "Ignore Recruitment Level disabled");
}
}
