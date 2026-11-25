#pragma once
#include <Windows.h>

namespace uruk {
void Log(const char* message);
bool LoadSettings(int page);
bool IgnoreRecruitmentLevel();
void SetIgnoreRecruitmentLevel(bool enabled);
}
