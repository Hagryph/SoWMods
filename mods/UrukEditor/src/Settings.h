#pragma once
#include <Windows.h>

namespace uruk {
void Log(const char* message);
bool LoadSettings(HMODULE module);
bool IgnoreRecruitmentLevel();
void SetIgnoreRecruitmentLevel(bool enabled);
}
