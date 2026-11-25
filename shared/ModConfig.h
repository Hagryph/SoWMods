#pragma once
#include <Windows.h>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace sowmod {
enum class ConfigType : uint32_t { Boolean, Integer, Real, String };
struct ConfigAPI {
    uint32_t version;
    uint64_t (*Register)(int page, const char* section, const char* key, ConfigType type, const char* defaultValue);
    bool (*Read)(uint64_t option, ConfigType type, char* value, uint32_t capacity);
    bool (*Update)(uint64_t option, ConfigType type, const char* value);
};
using GetConfigAPI = const ConfigAPI* (*)(uint32_t);

template<class T> constexpr ConfigType Type() {
    if constexpr (std::is_same_v<T, bool>) return ConfigType::Boolean;
    else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, int64_t>) return ConfigType::Integer;
    else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) return ConfigType::Real;
    else { static_assert(std::is_same_v<T, std::string>, "Use bool, int, int64_t, float, double or std::string"); return ConfigType::String; }
}
template<class T> class Option {
    friend class Config;
    uint64_t id_ = 0;
    int page_ = -1;
    Option(uint64_t id, int page) : id_(id), page_(page) {}
public:
    Option() = default;
};

// No persistence, registry, or STL objects cross the DLL boundary.
class Config {
    const ConfigAPI* api_ = nullptr;
    int page_ = -1;
    template<class T> static std::string Encode(const T& value) {
        (void)Type<T>();
        if constexpr (std::is_same_v<T, std::string>) {
            if (value.find('\0') != std::string::npos) throw std::runtime_error("Config strings cannot contain NUL");
            return value;
        }
        else if constexpr (std::is_same_v<T, bool>) return value ? "1" : "0";
        else {
            char buffer[128];
            auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (result.ec != std::errc{}) throw std::runtime_error("Invalid config value");
            return {buffer, result.ptr};
        }
    }
public:
    Config() = default;
    explicit Config(int page) : page_(page) {
        auto module = ::GetModuleHandleW(L"steam_api64.dll");
        auto get = module ? reinterpret_cast<GetConfigAPI>(::GetProcAddress(module, "SoWConfig_GetAPI")) : nullptr;
        api_ = get ? get(1) : nullptr;
    }
    Config(int page, const ConfigAPI* api) : api_(api), page_(page) {}
    template<class T> Option<T> Register(const char* section, const char* key, const T& defaultValue) {
        auto text = Encode(defaultValue);
        auto id = api_ ? api_->Register(page_, section, key, Type<T>(), text.c_str()) : 0;
        if (!id) throw std::runtime_error("Config registration failed");
        return Option<T>(id, page_);
    }
    template<class T> T Read(Option<T> option) const {
        char buffer[4096]{};
        if (!api_ || option.page_ != page_ || !api_->Read(option.id_, Type<T>(), buffer, sizeof(buffer)))
            throw std::runtime_error("Config read failed");
        if constexpr (std::is_same_v<T, std::string>) return buffer;
        else if constexpr (std::is_same_v<T, bool>) return buffer[0] == '1';
        else {
            T value{};
            auto end = buffer + std::char_traits<char>::length(buffer);
            auto result = std::from_chars(buffer, end, value);
            if (result.ec != std::errc{} || result.ptr != end) throw std::runtime_error("Invalid config value");
            return value;
        }
    }
    template<class T> bool Update(Option<T> option, const T& value) const {
        auto text = Encode(value);
        return api_ && option.page_ == page_ && api_->Update(option.id_, Type<T>(), text.c_str());
    }
};
}
