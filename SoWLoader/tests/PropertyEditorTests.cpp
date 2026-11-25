#include "PropertyEditor.h"
#include <imgui.h>
#include <iostream>
#include <stdexcept>
#include <cmath>

void Refresh(void*) {}
int Count(void*) { return 200; }
bool Read(void*, int index, HagUI_Property* field) {
    *field = {};
    field->id = index + 1;
    strcpy_s(field->label, "Weapon / ability with long label");
    strcpy_s(field->value, "Picker_This_Is_A_Long_Engine_Record_Name_To_Verify_Clipping_And_Fixed_Grid_Heights");
    field->editable = true;
    return true;
}
int Choices(void*, uint64_t) { return 0; }
bool Choice(void*, uint64_t, int, HagUI_Choice*) { return false; }
bool Apply(void*, uint64_t, uint64_t) { return false; }
int main() {
    try {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        auto* font = io.Fonts->AddFontDefault();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width < 1 || height < 1) throw std::runtime_error("Blank font atlas");
        sow::HagUI::Widget widget{};
        widget.properties = {Refresh, Count, Read, Choices, Choice, Apply};
        for (const auto display : {ImVec2(1280,720), ImVec2(1920,1080), ImVec2(2560,1440), ImVec2(3440,1440)}) {
            io.DisplaySize = display;
            io.DeltaTime = 1.0f / 60;
            for (int frame = 0; frame < 3; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0,0));
                ImGui::SetNextWindowSize(display);
                ImGui::Begin("Test", nullptr, ImGuiWindowFlags_NoTitleBar);
                const float sx = display.x * .64f / 820, sy = display.y * .64f / 462;
                sow::DrawPropertyEditor(widget, ImVec2(20,20), ImVec2(712 * sx, 304 * sy), sx, sy, font);
                ImGui::End();
                ImGui::Render();
                const auto* data = ImGui::GetDrawData();
                if (!data || data->TotalVtxCount < 100) throw std::runtime_error("Blank property editor");
                for (const auto* list : data->CmdLists) for (const auto& command : list->CmdBuffer) {
                    const auto clip = command.ClipRect;
                    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || clip.x > clip.z || clip.y > clip.w)
                        throw std::runtime_error("Invalid editor clip rectangle");
                }
            }
        }
        ImGui::DestroyContext();
        std::cout << "Property editor rendered nonblank across four desktop viewports\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
