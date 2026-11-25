#include "ModConfigService.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

namespace sow {
namespace {
struct Entry {
    int page;
    std::string section, key, value, defaultValue;
    sowmod::ConfigType type;
};
std::mutex mutex;
std::map<int, std::wstring> paths;
std::map<uint64_t, Entry> entries;
uint64_t nextId = 1;

bool Name(const char* name) {
    if (!name || !*name || std::strlen(name) > 128) return false;
    for (; *name; ++name) if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') ||
        (*name >= '0' && *name <= '9') || *name == '_' || *name == '-' || *name == '.')) return false;
    return true;
}
bool Normalize(sowmod::ConfigType type, const char* input, std::string& output) {
    if (!input || std::strlen(input) >= 4096) return false;
    output = input;
    switch (type) {
    case sowmod::ConfigType::String: return true;
    case sowmod::ConfigType::Boolean:
        if (output == "true") output = "1";
        if (output == "false") output = "0";
        return output == "0" || output == "1";
    case sowmod::ConfigType::Integer: {
        int64_t value;
        auto r = std::from_chars(input, input + output.size(), value);
        return r.ec == std::errc{} && r.ptr == input + output.size();
    }
    case sowmod::ConfigType::Real: {
        double value;
        auto r = std::from_chars(input, input + output.size(), value);
        return r.ec == std::errc{} && r.ptr == input + output.size() && std::isfinite(value);
    }
    default: return false;
    }
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 config value");
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
// Hex protects arbitrary UTF-8 strings from INI quoting, whitespace and newline rules.
std::string Pack(const Entry& entry, const std::string& value) {
    if (entry.type != sowmod::ConfigType::String) return value;
    if (value.empty() || (value.front() != ' ' && value.back() != ' ' &&
        value.find_first_of("\"'") == std::string::npos &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c >= 127; }) &&
        value.compare(0, 6, "@utf8:") != 0)) return value;
    std::string result = "@utf8:";
    for (unsigned char c : value) { result += "0123456789abcdef"[c >> 4]; result += "0123456789abcdef"[c & 15]; }
    return result;
}
bool Unpack(const Entry& entry, std::string& value) {
    if (entry.type != sowmod::ConfigType::String) return true;
    if (value.compare(0, 6, "@utf8:") != 0) return true;
    if (value.size() % 2) return false;
    std::string result;
    for (size_t i = 6; i < value.size(); i += 2) {
        unsigned int byte = 0;
        auto r = std::from_chars(value.data() + i, value.data() + i + 2, byte, 16);
        if (r.ec != std::errc{} || r.ptr != value.data() + i + 2 || byte == 0) return false;
        result += static_cast<char>(byte);
    }
    value = std::move(result);
    return true;
}
bool Persist(const Entry& entry, const std::string& value) {
    const auto& path = paths.at(entry.page);
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    if (error) return false;
    return ::WritePrivateProfileStringW(Wide(entry.section).c_str(), Wide(entry.key).c_str(),
        Wide(Pack(entry, value)).c_str(), path.c_str()) != FALSE;
}
uint64_t Register(int page, const char* section, const char* key, sowmod::ConfigType type, const char* def) {
    try {
        std::lock_guard lock(mutex);
        std::string value;
        if (!paths.count(page) || !Name(section) || !Name(key) || !Normalize(type, def, value)) return 0;
        for (const auto& [id, e] : entries)
            if (e.page == page && _stricmp(e.section.c_str(), section) == 0 && _stricmp(e.key.c_str(), key) == 0)
                return e.type == type && e.defaultValue == value ? id : 0;
        Entry entry{page, section, key, value, value, type};
        wchar_t buffer[8198]{};
        DWORD count = ::GetPrivateProfileStringW(Wide(section).c_str(), Wide(key).c_str(), L"\x1",
            buffer, 8198, paths.at(page).c_str());
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
        std::string raw(bytes, 0);
        if (bytes) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, static_cast<int>(count), raw.data(), bytes, nullptr, nullptr);
        const bool missing = count == 1 && buffer[0] == 1;
        if (!missing && count < 8197 && (bytes || count == 0) && Unpack(entry, raw) && Normalize(type, raw.c_str(), value)) entry.value = value;
        else if (!Persist(entry, entry.value)) return 0;
        const auto id = nextId++;
        entries.emplace(id, std::move(entry));
        return id;
    } catch (...) { return 0; }
}
bool Read(uint64_t id, sowmod::ConfigType type, char* value, uint32_t capacity) {
    try {
        std::lock_guard lock(mutex);
        auto it = entries.find(id);
        if (it == entries.end() || it->second.type != type || !value || capacity <= it->second.value.size()) return false;
        std::memcpy(value, it->second.value.c_str(), it->second.value.size() + 1);
        return true;
    } catch (...) { return false; }
}
bool Update(uint64_t id, sowmod::ConfigType type, const char* input) {
    try {
        std::lock_guard lock(mutex);
        auto it = entries.find(id);
        std::string value;
        if (it == entries.end() || it->second.type != type || !Normalize(type, input, value)) return false;
        if (!Persist(it->second, value)) return false;
        it->second.value = std::move(value);
        return true;
    } catch (...) { return false; }
}
const sowmod::ConfigAPI api{1, Register, Read, Update};
}
void BindModConfig(int page, const std::wstring& dllPath) {
    std::lock_guard lock(mutex);
    auto path = std::filesystem::path(dllPath);
    paths.try_emplace(page, (path.parent_path() / L"cfg" / path.filename().replace_extension(L".ini")).wstring());
}
const sowmod::ConfigAPI* ModConfigAPI() { return &api; }
}
extern "C" __declspec(dllexport) const sowmod::ConfigAPI* SoWConfig_GetAPI(uint32_t version) {
    return version == 1 ? sow::ModConfigAPI() : nullptr;
}
