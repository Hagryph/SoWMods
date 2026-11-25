#pragma once
#include "ModConfig.h"
#include <string>
namespace sow {
void BindModConfig(int page, const std::wstring& dllPath);
const sowmod::ConfigAPI* ModConfigAPI();
}
