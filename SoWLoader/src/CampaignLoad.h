#pragma once
#include "ModActions.h"
namespace sow {
bool RegisterCampaignActions(const sowmod::actions::Registrar*);
void PumpCampaignLoad();
void CampaignFrontEnd();
void CampaignWorldLoaded();
}
