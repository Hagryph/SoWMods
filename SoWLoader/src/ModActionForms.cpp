#include "ModActions.h"
#include "imgui.h"
#include <array>
#include <algorithm>
#include <map>
#include <sstream>
namespace sow::actions {
using namespace sowmod::actions;
namespace {
struct Form {
    std::vector<std::string> values;
    std::shared_ptr<Job> run,query;
    int open=-1;
    std::array<char,128> filter{};
    std::string status,options;
};
}
void DrawForm(const std::string& prefix) {
    static std::map<std::string,std::string> selected;
    static std::map<std::string,Form> forms;
    auto all=Commands();std::erase_if(all,[&](auto& c){return !c.id.starts_with(prefix+".");});
    if(all.empty())return;
    auto it=std::ranges::find(all,selected[prefix],&Command::id);if(it==all.end())it=all.begin();
    if(ImGui::BeginCombo("Action",it->spec.label)){
        for(auto i=all.begin();i!=all.end();++i)if(ImGui::Selectable(i->spec.label,i==it)){selected[prefix]=i->id;it=i;}
        ImGui::EndCombo();
    }
    auto& a=it->spec;auto& f=forms[it->id];ImGui::PushID(it->id.c_str());
    if(f.values.size()!=a.count)for(unsigned i=0;i<a.count;++i)f.values.emplace_back(a.inputs[i].initial?a.inputs[i].initial:"");
    Result result;
    if(Poll(f.run,result)){f.status=(result.ok?"":"Error: ")+result.text;f.run.reset();}
    if(Poll(f.query,result)){f.options=result.ok?result.text:"";if(!result.ok)f.status=result.text;f.query.reset();}
    bool anyOpen=false;
    for(unsigned n=0;n<a.count;++n){
        auto& input=a.inputs[n];auto& v=f.values[n];ImGui::PushID(input.key);
        if(input.type==Boolean){bool b=v=="true";if(ImGui::Checkbox(input.label,&b))v=b?"true":"false";}
        else if(input.type==Integer){int i=atoi(v.c_str());if(ImGui::InputInt(input.label,&i))v=std::to_string(std::clamp(i,input.min,input.max));}
        else if(input.type==Text){std::array<char,4097> buf{};strncpy_s(buf.data(),buf.size(),v.c_str(),_TRUNCATE);
            if(ImGui::InputText(input.label,buf.data(),buf.size()))v=buf.data();}
        else if(ImGui::BeginCombo(input.label,v.empty()?"Select...":v.c_str())){
            anyOpen=true;
            auto refresh=[&]{f.options.clear();f.query=Submit(*it,f.values,n,f.filter.data());};
            if(f.open!=int(n)){f.open=n;f.filter[0]=0;refresh();}
            if(ImGui::InputText("Filter",f.filter.data(),f.filter.size(),ImGuiInputTextFlags_EnterReturnsTrue))refresh();
            ImGui::SameLine();if(ImGui::Button("Search"))refresh();
            if(f.query)ImGui::TextUnformatted("Loading...");
            std::istringstream rows(f.options);std::string option;
            while(std::getline(rows,option)){
                std::vector<std::string> parts;std::istringstream ss(v);std::string p;
                while(std::getline(ss,p,','))parts.push_back(p);
                bool on=std::ranges::find(parts,option)!=parts.end();
                if(ImGui::Selectable(option.c_str(),on,input.type==MultiChoice?ImGuiSelectableFlags_NoAutoClosePopups:0)){
                    if(input.type==Choice)v=option;
                    else{if(on)std::erase(parts,option);else parts.push_back(option);
                        v.clear();for(auto& e:parts){if(!v.empty())v+=',';v+=e;}}
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if(!anyOpen)f.open=-1;
    ImGui::BeginDisabled(bool(f.run));
    if(ImGui::Button(a.label)){f.run=Submit(*it,f.values);f.status="Running...";}
    ImGui::EndDisabled();
    if(!f.status.empty())ImGui::TextWrapped("%s",f.status.c_str());
    ImGui::PopID();
}
}
