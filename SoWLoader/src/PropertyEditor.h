#pragma once
#include "HagUI.h"
#include <imgui.h>
namespace sow {
void DrawPropertyEditor(const HagUI::Widget& widget, ImVec2 position, ImVec2 size,
                        float sx, float sy, ImFont* font);
}
