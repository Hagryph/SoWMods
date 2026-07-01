#pragma once
#include "PCH.h"

namespace sow {

// The mod loader itself. For this first milestone it only PROVES the injection point:
// it logs that our proxy DLL was loaded into ShadowOfWar.exe and records basic process facts.
// Later it will enumerate and LoadLibrary the mod DLLs (done off a spawned thread, never in
// DllMain, to stay out of the loader lock).
class Loader {
public:
    static Loader& Get();

    // Called from DllMain(DLL_PROCESS_ATTACH). Must be loader-lock safe:
    // Win32-only work here (logging); anything heavier is deferred to a worker thread.
    void OnAttach(HMODULE self);

    // Called from DllMain(DLL_PROCESS_DETACH).
    void OnDetach();

    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

private:
    Loader() = default;

    static DWORD WINAPI WorkerThunk(LPVOID param);
    void Worker();

    HMODULE self_ = nullptr;
    HANDLE  thread_ = nullptr;
};

}  // namespace sow
