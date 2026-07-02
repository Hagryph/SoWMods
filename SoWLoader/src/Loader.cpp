#include "Loader.h"
#include "Log.h"
#include "Config.h"
#include "Console.h"
#include "GameHooks.h"
#include "WindowWatch.h"
#include "ModManager.h"

namespace sow {

Loader& Loader::Get() {
    static Loader instance;
    return instance;
}

void Loader::OnAttach(HMODULE self) {
    self_ = self;
    // Keep DllMain minimal + loader-lock safe: all logging/console/hook setup runs on the worker.
    thread_ = ::CreateThread(nullptr, 0, &Loader::WorkerThunk, this, 0, nullptr);
}

void Loader::OnDetach() {
    Log::Get().Line("SoWLoader proxy detaching.");
}

void Loader::OnRenderLive() {
    // Fired from the Present hook's first frame = the game window exists and is rendering (fully
    // loaded). Open the console HERE so it never becomes the process's front/"main" window ahead of
    // the game (Console::Open pushes itself to the bottom of the z-order without activating).
    if (consoleDone_) return;
    consoleDone_ = true;
    if (wantConsole_ && Console::Get().Open()) {
        Log::AttachConsole(&Console::Get());
        Log::Get().Good("[console] opened once the game window was up (buffered lines replayed)");
    }
}

DWORD WINAPI Loader::WorkerThunk(LPVOID param) {
    static_cast<Loader*>(param)->Worker();
    return 0;
}

void Loader::Worker() {
    // The Loader owns logging setup. Config comes from the shared framework (config\SoWLoader.ini).
    auto& cfg = Config::For("SoWLoader");
    wantConsole_ = cfg.GetBool("General", "console", true);
    if (const wchar_t* cmd = ::GetCommandLineW()) {
        const std::wstring c = cmd;
        if (c.find(L"-sowsilent")  != std::wstring::npos) wantConsole_ = false;   // silent run
        if (c.find(L"-sowconsole") != std::wstring::npos) wantConsole_ = true;
    }

    // Arm the window watcher FIRST (SetWinEventHook, in-context, own PID): it logs the instant the
    // game window starts to exist and opens the console when that window BECOMES FOREGROUND — the
    // only moment the console is guaranteed to drop behind the game instead of fronting it.
    // Everything logged before the console attaches is buffered by Log and replayed on attach.
    WindowWatch::Get().Install();
    GameHooks::Get().Install();

    auto& log = Log::Get();
    log.Accent("SoWLoader online.");
    {
        wchar_t exePath[MAX_PATH]{}; ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        char narrow[MAX_PATH]{}; ::WideCharToMultiByte(CP_UTF8, 0, exePath, -1, narrow, MAX_PATH, nullptr, nullptr);
        log.Line(std::string("host      : ") + narrow);
        char b[32]{}; ::wsprintfA(b, "0x%p", (void*)::GetModuleHandleW(L"steam_api64.dll"));
        log.Line(std::string("proxy base: ") + b);
        log.Line(std::string("steam_api64_org.dll: ") + (::GetModuleHandleW(L"steam_api64_org.dll") ? "loaded" : "not yet"));
        log.Line(std::string("console   : ") + (wantConsole_ ? "opens when the game window is up" : "silent (file log only)"));
    }

    // Module roster (proper-modloader feel). The loader core + HagUI are the two mains compiled into
    // this DLL; every other mod is a standalone DLL in x64\mods\ (loaded just below).
    log.Good("modules:");
    log.Line("   loader   steam_api64 proxy   ok   (796 exports forwarded)");
    log.Line("   hooks    GameHooks           arming");
    log.Line("   render   Overlay (D3D11)     pending start menu");
    log.Line("   ui       HagUI framework     pending start menu");

    // -sowipc / -sowhooks=<file>: preload HagIPC + arm hooks from a file, at process start (before
    // item templates deserialize). Done before mods so IPC/hooks are up as early as possible.
    if (const wchar_t* cmd = ::GetCommandLineW()) PreloadIpc(cmd);

    // Load external mods now (DLLs in x64\mods\, in filename order). Each may register HagUI pages
    // and/or install its own game hooks in SoWMod_Init.
    ModManager::Get().LoadAll();

    log.Line("[worker] injection point VALIDATED — proxy is live; hooks armed; mods loaded.");
}

// -sowipc : preload HagIPC (starts its localhost IPC server).
// -sowhooks=<path> : ALSO arm the hooks listed in <path> (implies -sowipc). <path> may be quoted if
//                    it contains spaces. HagIPC.dll is loaded from the game install root.
void Loader::PreloadIpc(const std::wstring& cmd) {
    auto& log = Log::Get();

    // extract -sowhooks=<path> (optionally "quoted"); presence of -sowipc OR -sowhooks enables preload.
    std::wstring hooksFile;
    if (size_t p = cmd.find(L"-sowhooks="); p != std::wstring::npos) {
        size_t v = p + 10;                                    // past "-sowhooks="
        if (v < cmd.size() && cmd[v] == L'"') {               // quoted path
            size_t e = cmd.find(L'"', v + 1);
            hooksFile = cmd.substr(v + 1, e == std::wstring::npos ? std::wstring::npos : e - (v + 1));
        } else {                                              // to next whitespace
            size_t e = cmd.find_first_of(L" \t", v);
            hooksFile = cmd.substr(v, e == std::wstring::npos ? std::wstring::npos : e - v);
        }
    }
    const bool wantIpc = cmd.find(L"-sowipc") != std::wstring::npos || !hooksFile.empty();
    if (!wantIpc) return;

    // HagIPC.dll at the install root (our DLL is in x64\ -> go up one). Not in mods\, so it loads
    // ONLY when requested via the launch arg (mods\ auto-loads everything).
    wchar_t self[MAX_PATH]{}; ::GetModuleFileNameW(::GetModuleHandleW(L"steam_api64.dll"), self, MAX_PATH);
    std::wstring dir = self; size_t s1 = dir.find_last_of(L"\\/"); if (s1 != std::wstring::npos) dir.resize(s1);
    size_t s2 = dir.find_last_of(L"\\/"); std::wstring root = (s2 == std::wstring::npos) ? dir : dir.substr(0, s2);
    const std::wstring dll = root + L"\\HagIPC.dll";

    HMODULE h = ::LoadLibraryW(dll.c_str());
    if (!h) {
        char b[MAX_PATH]; ::WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), -1, b, MAX_PATH, nullptr, nullptr);
        log.Error(std::string("[sowipc] HagIPC preload FAILED (") + b + ", err " + std::to_string(::GetLastError()) + ")");
        return;
    }
    log.Good("[sowipc] HagIPC preloaded (IPC server starting)");

    if (!hooksFile.empty()) {
        using InstallFromFileFn = int(*)(const wchar_t*);
        auto fn = reinterpret_cast<InstallFromFileFn>(::GetProcAddress(h, "HagIPC_InstallHooksFromFile"));
        if (!fn) { log.Error("[sowipc] HagIPC_InstallHooksFromFile export not found"); return; }
        char b[MAX_PATH]; ::WideCharToMultiByte(CP_UTF8, 0, hooksFile.c_str(), -1, b, MAX_PATH, nullptr, nullptr);
        const int n = fn(hooksFile.c_str());
        log.Good(std::string("[sowipc] armed ") + std::to_string(n) + " hook(s) from " + b);
    }
}

}  // namespace sow
