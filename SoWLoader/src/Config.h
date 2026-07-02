#pragma once
#include "PCH.h"
#include <string>

namespace sow {

// The config framework. Every mod gets one .ini in the shared folder
// %LOCALAPPDATA%\SoWLoader\config\<Mod>.ini, all with the same base architecture: [Section]
// key=value, `[General]` as the default section. Getters WRITE THE DEFAULT BACK when a key is
// missing, so every ini self-populates with its mod's full option set; setters write through
// immediately. Launch-arg overrides stay the caller's job (see Loader for -sowsilent).
class Config {
public:
    static Config& For(const std::string& mod);

    bool        GetBool  (const char* sec, const char* key, bool def);
    int         GetInt   (const char* sec, const char* key, int def);
    float       GetFloat (const char* sec, const char* key, float def);
    std::string GetString(const char* sec, const char* key, const std::string& def);

    void SetBool  (const char* sec, const char* key, bool v);
    void SetInt   (const char* sec, const char* key, int v);
    void SetFloat (const char* sec, const char* key, float v);
    void SetString(const char* sec, const char* key, const std::string& v);

    const std::wstring& Path() const { return path_; }

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

private:
    explicit Config(const std::string& mod);
    std::string  ReadRaw(const char* sec, const char* key);          // "" if absent
    void         WriteRaw(const char* sec, const char* key, const std::string& v);

    std::wstring path_;
};

}  // namespace sow
