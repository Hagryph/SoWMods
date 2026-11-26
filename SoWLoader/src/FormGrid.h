#pragma once
#include <imgui.h>
#include <algorithm>

namespace sow::actions {
template<class Label,class Control>
void DrawFormGrid(unsigned count,Label label,Control control) {
    const unsigned columns=ImGui::GetContentRegionAvail().x>=26*ImGui::GetFontSize()?2:1;
    if(!ImGui::BeginTable("##inputs",columns,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_PadOuterX))return;
    for(unsigned first=0;first<count;first+=columns){
        const unsigned end=std::min(first+columns,count);
        // Keep framed items out of the label row: their baseline includes frame padding.
        ImGui::TableNextRow();
        for(unsigned i=first;i<end;++i){ImGui::TableSetColumnIndex(i-first);label(i);}
        ImGui::TableNextRow();
        for(unsigned i=first;i<end;++i){ImGui::TableSetColumnIndex(i-first);control(i);}
    }
    ImGui::EndTable();
}
}
