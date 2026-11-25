#pragma once
#include "HagUI.h"
#include <imgui.h>
namespace sow {
using SceneTextureFn = ImTextureID (*)(void*, float& aspect);
void BeginPropertyFrame(void* context, SceneTextureFn capture);
void EndPropertyFrame();
void DrawGeneratorFooter(const HagUI::Widget& widget,ImVec2 position,ImVec2 size);
void DrawPropertyEditor(const HagUI::Widget& widget, ImVec2 position, ImVec2 size,
                        float sx, float sy, ImFont* font);
}
