// HagIPCExport.cpp - a tiny exported C API so a host (the SoWLoader) can PRELOAD HagIPC and arm
// runtime hooks at PROCESS START, before the game deserializes its item templates. Without this the
// only way in is post-launch hot-injection (tools/inject.ps1), which is too late to catch load-time
// deserialization. Delegates to HookProbe (MinHook) + the shared offset table.
#include "PCH.h"
#include "ipc/HookProbe.h"
#include "Offsets.h"
#include "Log.h"

#include <string>
#include <fstream>

namespace {

// Parse a leading hex value ("0x..." or bare hex) from a token; false if no hex digits.
bool ParseHex(const std::string& s, unsigned long long& out) {
    std::size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    if (i >= s.size()) return false;
    out = 0; bool any = false;
    for (; i < s.size(); ++i) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        out = (out << 4) | (unsigned)d; any = true;
    }
    return any;
}

}  // namespace

// Install one logging hook at a FILE RVA (off 0x140000000). Returns 1 on success, -1 on failure.
extern "C" __declspec(dllexport) int HagIPC_InstallHook(unsigned long long rva) {
    const std::string r = hag::ipc::HookProbe::Get().Install(hag::offsets::FromRVA(static_cast<std::uintptr_t>(rva)));
    const bool ok = r.rfind("ok", 0) == 0;
    hag::Log::Line("[hooksfile] hook " + std::string(ok ? "" : "FAILED ") + r);
    return ok ? 1 : -1;
}

// Read a hook-definitions file and arm each hook. Format: one FILE RVA per line (off 0x140000000,
// e.g. 0x141879468), '#' starts a comment, blank lines ignored, trailing tokens on a line are
// ignored (so "0x141879468  read-obj-prop" works). Returns the number of hooks installed, or -1 if
// the file could not be opened.
extern "C" __declspec(dllexport) int HagIPC_InstallHooksFromFile(const wchar_t* path) {
    std::ifstream f(path);
    if (!f) { hag::Log::Line("[hooksfile] cannot open the hooks file"); return -1; }

    int n = 0, line = 0;
    std::string raw;
    while (std::getline(f, raw)) {
        ++line;
        const std::size_t h = raw.find('#');
        if (h != std::string::npos) raw.erase(h);
        // first whitespace-delimited token
        std::size_t a = raw.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        std::size_t b = raw.find_first_of(" \t\r\n", a);
        const std::string tok = raw.substr(a, b == std::string::npos ? std::string::npos : b - a);

        unsigned long long rva = 0;
        if (!ParseHex(tok, rva)) { hag::Log::Line("[hooksfile] line " + std::to_string(line) + ": not a hex RVA, skipped"); continue; }
        if (HagIPC_InstallHook(rva) == 1) ++n;
    }
    hag::Log::Line("[hooksfile] armed " + std::to_string(n) + " hook(s)");
    return n;
}
