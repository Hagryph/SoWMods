#include "PCH.h"
#include "Config.h"

namespace hag {

namespace {
std::wstring IniPath() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? (std::wstring(base, n) + L"\\SoWLoader")
                                               : std::wstring(L".");
    ::CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\config";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\HagIPC.ini";
}
}  // namespace

Config Config::Load() {
    const std::wstring ini = IniPath();

    if (::GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = ::CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const char* def =
                "; HagIPC - local debug/IPC server for Shadow of War. DEV TOOL.\r\n"
                "; Exposes memory read/write, function call, and code-exec to a localhost-only TCP\r\n"
                "; client. Treat it like an attached debugger: do NOT enable it on an untrusted machine\r\n"
                "; and do NOT ship it enabled to other users.\r\n"
                "\r\n[General]\r\n"
                "; turn the server off without removing the DLL\r\n"
                "enabled=1\r\n"
                "; localhost TCP port the client connects to (127.0.0.1 only)\r\n"
                "port=19000\r\n"
                "; optional shared secret; if set, the client's first line must be: auth <token>\r\n"
                "token=\r\n";
            DWORD w = 0; ::WriteFile(h, def, static_cast<DWORD>(::strlen(def)), &w, nullptr);
            ::CloseHandle(h);
        }
    }

    Config c;
    c.enabled = ::GetPrivateProfileIntW(L"General", L"enabled", 1, ini.c_str()) != 0;
    c.port    = static_cast<std::uint16_t>(::GetPrivateProfileIntW(L"General", L"port", 19000, ini.c_str()));

    wchar_t tok[256]{};
    ::GetPrivateProfileStringW(L"General", L"token", L"", tok, 256, ini.c_str());
    char narrow[256]{};
    ::WideCharToMultiByte(CP_UTF8, 0, tok, -1, narrow, 256, nullptr, nullptr);
    c.token = narrow;
    return c;
}

}  // namespace hag
