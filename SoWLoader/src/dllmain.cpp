#include "PCH.h"
#include "Loader.h"
#include "exports_forward.h"   // 796 /EXPORT: forwarders -> steam_api64_org.dll

// The only free function in the project — the unavoidable ABI entry point — which
// immediately delegates to the Loader class (per the workspace's full-OOP convention).
// All 796 Steam exports are forwarded to steam_api64_org.dll via exports_forward.h,
// so this DLL implements none of them; it exists purely to get our code into the process.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        ::DisableThreadLibraryCalls(hModule);
        sow::Loader::Get().OnAttach(hModule);
        break;
    case DLL_PROCESS_DETACH:
        sow::Loader::Get().OnDetach();
        break;
    default:
        break;
    }
    return TRUE;
}
