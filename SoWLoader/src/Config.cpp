#include "Config.h"

#include <map>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace sow {

namespace {
struct Registry {
    CRITICAL_SECTION cs;
    std::map<std::string, Config*> map;
    Registry() { ::InitializeCriticalSection(&cs); }
};
Registry& Reg() { static Registry r; return r; }

std::wstring ConfigDir() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? (std::wstring(base, n) + L"\\SoWLoader")
                                               : std::wstring(L".");
    ::CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\config";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Widen(const std::string& s) {
    wchar_t buf[512]{};
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf, 512);
    return buf;
}
std::string Narrow(const wchar_t* s) {
    char buf[512]{};
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, 512, nullptr, nullptr);
    return buf;
}
}  // namespace

Config& Config::For(const std::string& mod) {
    auto& r = Reg();
    ::EnterCriticalSection(&r.cs);
    auto it = r.map.find(mod);
    if (it == r.map.end()) it = r.map.emplace(mod, new Config(mod)).first;
    Config& c = *it->second;
    ::LeaveCriticalSection(&r.cs);
    return c;
}

Config::Config(const std::string& mod) {
    path_ = ConfigDir() + L"\\" + Widen(mod) + L".ini";
    // First run: create the file with a shared header so every mod's ini looks the same.
    if (::GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = ::CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const std::string hdr =
                "; " + mod + " configuration (SoWLoader config framework)\r\n"
                "; Keys appear here with their defaults the first time the mod reads them.\r\n"
                "\r\n[General]\r\n";
            DWORD w = 0; ::WriteFile(h, hdr.c_str(), (DWORD)hdr.size(), &w, nullptr);
            ::CloseHandle(h);
        }
    }
}

std::string Config::ReadRaw(const char* sec, const char* key) {
    wchar_t buf[512]{};
    ::GetPrivateProfileStringW(Widen(sec).c_str(), Widen(key).c_str(), L"\x1",
                               buf, 512, path_.c_str());
    if (buf[0] == L'\x1' && buf[1] == 0) return {};   // sentinel = key absent
    return Narrow(buf);
}
void Config::WriteRaw(const char* sec, const char* key, const std::string& v) {
    ::WritePrivateProfileStringW(Widen(sec).c_str(), Widen(key).c_str(),
                                 Widen(v).c_str(), path_.c_str());
}

bool Config::GetBool(const char* sec, const char* key, bool def) {
    const std::string raw = ReadRaw(sec, key);
    if (raw.empty()) { SetBool(sec, key, def); return def; }
    return raw != "0" && raw != "false" && raw != "off";
}
int Config::GetInt(const char* sec, const char* key, int def) {
    const std::string raw = ReadRaw(sec, key);
    if (raw.empty()) { SetInt(sec, key, def); return def; }
    return ::atoi(raw.c_str());
}
float Config::GetFloat(const char* sec, const char* key, float def) {
    const std::string raw = ReadRaw(sec, key);
    if (raw.empty()) { SetFloat(sec, key, def); return def; }
    return (float)::atof(raw.c_str());
}
std::string Config::GetString(const char* sec, const char* key, const std::string& def) {
    const std::string raw = ReadRaw(sec, key);
    if (raw.empty()) { SetString(sec, key, def); return def; }
    return raw;
}

void Config::SetBool(const char* sec, const char* key, bool v)   { WriteRaw(sec, key, v ? "1" : "0"); }
void Config::SetInt(const char* sec, const char* key, int v)     { char b[32]; ::sprintf_s(b, "%d", v); WriteRaw(sec, key, b); }
void Config::SetFloat(const char* sec, const char* key, float v) { char b[64]; ::sprintf_s(b, "%.6g", v); WriteRaw(sec, key, b); }
void Config::SetString(const char* sec, const char* key, const std::string& v) { WriteRaw(sec, key, v); }

}  // namespace sow
