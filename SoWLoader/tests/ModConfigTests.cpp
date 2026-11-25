#include "ModConfigService.h"
#include <filesystem>
#include <iostream>
#include <limits>

void Check(bool condition) { if (!condition) throw std::runtime_error("Config test failed"); }
int main() {
    try {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        auto root = std::filesystem::path(temp) / (L"SoWConfigTests-" + std::to_wstring(GetCurrentProcessId()));
        auto api = sow::ModConfigAPI();
        sow::BindModConfig(1, (root / "First.dll").wstring());
        sow::BindModConfig(2, (root / "Second.dll").wstring());
        sow::BindModConfig(3, (root / "Unused.dll").wstring());
        Check(!std::filesystem::exists(root / "cfg"));
        sowmod::Config first(1, api), second(2, api);
        auto enabled = first.Register<bool>("General", "Enabled", false);
        Check(std::filesystem::exists(root / "cfg/First.ini"));
        Check(!std::filesystem::exists(root / "cfg/Second.ini"));
        Check(!std::filesystem::exists(root / "cfg/Unused.ini"));
        Check(!first.Read(enabled));
        Check(first.Update(enabled, true));
        Check(first.Read(enabled));
        Check(first.Read(first.Register<bool>("general", "enabled", false)));
        bool rejected = false;
        try { first.Register<int64_t>("General", "Enabled", 1); } catch (...) { rejected = true; }
        Check(rejected);
        rejected = false;
        try { first.Read(sowmod::Option<bool>{}); } catch (...) { rejected = true; }
        Check(rejected);
        Check(!api->Register(999, "General", "Unknown", sowmod::ConfigType::Boolean, "0"));
        Check(!api->Register(1, "../bad", "Unknown", sowmod::ConfigType::Boolean, "0"));
        auto id = api->Register(1, "General", "Enabled", sowmod::ConfigType::Boolean, "0");
        char buffer[32];
        Check(!api->Read(id, sowmod::ConfigType::Integer, buffer, sizeof(buffer)));
        Check(!api->Read(id, sowmod::ConfigType::Boolean, buffer, 1));
        Check(!api->Update(id, sowmod::ConfigType::Boolean, "garbage"));
        Check(!api->Update(0, sowmod::ConfigType::Boolean, "1"));
        auto independent = second.Register<bool>("General", "Enabled", false);
        Check(!second.Read(independent));
        Check(!second.Update(enabled, false));
        sow::BindModConfig(4, (root / "First.dll").wstring());
        sowmod::Config reload(4, api);
        Check(reload.Read(reload.Register<bool>("General", "Enabled", false)));
        auto number = first.Register<int64_t>("Types", "Integer", -123);
        Check(first.Read(number) == -123);
        auto real = first.Register<double>("Types", "Real", 1.23456789012345);
        Check(first.Read(real) == 1.23456789012345);
        Check(!first.Update(real, std::numeric_limits<double>::infinity()));
        const std::string text = "  \"quoted\"\nUTF8: \xc3\xa4  ";
        auto string = first.Register<std::string>("Types", "String", text);
        Check(first.Read(string) == text);
        auto plain = first.Register<std::string>("Types", "Plain", "hello");
        Check(first.Read(plain) == "hello");
        Check(first.Update(plain, std::string("")));
        auto unicode = first.Register<std::string>("Types", "Unicode", "\xe7\x8e\xaf");
        Check(first.Read(unicode) == "\xe7\x8e\xaf");
        sow::BindModConfig(5, (root / "First.dll").wstring());
        sowmod::Config reloadText(5, api);
        Check(reloadText.Read(reloadText.Register<std::string>("Types", "String", "")) == text);
        Check(reloadText.Read(reloadText.Register<std::string>("Types", "Plain", "fallback")).empty());
        Check(reloadText.Read(reloadText.Register<std::string>("Types", "Unicode", "fallback")) == "\xe7\x8e\xaf");
        WritePrivateProfileStringW(L"Invalid", L"Bool", L"invalid", (root / "cfg/First.ini").c_str());
        Check(first.Read(first.Register<bool>("Invalid", "Bool", true)));
        auto path = root / "cfg/First.ini";
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
        Check(!first.Update(enabled, false));
        Check(first.Read(enabled));
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        // Remove only this process's uniquely named test directory.
        std::filesystem::remove_all(root);
        std::cout << "Loader config tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
