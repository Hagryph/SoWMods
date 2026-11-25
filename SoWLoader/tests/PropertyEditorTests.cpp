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
        uint64_t available=0;
        widget.context=&available;
        widget.footerLabel="Generate";
        widget.footerGenerator=[](void* p){return *static_cast<uint64_t*>(p);};
        widget.popup.generate=[](void* ctx,uint64_t id){return id && id==*static_cast<uint64_t*>(ctx);};
        auto footerFrame=[&](bool down) {
            io.AddMousePosEvent(32,320);io.AddMouseButtonEvent(0,down);
            ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0,0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Test",nullptr,ImGuiWindowFlags_NoTitleBar);
            sow::DrawGeneratorFooter(widget,ImVec2(20,310),ImVec2(72,28));
            ImGui::End(); ImGui::Render();
        };
        footerFrame(false);footerFrame(true);footerFrame(false);
        if (widget.editingField) throw std::runtime_error("Disabled footer opened a preview");
        available=410;footerFrame(true);footerFrame(false);
        if (widget.editingField!=410 || widget.actionField!=410 || widget.popupTitle!="Generate###property-value")
            throw std::runtime_error("Generate did not open its preview");
        ImGui::DestroyContext();
        std::cout << "Property editor rendered nonblank across four desktop viewports\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
