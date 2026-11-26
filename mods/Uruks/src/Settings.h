#pragma once
#include <Windows.h>

namespace uruk {
void Log(const char* message);
bool WriteGenerationLog(const char* report);
bool LoadSettings(int page);
bool IgnoreRecruitmentLevel();
bool FreeGarrisonPlacement();
bool SaveFreeGarrisonPlacement(bool enabled);
bool RetainGeneratedTraits();
bool SaveRetainGeneratedTraits(bool enabled);
bool ApplyGarrisonCost(bool free);
void SetIgnoreRecruitmentLevel(bool enabled);
}
