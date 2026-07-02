#include "PCH.h"
#include "Log.h"
#include "Config.h"
#include "Offsets.h"
#include "ipc/Server.h"

// HagIPC is a STANDALONE debug DLL, not a SoWLoader proxy. It is meant to be hot-injected into the
// already-running ShadowOfWar.exe (tools/inject.ps1 does LoadLibrary via CreateRemoteThread), so we
// can drive the live game by RVA without paying the Denuvo boot each iteration. It can also be
// auto-loaded by SoWLoader's mods folder on a normal launch.
//
// All server work runs on a spawned worker (never in DllMain / loader lock).

namespace {

DWORD WINAPI Worker(LPVOID) {
    hag::Log::Init();
    const auto cfg = hag::Config::Load();
    char base[32]{}; ::wsprintfA(base, "0x%p", reinterpret_cast<void*>(hag::offsets::Base()));
    hag::Log::Line(std::string("HagIPC injected. image base=") + base);

    if (!cfg.enabled) {
        hag::Log::Line("HagIPC disabled in config (enabled=0); server not started.");
        return 0;
    }
    hag::Log::Line("starting server on 127.0.0.1:" + std::to_string(cfg.port) +
                   (cfg.token.empty() ? " (no auth)" : " (auth required)"));
    hag::ipc::Server::Get().Start(cfg.port, cfg.token);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        ::DisableThreadLibraryCalls(mod);
        if (HANDLE t = ::CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr)) ::CloseHandle(t);
        break;
    case DLL_PROCESS_DETACH:
        hag::ipc::Server::Get().Stop();
        break;
    default:
        break;
    }
    return TRUE;
}
