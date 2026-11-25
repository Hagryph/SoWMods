#include "PropertyEditor.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace sow {
namespace {
bool Matches(const char* text, const char* query) {
    std::string value(text), filter(query);
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(value.begin(), value.end(), value.begin(), lower);
    std::transform(filter.begin(), filter.end(), filter.begin(), lower);
    return value.find(filter) != std::string::npos;
}
}
void DrawPropertyEditor(const HagUI::Widget& widget, ImVec2 position, ImVec2 size,
                        float sx, float sy, ImFont* font) {
    const auto& source = widget.properties;
    ImGui::PushFont(font, 15.0f * (std::min)(sx, sy));
    ImGui::SetCursorScreenPos(position);
    ImGui::SetNextItemWidth((std::max)(80.0f, size.x - 108.0f * sx));
    ImGui::InputTextWithHint("##fields", "Filter fields...", widget.propertySearch, sizeof(widget.propertySearch));
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(96 * sx, 28 * sy))) source.refresh(widget.context);
    std::vector<HagUI_Property> fields;
    const int count = (std::clamp)(source.count(widget.context), 0, 4096);
    for (int i = 0; i < count; ++i) {
        HagUI_Property field{};
        if (source.read(widget.context, i, &field) &&
            (Matches(field.label, widget.propertySearch) || Matches(field.value, widget.propertySearch))) fields.push_back(field);
    }
    ImGui::SetCursorScreenPos(ImVec2(position.x, position.y + 38 * sy));
    if (ImGui::BeginChild("##properties", ImVec2(size.x, (std::max)(1.0f, size.y - 38 * sy)), ImGuiChildFlags_Borders)) {
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
                    ImGui::TextUnformatted(field.label);
                    draw->PopClipRect();
                    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 52 * sx, origin.y));
                    ImGui::BeginDisabled(!field.editable);
                    if (ImGui::Button("Edit", ImVec2(48 * sx, 24 * sy))) {
                        widget.editingField = field.id;
                        widget.actionField = field.id;
                        widget.choiceSearch[0] = 0;
                        widget.applyFailed = false;
                        widget.choices.clear();
                        const int n = (std::clamp)(source.choiceCount(widget.context, field.id), 0, 100000);
                        for (int k = 0; k < n; ++k) {
                            HagUI_Choice choice{};
                            if (source.choice(widget.context, field.id, k, &choice)) widget.choices.push_back(choice);
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
    if (widget.editingField) {
        ImGui::OpenPopup("Choose value");
        widget.editingField = 0;
    }
    ImGui::SetNextWindowSize(ImVec2((std::min)(size.x, 600 * sx), 320 * sy), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Choose value", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##choices", "Search engine records...", widget.choiceSearch, sizeof(widget.choiceSearch));
        if (ImGui::BeginChild("##values", ImVec2(0, -64 * sy))) {
            for (const auto& choice : widget.choices) {
                if (!Matches(choice.label, widget.choiceSearch)) continue;
                ImGui::PushID(static_cast<int>(choice.id));
                if (ImGui::Selectable(choice.label, choice.selected)) {
                    if (source.apply(widget.context, widget.actionField, choice.id)) ImGui::CloseCurrentPopup();
                    else widget.applyFailed = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", choice.label);
                ImGui::PopID();
            }
            if (widget.choices.empty()) ImGui::TextDisabled("No compatible engine records.");
        }
        ImGui::EndChild();
        if (widget.applyFailed) ImGui::TextWrapped("Selection expired or another operation is pending.");
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopFont();
}
}
