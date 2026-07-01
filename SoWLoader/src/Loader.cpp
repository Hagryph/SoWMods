#include "Loader.h"
#include "Log.h"
#include "GameHooks.h"

namespace sow {

Loader& Loader::Get() {
    static Loader instance;
    return instance;
}

void Loader::OnAttach(HMODULE self) {
    self_ = self;

    // Loader-lock safe: Win32 logging only.
    auto& log = Log::Get();
    log.Line("==================================================");
    log.Line("SoWLoader proxy (steam_api64.dll) attached.");

    wchar_t exePath[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    char narrow[MAX_PATH]{};
    ::WideCharToMultiByte(CP_UTF8, 0, exePath, -1, narrow, MAX_PATH, nullptr, nullptr);
    log.Line(std::string("host exe : ") + narrow);
    log.Line(std::string("proxy base : 0x") + [] {
        char b[32]{}; ::wsprintfA(b, "%p", (void*)::GetModuleHandleW(L"steam_api64.dll")); return std::string(b);
    }());

    // Confirm the renamed original is present + resolvable (forwarders depend on it).
    HMODULE orig = ::GetModuleHandleW(L"steam_api64_org.dll");
    log.Line(std::string("steam_api64_org.dll loaded? ") + (orig ? "yes" : "not yet"));

    // Defer any heavier work (mod enumeration/LoadLibrary) to a worker thread — never in DllMain.
    thread_ = ::CreateThread(nullptr, 0, &Loader::WorkerThunk, this, 0, nullptr);
}

void Loader::OnDetach() {
    Log::Get().Line("SoWLoader proxy detaching.");
}

DWORD WINAPI Loader::WorkerThunk(LPVOID param) {
    static_cast<Loader*>(param)->Worker();
    return 0;
}

void Loader::Worker() {
    auto& log = Log::Get();
    log.Line("[worker] injection point VALIDATED — proxy is live in the game process.");

    // Game-driven init: hook the front-end menu's root-UI-layer constructor so the overlay
    // initializes exactly when the start menu is displayed. The static target exists from module
    // load and the menu appears seconds later, so hooking it now is safe.
    log.Line("[worker] installing start-menu (CUIFrontEndRootLayer ctor) trigger ...");
    GameHooks::Get().Install();

    // Last-resort safety net only: the CUIFrontEndRootLayer ctor is the real trigger and fires
    // ~33s in (start-menu build), so this long timeout lets the ctor win under normal conditions;
    // the fallback installs the overlay only if the menu never appears (e.g. a very slow load).
    ::Sleep(90000);
    GameHooks::Get().InstallOverlayFallback();
    // TODO(next milestone): enumerate SoWLoader\\mods\\*.dll and LoadLibrary each here.
}

}  // namespace sow
