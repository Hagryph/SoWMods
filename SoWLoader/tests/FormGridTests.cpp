#include "FormGrid.h"
#include <array>
#include <cstdio>

struct Bounds {float x,top,bottom;};
static Bounds item(){
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    return {a.x,a.y,b.y};
}
int main(){
    ImGui::CreateContext();
    auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={2560,1440};
    unsigned char* pixels;int w,h;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    for(float scale:{1.f,1.5f,2.f})for(bool narrow:{false,true}){
        for(int frame=0;frame<2;++frame){
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({(narrow?220.f:700.f)*scale,650.f*scale});
            ImGui::Begin("Form");
            ImGui::SetWindowFontScale(scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(6*scale,4*scale));
            std::array<Bounds,5> labels{},boxes{};
            sow::actions::DrawFormGrid(5,[&](unsigned i){
                ImGui::TextUnformatted(i?"Base class":"Captain");labels[i]=item();
            },[&](unsigned i){
                ImGui::PushID(i);ImGui::SetNextItemWidth(-1);
                if(ImGui::BeginCombo("##choice",i?"Select...":"Selected captain"))ImGui::EndCombo();
                boxes[i]=item();if(i==1)ImGui::TextUnformatted("Field validation message");
                ImGui::PopID();
            });
            ImGui::PopStyleVar();ImGui::End();ImGui::Render();
            for(unsigned i=0;i<boxes.size();++i){
                bool ok=boxes[i].bottom>boxes[i].top && labels[i].bottom<=boxes[i].top;
                if(!narrow && i%2)ok &= labels[i].top==labels[i-1].top &&
                    boxes[i].top==boxes[i-1].top && boxes[i].bottom==boxes[i-1].bottom && boxes[i].x>boxes[i-1].x;
                else if(i)ok &= labels[i].top>=boxes[i-1].bottom;
                if(!ok){std::fprintf(stderr,"Misaligned field %u at scale %.1f (narrow=%d)\n",i,scale,narrow);return 1;}
            }
        }
    }
    ImGui::DestroyContext();
}
