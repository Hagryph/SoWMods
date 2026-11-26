#include "ModActions.h"
#include "GameHooks.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <map>
#include <sstream>

namespace sow::actions {
using namespace sowmod::actions;
namespace {
struct Binding {
    std::shared_ptr<Job> query;
    std::string data,error;
    std::array<char,128> search{};
    double due=0;
    bool open=false,loaded=false;
};
struct Form {
    std::vector<std::string> values;
    std::vector<Binding> bindings;
    std::shared_ptr<Job> run,preset,commit;
    std::vector<std::string> commitValues;
    std::string status;
    uint64_t session=0;
    int frame=-2;
};
}
void DrawForm(const std::string& id) {
    static std::map<std::string,Form> forms;
    auto all=Commands();
    auto it=std::ranges::find(all,id,&Command::id);
    if(it==all.end())return;
    auto& a=it->spec;auto& f=forms[id];
    const auto session=CurrentSaveSession();
    if(f.session!=session){f=Form{};f.session=session;}
    if(a.needsSave && !session){ImGui::TextUnformatted("Load a save first.");return;}
    if(f.bindings.size()!=a.count){
        f.bindings.resize(a.count);
        for(unsigned i=0;i<a.count;++i)f.values.emplace_back(a.inputs[i].initial?a.inputs[i].initial:"");
    }
    bool enter=f.frame+1!=ImGui::GetFrameCount();f.frame=ImGui::GetFrameCount();
    Result result;bool ran=Poll(f.run,result);
    auto next=all.end();
    if(a.followup)next=std::ranges::find(all,id.substr(0,id.rfind('.')+1)+a.followup,&Command::id);
    if(ran){
        f.status=(result.ok?"":"Error: ")+result.text;f.run.reset();
        if(result.ok && next!=all.end()){
            std::map<std::string,std::string> outputs;std::istringstream words(result.text);
            for(std::string word;words>>word;){auto p=word.find('=');if(p!=word.npos)outputs.emplace(word.substr(0,p),word.substr(p+1));}
            for(unsigned i=0;i<next->spec.count;++i){
                auto pos=outputs.find(next->spec.inputs[i].key);
                if(pos==outputs.end()){f.commitValues.clear();break;}
                f.commitValues.push_back(pos->second);
            }
            if(!f.commitValues.empty())f.status="Preview ready.";
        }
    }
    if(Poll(f.commit,result)){f.status=(result.ok?"":"Error: ")+result.text;f.commit.reset();}
    if(a.preset){
        if(Poll(f.preset,result)){
            if(result.ok){
                std::istringstream lines(result.text);
                for(std::string line;std::getline(lines,line);){
                    auto eq=line.find('=');
                    if(eq==line.npos)continue;
                    for(unsigned i=0;i<a.count;++i)if(line.substr(0,eq)==a.inputs[i].key){
                        f.values[i]=line.substr(eq+1);f.bindings[i]=Binding{};break;
                    }
                }
                f.status="Template loaded.";
            }else f.status=result.text;
            f.preset.reset();
        }
        ImGui::BeginDisabled(bool(f.preset)||bool(f.run)||bool(f.commit));
        if(ImGui::Button(a.presetLabel)){f.commitValues.clear();f.preset=Submit(*it,f.values,-2);}
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(bool(f.preset)||bool(f.run)||bool(f.commit));
    std::uint32_t changed=0;
    for(unsigned n=0;n<a.count;++n){
        auto& in=a.inputs[n];auto& b=f.bindings[n];auto& value=f.values[n];
        const auto before=value;auto spec=in.source;const bool choice=in.type>=Choice;
        auto pull=[&]{
            b.query=Submit(*it,f.values,n,b.search.data());b.loaded=true;b.data.clear();b.error.clear();
            b.due=ImGui::GetTime()+spec.intervalMs/1000.0;
        };
        if(enter)b.open=false;
        if(enter && !choice && (spec.when&OnOpen) && spec.pull)pull();
        if(ran && (spec.when&AfterRun))pull();
        if((spec.when&Periodic) && !b.query && ImGui::GetTime()>=b.due)pull();
        if(Poll(b.query,result)){
            if(!result.ok)b.error=result.text;
            else if(choice)b.data=result.text;
            else value=result.text;
            b.query.reset();
        }
        ImGui::PushID(in.key);
        if(!choice)ImGui::BeginDisabled(bool(b.query));
        if(in.type==Boolean){
            bool on=value=="true";if(ImGui::Checkbox(in.label,&on))value=on?"true":"false";
        }else if(in.type==Integer){
            int i=atoi(value.c_str());if(ImGui::InputInt(in.label,&i))value=std::to_string(std::clamp(i,in.min,in.max));
        }else if(in.type==Text){
            std::array<char,4097> buf{};strncpy_s(buf.data(),buf.size(),value.c_str(),_TRUNCATE);
            if(ImGui::InputText(in.label,buf.data(),buf.size()))value=buf.data();
        }else if(ImGui::BeginCombo(in.label,value.empty()?"Select...":value.c_str())){
            if(!b.open && (!b.loaded || (spec.when&OnOpen)))pull();
            b.open=true;
            if(ImGui::InputText("Filter",b.search.data(),b.search.size(),ImGuiInputTextFlags_EnterReturnsTrue))pull();
            if(b.query)ImGui::TextUnformatted("Loading...");
            std::istringstream rows(b.data);
            for(std::string opt;std::getline(rows,opt);){
                std::vector<std::string> parts;std::istringstream ss(value);
                for(std::string part;std::getline(ss,part,',');)parts.push_back(part);
                bool on=std::ranges::find(parts,opt)!=parts.end();
                if(ImGui::Selectable(opt.c_str(),on,in.type==MultiChoice?ImGuiSelectableFlags_NoAutoClosePopups:0)){
                    if(in.type==Choice)value=opt;
                    else{if(on)std::erase(parts,opt);else parts.push_back(opt);
                        value.clear();for(auto& part:parts){if(!value.empty())value+=',';value+=part;}}
                }
            }
            ImGui::EndCombo();
        }else b.open=false;
        if(!choice)ImGui::EndDisabled();
        if(!b.error.empty())ImGui::TextWrapped("%s",b.error.c_str());
        if(value!=before)changed|=1u<<n;
        ImGui::PopID();
    }
    // Invalidate even closed dependants. A late reply to the old request must not win.
    for(unsigned n=0;n<a.count;++n){
        auto& in=a.inputs[n];auto& b=f.bindings[n];
        if(!(in.source.dependencies&changed))continue;
        if(in.type>=Choice){
            auto reset=in.initial?in.initial:"";
            if(f.values[n]!=reset)changed|=1u<<n;
            f.values[n]=reset;
        }
        b.data.clear();b.error.clear();b.loaded=true;
        b.query=Submit(*it,f.values,n,b.search.data());
    }
    bool pending=false;
    for(unsigned i=0;i<a.count;++i)pending|=a.inputs[i].type<Choice && bool(f.bindings[i].query);
    ImGui::BeginDisabled(bool(f.run)||pending);
    if(changed){f.commitValues.clear();f.status.clear();}
    if(ImGui::Button(a.label)){f.commitValues.clear();f.run=Submit(*it,f.values);f.status="Running...";}
    ImGui::EndDisabled();
    if(next!=all.end()){
        ImGui::SameLine();ImGui::BeginDisabled(f.commitValues.empty());
        if(ImGui::Button(next->spec.label)){f.commit=Submit(*next,f.commitValues);f.commitValues.clear();f.status="Saving...";}
        ImGui::EndDisabled();
    }
    if(!f.status.empty())ImGui::TextWrapped("%s",f.status.c_str());
    ImGui::EndDisabled();
}
}
