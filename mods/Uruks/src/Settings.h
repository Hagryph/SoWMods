#pragma once
#include <Windows.h>

namespace uruk {
void Log(const char* message);
bool WriteGenerationLog(const char* report);
bool LoadSettings(int page);
bool IgnoreRecruitmentLevel();
void SetIgnoreRecruitmentLevel(bool enabled);
}
