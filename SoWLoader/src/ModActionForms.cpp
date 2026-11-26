#include "ModActions.h"
#include "FormGrid.h"
#include "GameHooks.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

namespace sow::actions {
using namespace sowmod::actions;
namespace {
struct Binding {
    std::shared_ptr<Job> query;
    std::string data,error;
    std::array<char,128> search{};
    std::map<std::string,std::string> labels;
    double due=0,searchDue=0;
    bool open=false,loaded=false;
};
struct FormTheme {
    FormTheme(float em){
        auto color=[](ImGuiCol key,ImU32 v){ImGui::PushStyleColor(key,v);};
        color(ImGuiCol_Header,IM_COL32(91,70,36,255));
        color(ImGuiCol_HeaderHovered,IM_COL32(77,61,36,255));
        color(ImGuiCol_HeaderActive,IM_COL32(111,84,39,255));
        color(ImGuiCol_TextSelectedBg,IM_COL32(171,131,55,150));
        color(ImGuiCol_NavCursor,IM_COL32(224,179,74,255));
        color(ImGuiCol_PopupBg,IM_COL32(29,26,22,255));
        color(ImGuiCol_Border,IM_COL32(115,91,47,255));
        color(ImGuiCol_FrameBg,IM_COL32(38,33,26,255));
        color(ImGuiCol_FrameBgHovered,IM_COL32(51,43,31,255));
        color(ImGuiCol_FrameBgActive,IM_COL32(61,49,32,255));
        color(ImGuiCol_Button,IM_COL32(59,48,30,255));
        color(ImGuiCol_ButtonHovered,IM_COL32(80,63,34,255));
        color(ImGuiCol_ButtonActive,IM_COL32(103,79,39,255));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,em*.16f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,em*.35f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(em*.46f,em*.20f));
    }
    ~FormTheme(){ImGui::PopStyleVar(4);ImGui::PopStyleColor(13);}
};
std::string fold(std::string s){
    for(char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;
}
struct Option {std::string id,label;};
bool optionRow(const Option& opt,bool selected,float em){
    ImGui::PushID(opt.id.c_str());
    const auto pos=ImGui::GetCursorScreenPos();
    const ImVec2 size{ImGui::GetContentRegionAvail().x,em*1.55f};
    const bool clicked=ImGui::InvisibleButton("##option",size,ImGuiButtonFlags_EnableNav);
    auto* draw=ImGui::GetWindowDrawList();
    const bool hover=ImGui::IsItemHovered(),focused=ImGui::IsItemFocused();
    const ImVec2 end{pos.x+size.x,pos.y+size.y};
    if(selected || hover || focused)
        draw->AddRectFilled(pos,end,selected?IM_COL32(58,48,32,255):IM_COL32(45,39,31,255),em*.22f);
    if(focused)draw->AddRect(pos,end,IM_COL32(150,120,64,255),em*.22f);
    float mid=pos.y+size.y*.5f;
    draw->PushClipRect(pos,{end.x-em*1.35f,end.y},true);
    draw->AddText({pos.x+em*.55f,mid-em*.5f},selected?IM_COL32(239,205,140,255):ImGui::GetColorU32(ImGuiCol_Text),opt.label.c_str());
    draw->PopClipRect();
    if(selected){
        const float x=end.x-em*.85f;
        const ImVec2 tick[]={{x-em*.22f,mid},{x-em*.05f,mid+em*.16f},{x+em*.25f,mid-em*.20f}};
        draw->AddPolyline(tick,3,IM_COL32(224,179,74,255),0,em*.09f);
    }
    ImGui::PopID();return clicked;
}
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
    const float em=ImGui::GetFontSize();FormTheme theme(em);
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
                        f.values[i]=line.substr(eq+1);break;
                    }
                }
                for(unsigned i=0;i<a.count;++i){
                    auto& binding=f.bindings[i];binding=Binding{};
                    if(a.inputs[i].type==Choice && !f.values[i].empty())
                        binding.query=Submit(*it,f.values,i,f.values[i]);
                }
                f.status="Template loaded.";
            }else f.status=result.text;
            f.preset.reset();
        }
        ImGui::BeginDisabled(bool(f.preset)||bool(f.run)||bool(f.commit)||(a.count && f.values[0].empty()));
        if(ImGui::Button(a.presetLabel)){f.commitValues.clear();f.preset=Submit(*it,f.values,-2);}
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(bool(f.preset)||bool(f.run)||bool(f.commit));
    bool missing=false;
    for(unsigned n=0;n<a.count;++n)missing|=!a.inputs[n].initial && f.values[n].empty();
    if(a.preset)ImGui::SameLine();
    ImGui::BeginDisabled(missing);
    if(ImGui::Button(a.label)){f.commitValues.clear();f.run=Submit(*it,f.values);f.status="Running...";}
    ImGui::EndDisabled();
    if(!f.status.empty())ImGui::TextWrapped("%s",f.status.c_str());
    ImGui::Spacing();
    std::uint32_t changed=0;
    DrawFormGrid(a.count,[&](unsigned n){
        ImGui::TextColored(ImVec4(.78f,.73f,.63f,1),"%s",a.inputs[n].label);
    },[&](unsigned n){
        auto& in=a.inputs[n];auto& b=f.bindings[n];auto& value=f.values[n];
        const auto before=value;auto spec=in.source;const bool choice=in.type>=Choice;
        auto pull=[&]{
            b.query=Submit(*it,f.values,n,b.search.data());b.loaded=true;b.error.clear();
            b.due=ImGui::GetTime()+spec.intervalMs/1000.0;
        };
        if(enter)b.open=false;
        if(enter && !choice && (spec.when&OnOpen) && spec.pull)pull();
        if(ran && (spec.when&AfterRun))pull();
        if((spec.when&Periodic) && !b.query && ImGui::GetTime()>=b.due)pull();
        if(Poll(b.query,result)){
            if(!result.ok)b.error=result.text;
            else if(choice){
                b.data=std::move(result.text);std::istringstream rows(b.data);
                for(std::string row;std::getline(rows,row);){
                    auto tab=row.find('\t');if(tab!=row.npos)b.labels[row.substr(0,tab)]=row.substr(tab+1);
                }
            }
            else value=result.text;
            b.query.reset();
        }
        ImGui::PushID(in.key);
        ImGui::SetNextItemWidth(-1);
        if(!choice)ImGui::BeginDisabled(bool(b.query));
        if(in.type==Boolean){
            bool on=value=="true";if(ImGui::Checkbox("##value",&on))value=on?"true":"false";
        }else if(in.type==Integer){
            int i=atoi(value.c_str());if(ImGui::InputInt("##value",&i))value=std::to_string(std::clamp(i,in.min,in.max));
        }else if(in.type==Text){
            std::array<char,4097> buf{};strncpy_s(buf.data(),buf.size(),value.c_str(),_TRUNCATE);
            if(ImGui::InputText("##value",buf.data(),buf.size()))value=buf.data();
         }else{
            std::vector<std::string> picks;std::istringstream selected(value);
            for(std::string part;std::getline(selected,part,',');)picks.push_back(part);
            const std::string preview=value.empty()?"Select...":in.type==MultiChoice?
                std::to_string(picks.size())+" selected":b.labels.contains(value)?b.labels.at(value):value;
            ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(FLT_MAX,em*16));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(em*.50f,em*.40f));
            if(ImGui::BeginCombo("##choice",preview.c_str())){
                if(!b.open){
                    b.search.fill(0);b.searchDue=0;
                    if(!b.loaded || (spec.when&OnOpen))pull();
                    ImGui::SetKeyboardFocusHere();
                }
                b.open=true;
                const bool searchable=b.search[0] || std::count(b.data.begin(),b.data.end(),'\n')>=6;
                if(searchable){
                    ImGui::SetNextItemWidth(-1);
                    if(ImGui::InputTextWithHint("##filter","Search...",b.search.data(),b.search.size()))
                        b.searchDue=ImGui::GetTime()+.20;
                }
                if(b.searchDue && ImGui::GetTime()>=b.searchDue){pull();b.searchDue=0;}
                const auto needle=fold(b.search.data());
                std::istringstream rows(b.data);std::vector<Option> options;
                for(std::string row;std::getline(rows,row);){
                    auto tab=row.find('\t');auto opt=row.substr(0,tab);
                    auto label=tab==row.npos?opt:row.substr(tab+1);
                    if(needle.empty() || fold(label).contains(needle) || fold(opt).contains(needle))
                        options.push_back({std::move(opt),std::move(label)});
                }
                const int count=std::clamp(int(options.size()),1,6);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(em*.3f,em*.10f));
                const float height=std::ceil(count*em*1.55f+(count-1)*ImGui::GetStyle().ItemSpacing.y)+1;
                if(ImGui::BeginChild("##options",ImVec2(0,height))){
                    for(auto& opt:options){
                        bool on=std::ranges::find(picks,opt.id)!=picks.end();
                        if(optionRow(opt,on,em)){
                            if(in.type==Choice){value=opt.id;ImGui::CloseCurrentPopup();}
                            else{if(on)std::erase(picks,opt.id);else picks.push_back(opt.id);
                                value.clear();for(auto& p:picks){if(!value.empty())value+=',';value+=p;}}
                        }
                    }
                    if(options.empty())ImGui::TextDisabled("%s",b.query?"Loading...":"No matches");
                }
                ImGui::EndChild();ImGui::PopStyleVar();ImGui::EndCombo();
            }else b.open=false;
            ImGui::PopStyleVar();
        }
        if(!choice)ImGui::EndDisabled();
        if(!b.error.empty())ImGui::TextWrapped("%s",b.error.c_str());
        if(value!=before)changed|=1u<<n;
        ImGui::PopID();
    });
    // Invalidate even closed dependants. A late reply to the old request must not win.
    for(unsigned n=0;n<a.count;++n){
        auto& in=a.inputs[n];auto& b=f.bindings[n];
        const bool resetByParent=(in.source.dependencies&changed)!=0;
        if(!resetByParent && !(changed && (in.source.when&OnDraft)))continue;
        if(resetByParent && in.type>=Choice){
            auto reset=in.initial?in.initial:"";
            if(f.values[n]!=reset)changed|=1u<<n;
            f.values[n]=reset;
        }
        b.data.clear();b.error.clear();b.query.reset();b.loaded=false;b.searchDue=0;
        if(b.open || in.type<Choice){b.query=Submit(*it,f.values,n,b.search.data());b.loaded=true;}
    }
    if(changed){f.commitValues.clear();f.status.clear();}
    if(next!=all.end()){
        ImGui::SameLine();ImGui::BeginDisabled(f.commitValues.empty());
        if(ImGui::Button(next->spec.label)){f.commit=Submit(*next,f.commitValues);f.commitValues.clear();f.status="Saving...";}
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
}
}
