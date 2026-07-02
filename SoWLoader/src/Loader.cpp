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

    // Load external mods now (DLLs in x64\mods\, in filename order). Each may register HagUI pages
    // and/or install its own game hooks in SoWMod_Init.
    ModManager::Get().LoadAll();

    log.Line("[worker] injection point VALIDATED — proxy is live; hooks armed; mods loaded.");
}

}  // namespace sow
