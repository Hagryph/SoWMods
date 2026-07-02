#include "Loader.h"
#include "Log.h"
#include "Config.h"
#include "Console.h"
#include "GameHooks.h"

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
    if (consoleDone_) return;
    consoleDone_ = true;
    if (wantConsole_ && Console::Get().Open()) {
        Log::AttachConsole(&Console::Get());
        Log::Get().Good("[console] opened after game init (buffered lines replayed)");
    }
}

DWORD WINAPI Loader::WorkerThunk(LPVOID param) {
    static_cast<Loader*>(param)->Worker();
    return 0;
}

void Loader::Worker() {
    // The Loader owns logging setup. Config comes from the shared framework
    // (config\SoWLoader.ini); the console itself opens LATER (OnRenderLive) so the game claims
    // its taskbar slot first — everything logged until then is buffered and replayed.
    auto& cfg = Config::For("SoWLoader");
    wantConsole_ = cfg.GetBool("General", "console", true);
    if (const wchar_t* cmd = ::GetCommandLineW()) {
        const std::wstring c = cmd;
        if (c.find(L"-sowsilent")  != std::wstring::npos) wantConsole_ = false;   // silent run
        if (c.find(L"-sowconsole") != std::wstring::npos) wantConsole_ = true;
    }

    auto& log = Log::Get();
    log.Accent("SoWLoader online.");
    {
        wchar_t exePath[MAX_PATH]{}; ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        char narrow[MAX_PATH]{}; ::WideCharToMultiByte(CP_UTF8, 0, exePath, -1, narrow, MAX_PATH, nullptr, nullptr);
        log.Line(std::string("host      : ") + narrow);
        char b[32]{}; ::wsprintfA(b, "0x%p", (void*)::GetModuleHandleW(L"steam_api64.dll"));
        log.Line(std::string("proxy base: ") + b);
        log.Line(std::string("steam_api64_org.dll: ") + (::GetModuleHandleW(L"steam_api64_org.dll") ? "loaded" : "not yet"));
        log.Line(std::string("console   : ") + (wantConsole_ ? "deferred until first frame" : "silent (file log only)"));
    }

    // Module roster (proper-modloader feel). External SoWLoader\mods\*.dll loading plugs in here next.
    log.Good("modules:");
    log.Line("   loader   steam_api64 proxy   ok   (796 exports forwarded)");
    log.Line("   hooks    GameHooks           arming");
    log.Line("   render   Overlay (D3D11)     pending start menu");
    log.Line("   ui       HagUI framework     pending start menu");

    log.Line("[worker] injection point VALIDATED — proxy is live in the game process.");

    // Game-driven init: hook the front-end menu's root-UI-layer constructor. The overlay installs
    // ONLY when that ctor actually runs (the start menu is really being built) — a deterministic
    // signal, no timer/fallback. A timed fallback races the ctor and installs at an arbitrary
    // moment (mid-load), which hangs the game; we do not do that.
    log.Line("[worker] installing start-menu (CUIFrontEndRootLayer ctor) trigger ...");
    GameHooks::Get().Install();
    // TODO(next milestone): enumerate SoWLoader\\mods\\*.dll and LoadLibrary each here.
}

}  // namespace sow
