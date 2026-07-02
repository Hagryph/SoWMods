#include "ModManager.h"
#include "Log.h"
#include "SoWModAPI.h"   // ../shared (on the include path)

#include <string>
#include <vector>
#include <algorithm>

namespace sow {

ModManager& ModManager::Get() { static ModManager m; return m; }

// mods\ next to our DLL:  x64\steam_api64.dll  ->  x64\mods\ .
static std::wstring ModsDir() {
    wchar_t path[MAX_PATH]{};
    HMODULE self = ::GetModuleHandleW(L"steam_api64.dll");
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    const size_t slash = p.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? std::wstring(L".") : p.substr(0, slash);
    return dir + L"\\mods";
}

static std::string Narrow(const std::wstring& w) {
    char b[MAX_PATH]{}; ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, MAX_PATH, nullptr, nullptr);
    return b;
}

void ModManager::LoadAll() {
    if (loaded_) return;
    loaded_ = true;
    auto& log = Log::Get();

    const std::wstring dir = ModsDir();
    ::CreateDirectoryW(dir.c_str(), nullptr);   // make the drop-folder if absent, so it's discoverable

    // Enumerate *.dll, sorted by filename => deterministic load order (prefix 01_/02_ to control it).
    std::vector<std::wstring> dlls;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) dlls.emplace_back(fd.cFileName);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::sort(dlls.begin(), dlls.end());

    const std::string ndir = Narrow(dir);
    if (dlls.empty()) { log.Line("[mods] no mod DLLs in " + ndir + " (drop *.dll there)"); return; }
    { char l[160]; ::wsprintfA(l, "[mods] loading %d mod(s) from %s (filename order):", (int)dlls.size(), ndir.c_str()); log.Good(l); }

    int idx = 0;
    for (const auto& name : dlls) {
        ++idx;
        const std::wstring full = dir + L"\\" + name;
        const std::string nn = Narrow(name);
        HMODULE mod = ::LoadLibraryW(full.c_str());   // runs the mod's DllMain under the loader lock
        if (!mod) {
            char l[220]; ::wsprintfA(l, "[mods]  %2d. %-36s FAILED (LoadLibrary error %lu)", idx, nn.c_str(), ::GetLastError());
            log.Error(l);
            continue;
        }
        auto init = reinterpret_cast<SoWMod_Init_t>(::GetProcAddress(mod, "SoWMod_Init"));
        char l[240];
        ::wsprintfA(l, "[mods]  %2d. %-36s loaded @ %p%s", idx, nn.c_str(), (void*)mod,
                    init ? "  -> SoWMod_Init()" : "  (no SoWMod_Init)");
        log.Good(l);
        if (init) init();   // outside the loader lock now (LoadLibrary returned) -> safe to do real work
    }
    log.Line("[mods] done");
}

}  // namespace sow
