#include "PropertyEditor.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace sow {
namespace {
SceneTextureFn captureScene{};
void *captureContext{}, *sceneContext{};
HagUI_SceneFn activeScene{};
bool sceneUsed{};
void StopScene() {
    char unused[192]{};
    if (activeScene && activeScene(sceneContext,0,0,unused)) return;
    activeScene=nullptr;
}
bool Matches(const char* text, const char* query) {
    std::string value(text), filter(query);
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(value.begin(), value.end(), value.begin(), lower);
    std::transform(filter.begin(), filter.end(), filter.begin(), lower);
    return value.find(filter) != std::string::npos;
}
}
void BeginPropertyFrame(void* context, SceneTextureFn capture) {
    captureContext=context; captureScene=capture; sceneUsed=false;
}
void EndPropertyFrame() {
    if (!sceneUsed) StopScene();
    captureContext=nullptr; captureScene=nullptr;
}
void DrawPropertyEditor(const HagUI::Widget& widget, ImVec2 position, ImVec2 size,
                        float sx, float sy, ImFont* font) {
    const auto& source = widget.properties;
    const auto& popup = widget.popup;
    HagUI_PopupState state{};
    if (popup.state) popup.state(widget.context, 0, 0, &state);
    ImGui::PushFont(font, 15.0f * (std::min)(sx, sy));
    ImGui::SetCursorScreenPos(position);
    const auto buttonSpace=[](const char* label) {
        return ImGui::CalcTextSize(label).x + 2*ImGui::GetStyle().FramePadding.x + ImGui::GetStyle().ItemSpacing.x;
    };
    const float commands=buttonSpace("Refresh")+(popup.state ? buttonSpace("Discard")+buttonSpace(popup.saveLabel) : 0.f);
    ImGui::SetNextItemWidth((std::max)(1.f,size.x-commands));
    ImGui::InputTextWithHint("##fields", "Filter fields...", widget.propertySearch, sizeof(widget.propertySearch));
    ImGui::SameLine();
    ImGui::BeginDisabled(state.busy || state.dirty);
    if (ImGui::Button("Refresh")) source.refresh(widget.context);
    ImGui::EndDisabled();
    if (popup.state) {
        ImGui::SameLine();
        ImGui::BeginDisabled(state.busy || !state.dirty);
        if (ImGui::Button("Discard")) popup.discard(widget.context);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(state.busy || !state.canSave);
        if (ImGui::Button(popup.saveLabel)) popup.save(widget.context);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && state.status[0]) ImGui::SetTooltip("%s", state.status);
    }
    std::vector<HagUI_Property> fields;
    const int count = (std::clamp)(source.count(widget.context), 0, 4096);
    for (int i = 0; i < count; ++i) {
        HagUI_Property field{};
        if (source.read(widget.context, i, &field) &&
            (Matches(field.label, widget.propertySearch) || Matches(field.value, widget.propertySearch))) fields.push_back(field);
    }
    ImGui::SetCursorScreenPos(ImVec2(position.x, position.y + 38 * sy));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(8*sx,6*sy));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(6*sx,3*sy));
    if (ImGui::BeginChild("##properties", ImVec2(size.x, (std::max)(1.0f, size.y - 38 * sy)), ImGuiChildFlags_Borders|ImGuiChildFlags_AlwaysUseWindowPadding)) {
        if (ImGui::BeginTable("##grid", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV)) {
            ImGuiListClipper clipper;
            const float rowHeight = 80 * sy;
            clipper.Begin(static_cast<int>((fields.size() + 1) / 2), rowHeight);
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                for (int col = 0; col < 2; ++col) {
                    const size_t index = row * 2 + col;
                    if (index >= fields.size()) break;
                    ImGui::TableSetColumnIndex(col);
                    const auto& field = fields[index];
                    ImGui::PushID(static_cast<int>(field.id));
                    auto origin = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    auto* draw = ImGui::GetWindowDrawList();
                    draw->PushClipRect(origin, ImVec2(origin.x + width - 60 * sx, origin.y + 26 * sy), true);
                    HagUI_PopupState detail{};
                    if (popup.state) popup.state(widget.context, field.id, 0, &detail);
                    if (detail.needsRepair) {
                        ImGui::TextColored(ImVec4(1,.28f,.24f,1), "!");
                        ImGui::SameLine();
                    }
                    ImGui::TextUnformatted(field.label);
                    draw->PopClipRect();
                    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 52 * sx, origin.y));
                    ImGui::BeginDisabled(!field.editable);
                    if (ImGui::Button("Edit", ImVec2(48 * sx, 24 * sy))) {
                        widget.editingField = field.id;
                        widget.actionField = field.id;
                        widget.popupTitle = std::string(field.label) + "###property-value";
                        widget.selectedChoice = 0;
                        widget.largePreview = false;
                        widget.choiceSearch[0] = 0;
                        widget.applyFailed = false;
                        widget.choices.clear();
                        widget.picks.clear();
                        const int n = (std::clamp)(source.choiceCount(widget.context, field.id), 0, 100000);
                        for (int k = 0; k < n; ++k) {
                            HagUI_Choice choice{};
                            if (source.choice(widget.context, field.id, k, &choice)) {
                                widget.choices.push_back(choice);
                                if (choice.selected) widget.picks.push_back(choice.id);
                            }
                        }
                    }
                    ImGui::EndDisabled();
                    if (!field.editable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", field.readOnlyReason[0] ? field.readOnlyReason : "Read-only");
                    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 30 * sy));
                    // Clip long records to the stable row; the tooltip retains the full value.
                    draw->PushClipRect(ImVec2(origin.x, origin.y + 28 * sy), ImVec2(origin.x + width, origin.y + 74 * sy), true);
                    draw->AddText(font, ImGui::GetFontSize(), ImVec2(origin.x, origin.y + 30 * sy),
                        ImGui::GetColorU32(ImGuiCol_Text), field.value, nullptr, width);
                    draw->PopClipRect();
                    ImGui::InvisibleButton("##value", ImVec2(width, 44 * sy));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", field.value);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (fields.empty()) ImGui::TextDisabled("No matching fields.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    if (widget.editingField) {
        ImGui::OpenPopup(widget.popupTitle.c_str());
        widget.editingField = 0;
    }
    if (widget.popupTitle.empty()) { ImGui::PopFont(); return; }
    ImVec2 display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(widget.largePreview ?
        ImVec2((std::min)(display.x*.92f,940*sx),(std::min)(display.y*.9f,660*sy)) :
        ImVec2((std::min)(size.x,600*sx),(std::min)(size.y,405*sy)));
    if (widget.largePreview) ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16*sx,12*sy));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(12*sx,5*sy));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8*sx,8*sy));
    if (ImGui::BeginPopupModal(widget.popupTitle.c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
        const bool building=widget.set.handles && widget.set.handles(widget.context,widget.actionField);
        if (building) ImGui::Text("Replacement list: %d selected",int(widget.picks.size()));
        const char* error="Selection expired or another operation is pending.";
        const float gap=ImGui::GetStyle().ItemSpacing.y;
        const float footer=ImGui::GetFrameHeight()+gap+(widget.applyFailed ?
            ImGui::CalcTextSize(error,nullptr,false,ImGui::GetContentRegionAvail().x).y+gap : 0.f);
        HagUI_PopupState detail{};
        if (popup.state) popup.state(widget.context, widget.actionField, widget.selectedChoice, &detail);
        if (detail.generator) {
            ImGui::BeginDisabled(detail.busy);
            if (ImGui::Button("Generate")) { widget.selectedChoice = 0; popup.generate(widget.context, widget.actionField); }
            ImGui::EndDisabled();
            HagUI_Choice candidate{};
            if (!detail.busy && source.choice(widget.context, widget.actionField, 0, &candidate))
                widget.selectedChoice = candidate.id;
            popup.state(widget.context, widget.actionField, widget.selectedChoice, &detail);
        } else {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##choices", "Search engine records...", widget.choiceSearch, sizeof(widget.choiceSearch));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10*sx,8*sy));
        if (ImGui::BeginChild("##values", ImVec2(0, (detail.preview[0] || widget.largePreview) ? 96 * sy : -footer),
                              ImGuiChildFlags_AlwaysUseWindowPadding)) {
            for (const auto& choice : widget.choices) {
                if (!Matches(choice.label, widget.choiceSearch)) continue;
                auto& picks=widget.picks;
                auto picked=std::find(picks.begin(),picks.end(),choice.id);
                if (building && picked==picks.end() && !widget.set.compatible(widget.context,widget.actionField,choice.id,picks.data(),int(picks.size()))) continue;
                ImGui::PushID(static_cast<int>(choice.id));
                HagUI_PopupState option{};
                if (popup.state) popup.state(widget.context,widget.actionField,choice.id,&option);
                ImGui::BeginDisabled(option.unavailable);
                if (building) {
                    bool on=picked!=picks.end();
                    if (ImGui::Checkbox(choice.label,&on)) {
                        if (on) picks.push_back(choice.id); else picks.erase(picked);
                    }
                } else if (ImGui::Selectable(choice.label, widget.selectedChoice ? widget.selectedChoice == choice.id : choice.selected))
                    widget.selectedChoice = choice.id;
                ImGui::EndDisabled();
                if ((option.unavailable || ImGui::CalcTextSize(choice.label).x>ImGui::GetItemRectSize().x) &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s", option.unavailable?option.status:choice.label);
                ImGui::PopID();
            }
            if (widget.choices.empty()) ImGui::TextDisabled("No compatible engine records.");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        }
        char message[192]{};
        int scene=0;
        if (widget.scene) {
            if (activeScene && (activeScene!=widget.scene || sceneContext!=widget.context)) StopScene();
            scene=widget.scene(widget.context,widget.actionField,widget.selectedChoice,message);
            widget.largePreview=scene!=0;
            if (scene) { activeScene=widget.scene; sceneContext=widget.context; sceneUsed=true; }
        }
        if (detail.generator || detail.preview[0] || scene) {
            if (ImGui::BeginChild("Preview", ImVec2(0,-footer), ImGuiChildFlags_Borders)) {
                ImGui::TextDisabled(scene ? "Army preview" : "Preview");
                if (scene==2 && captureScene) {
                    float aspect=1;
                    const auto texture=captureScene(captureContext,aspect);
                    if (texture && aspect>0) {
                        auto avail=ImGui::GetContentRegionAvail();
                        const float w=(std::min)(avail.x, (std::max)(1.f,avail.y)*aspect);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(avail.x-w)*.5f);
                        ImGui::Image(texture,ImVec2(w,w/aspect));
                    } else ImGui::TextDisabled("Scene texture unavailable.");
                }
                if (message[0]) ImGui::TextWrapped("%s",message);
                for (int i=0, shown=0; i<17; ++i) if (detail.palette[i] >> 24) {
                    if (shown++ % 8) ImGui::SameLine();
                    auto c=detail.palette[i];
                    ImGui::PushID(i);
                    ImGui::ColorButton("##colour", ImVec4(((c>>16)&255)/255.f, ((c>>8)&255)/255.f,(c&255)/255.f,1));
                    ImGui::PopID();
                }
                ImGui::TextWrapped("%s", detail.busy ? "Generating..." : detail.preview[0] ? detail.preview : detail.status);
            }
            ImGui::EndChild();
        }
        if (widget.applyFailed) ImGui::TextWrapped("%s",error);
        ImGui::BeginDisabled((!building && !widget.selectedChoice) || detail.busy);
        if (ImGui::Button("Apply")) {
            if (building ? widget.set.apply(widget.context,widget.actionField,widget.picks.data(),int(widget.picks.size())) : source.apply(widget.context,widget.actionField,widget.selectedChoice)) {
                StopScene();
                if (popup.close) popup.close(widget.context,widget.actionField);
                ImGui::CloseCurrentPopup();
            }
            else widget.applyFailed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            StopScene();
            if (popup.close) popup.close(widget.context,widget.actionField);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
}
}
