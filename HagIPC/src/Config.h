#pragma once
#include "PCH.h"
#include <string>

// HagIPC config. A flat INI in the shared SoWLoader config folder
// (%LOCALAPPDATA%\SoWLoader\config\HagIPC.ini), same architecture as every other Hagryph mod's
// config. Created with documented defaults on first run.
namespace hag {

struct Config {
    bool          enabled = true;    // dev tool: on by default for the dev's own machine
    std::uint16_t port    = 19000;   // localhost TCP port
    std::string   token;             // optional shared secret ("" = no auth)

    static Config Load();
};

}  // namespace hag
