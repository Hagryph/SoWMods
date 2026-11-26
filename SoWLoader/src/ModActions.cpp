#include "ModActions.h"
#include "GameHooks.h"
#include "HagUI.h"
#include "GameTaskQueue.h"
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <sddl.h>

namespace sow::actions {
using namespace sowmod::actions;
namespace {
std::mutex registryMutex;
std::map<std::string,Command> registry;
bool key(std::string_view s) {
    return !s.empty() && s.size()<=64 && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==s.npos;
}
bool add(void* context,const Action* a) {
    if (!a || !a->key || !key(a->key) || !a->label || !*a->label || !a->run || a->count>32 || (a->count && !a->inputs)) return false;
    auto& batch=*static_cast<std::vector<Command>*>(context);
    if(a->rows && (a->count!=2 || a->inputs[0].type!=Choice || a->inputs[1].type!=Integer ||
        !a->facets || !a->facetCount || a->facetCount>16))return false;
    if(a->titleRow && a->count)return false;
    if(a->preset && (!a->presetLabel || !*a->presetLabel))return false;
    std::string_view path=a->menu?a->menu:"";
    if(path.size()>160 || path.starts_with('/') || path.ends_with('/') || path.find("//")!=path.npos ||
        path.find_first_of("\r\n\t#")!=path.npos)return false;
    if (std::ranges::any_of(batch,[&](auto& c){return c.id==a->key;})) return false;
    std::set<std::string> names;
    for(unsigned i=0;i<a->count;++i) {
        auto& f=a->inputs[i];
        if (!f.key || !key(f.key) || !f.label || !names.insert(f.key).second || f.type>MultiChoice ||
            (f.type==Integer && f.max<f.min) || ((f.type==Choice || f.type==MultiChoice) && !f.options && !a->choices && !f.source.pull)) return false;
        auto s=f.source;
        if(!s.when || (s.when&~15u) || ((s.when&Periodic) && s.intervalMs<250) ||
           ((s.when&OnInputs)!=0)!=(s.dependencies!=0) || (s.dependencies>>i) ||
           (f.type<Choice && !s.pull && s.when!=OnOpen))return false;
    }
    batch.push_back({a->key,*a});return true;
}
const char* typeName(Type t){return t==Text?"text":t==Boolean?"bool":t==Integer?"int":t==Choice?"choice":"multi";}
std::string validate(const Input& f,const std::string& s) {
    if (s.empty()) return f.initial && !*f.initial?"":std::string("missing ")+f.key;
    if (s.size()>4096 || std::any_of(s.begin(),s.end(),[](unsigned char c){return c<32;})) return "invalid input";
    if (f.type==Integer) {
        int v;auto [end,err]=std::from_chars(s.data(),s.data()+s.size(),v);
        if(err!=std::errc{} || end!=s.data()+s.size() || v<f.min || v>f.max) return std::string("out of range: ")+f.key;
    }
    if(f.type==Boolean && s!="true" && s!="false") return std::string("expected true or false: ")+f.key;
    if(f.type==Choice || f.type==MultiChoice) {
        std::set<std::string> seen;
        for(size_t start=0;;) {
            auto end=s.find(',',start);auto part=s.substr(start,end-start);
            if(part.empty() || !seen.insert(part).second || (f.type==Choice && end!=s.npos)) return std::string("invalid selection: ")+f.key;
            if (f.options && (","+std::string(f.options)+",").find(","+part+",")==s.npos) return std::string("unknown choice: ")+f.key;
            if(end==s.npos) break;
            start=end+1;
        }
    }
    return {};
}
}
bool RegisterMod(const std::string& prefix,Register fn,std::vector<Command>& added) {
    if(!fn || !key(prefix)) return false;
    std::vector<Command> batch;Registrar api{SchemaVersion,&batch,add};
    if(!fn(&api) || batch.empty()) return false;
    for(auto& c:batch)if(c.spec.followup && (!key(c.spec.followup) ||
        !std::ranges::any_of(batch,[&](const Command& n){return n.id==c.spec.followup && n.spec.ipcOnly && n.spec.count && n.spec.needsSave==c.spec.needsSave;})))return false;
    std::lock_guard lock(registryMutex);
    for(auto& c:batch){c.id=prefix+"."+c.id;if(registry.contains(c.id))return false;}
    for(auto& c:batch)registry.emplace(c.id,c);
    added=std::move(batch);return true;
}
std::vector<Command> Commands(){std::lock_guard lock(registryMutex);std::vector<Command> out;for(auto& [_,c]:registry)out.push_back(c);return out;}
struct Job {
    Command command;std::vector<std::string> values;int field;std::string search;
    uint64_t session;std::mutex mutex;std::condition_variable changed;
    bool started=false,done=false;Result result{};
};
namespace {
void finish(Job& j,Result r){std::lock_guard lock(j.mutex);j.result=std::move(r);j.done=true;j.changed.notify_all();}
void emit(void* p,const char* value) {
    auto& out=*static_cast<std::string*>(p);
    if(value && out.size()+strlen(value)<60000){if(!out.empty())out+='\n';out+=value;}
}
void execute(Job& j) {
    {std::lock_guard lock(j.mutex);if(j.done)return;j.started=true;}
    if(j.command.spec.needsSave && (!j.session || CurrentSaveSession()!=j.session)){finish(j,{false,"save session changed"});return;}
    std::vector<const char*> values;for(auto& v:j.values)values.push_back(v.c_str());
    Result r{};
    auto& a=j.command.spec;
    if(j.field==CatalogRequest){
        struct Sink { unsigned n; std::vector<Row>& rows; } sink{a.facetCount,r.rows};
        r.ok=a.rows([](void* ctx,const char* id,const char* label,const char*const* facets){
            auto& s=*static_cast<Sink*>(ctx);if(!id || !*id || !label)return;
            Row row{id,label};for(unsigned i=0;i<s.n;++i)row.facets.emplace_back(facets && facets[i]?facets[i]:"");
            s.rows.push_back(std::move(row));
        },&sink);
        r.text=std::to_string(r.rows.size())+" rows";
    }else if(j.field==VisibilityRequest){r.ok=true;r.text=a.show()?"true":"false";}
    else if(j.field==-2) r.ok=a.preset(values.data(),emit,&r.text);
    else if(j.field<0) r.ok=a.run(values.data(),emit,&r.text);
    else if(auto pull=a.inputs[j.field].source.pull) r.ok=pull(j.field,values.data(),j.search.c_str(),emit,&r.text);
    else if(auto opts=a.inputs[j.field].options){
        std::istringstream ss(opts);std::string item;
        while(std::getline(ss,item,','))if(item.find(j.search)!=item.npos)emit(&r.text,item.c_str());
        r.ok=true;
    }else r.ok=a.choices && a.choices(j.field,values.data(),j.search.c_str(),emit,&r.text);
    finish(j,std::move(r));
}
bool guarded(Job& j){__try{execute(j);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
void run(void* data){
    std::unique_ptr<std::shared_ptr<Job>> owner(static_cast<std::shared_ptr<Job>*>(data));
    try {if(guarded(**owner))return;}catch(...){}
    finish(**owner,{false,"mod action failed; inspect game state before retrying"});
}
}
std::shared_ptr<Job> Submit(const Command& c,std::vector<std::string> values,int field,std::string search) {
    auto j=std::make_shared<Job>();j->command=c;j->field=field;j->search=std::move(search);j->values=std::move(values);
    auto& a=c.spec;j->session=CurrentSaveSession();
    std::string error;
    if(j->values.size()!=a.count || field<VisibilityRequest || (field==CatalogRequest && !a.rows) || (field==VisibilityRequest && !a.show) || (field==-2 && !a.preset) || field>=int(a.count)) error="bad inputs";
    else if(field==-1)for(unsigned i=0;i<a.count && error.empty();++i)error=validate(a.inputs[i],j->values[i]);
    if(error.empty() && a.needsSave && !j->session) error="load a save first";
    if(!error.empty()){finish(*j,{false,error});return j;}
    auto ctx=new std::shared_ptr<Job>(j);
    if(!a.needsSave)run(ctx);
    else if(!QueueGameTask(run,ctx)){delete ctx;finish(*j,{false,"game queue unavailable"});}
    return j;
}
bool Poll(const std::shared_ptr<Job>& j,Result& r){if(!j)return false;std::lock_guard lock(j->mutex);if(!j->done)return false;r=j->result;return true;}
namespace {
std::string dispatch(std::string line) {
    std::istringstream in(line);std::string verb,id,arg,field,search;
    in>>verb;
    if(verb=="layout"){
        std::string out="ok";auto& pages=HagUI::Get().Pages();
        for(auto& page:pages){
            std::string path=page.title;
            for(int parent=page.parent;parent>=0;parent=pages[parent].parent)path=pages[parent].title+"/"+path;
            for(auto& w:page.widgets){
                const char* kind=w.type==HagUI::WToggle?"toggle":w.type==HagUI::WHeaderButton?"title":w.type==HagUI::WList?"catalog":
                    w.type==HagUI::WForm?"form":w.type==HagUI::WSubPage?"submenu":"button";
                out+=(out=="ok"?" ":std::string(1,char(31)))+path+" | "+kind+" | "+w.text;
            }
        }
        return out;
    }
    if(verb=="list"){std::string out="ok";for(auto& c:Commands())out+=" "+c.id;return out;}
    if(verb!="help" && verb!="run" && verb!="choices" && verb!="read" && verb!="preset" && verb!="catalog" && verb!="visible")return "err expected list, help, read, choices, preset or run";
    in>>id;auto all=Commands();auto it=std::ranges::find(all,id,&Command::id);
    if(it==all.end())return "err unregistered action";
    auto& a=it->spec;
    if(verb=="help"){
        std::string s=std::string("ok ")+a.label+(a.needsSave?" [save]":" [global]");
        s+=a.ipcOnly?" ui=none":a.rows?" ui=catalog":a.titleRow?" ui=title":a.count==1 && a.inputs[0].type==Boolean && a.inputs[0].source.pull?" ui=toggle":a.count?" ui=form":" ui=button";
        if(a.followup)s+=" followup="+std::string(a.followup);
        if(a.menu)s+=" menu="+std::string(a.menu);
        if(a.preset)s+=" preset="+std::string(a.presetLabel);
        for(unsigned i=0;i<a.count;++i){auto& f=a.inputs[i];s+=" | "+std::string(f.key)+":"+typeName(f.type);
            if(f.type==Integer)s+="["+std::to_string(f.min)+".."+std::to_string(f.max)+"]";
            if(f.options)s+="{"+std::string(f.options)+"}";
            if(f.initial)s+="="+std::string(f.initial);
            if(f.source.pull || f.type>=Choice)s+=" (refresh="+std::to_string(f.source.when)+", dependencies="+std::to_string(f.source.dependencies)+", intervalMs="+std::to_string(f.source.intervalMs)+")";}
        return s;
    }
    int choice=verb=="catalog"?CatalogRequest:verb=="visible"?VisibilityRequest:verb=="preset"?-2:-1;
    if(choice==-2 && !a.preset)return "err no preset registered";
    if(verb=="choices" || verb=="read"){in>>field;for(unsigned i=0;i<a.count;++i)if(field==a.inputs[i].key)choice=i;
        if(choice<0 || (a.inputs[choice].type<Choice && (verb=="choices" || !a.inputs[choice].source.pull)))return "err field has no such source";}
    std::vector<std::string> v(a.count);std::set<std::string> seen;
    for(unsigned i=0;i<a.count;++i)if(a.inputs[i].initial)v[i]=a.inputs[i].initial;
    while(in>>std::ws && in.peek()!=EOF){
        if(!(in>>std::quoted(arg)))return "err malformed arguments";
        auto eq=arg.find('=');auto k=arg.substr(0,eq);
        if(eq==arg.npos || !seen.insert(k).second)return "err expected unique key=value arguments";
        if(choice>=0 && k=="search"){search=arg.substr(eq+1);continue;}
        unsigned i=0;while(i<a.count && k!=a.inputs[i].key)++i;
        if(i==a.count)return "err unknown input: "+k;
        v[i]=arg.substr(eq+1);
    }
    auto j=Submit(*it,std::move(v),choice,search);
    std::unique_lock lock(j->mutex);
    if(!j->changed.wait_for(lock,std::chrono::seconds(8),[&]{return j->started||j->done;}) && !j->started){
        j->done=true;j->result={false,"game did not tick; action cancelled"};
    }
    j->changed.wait(lock,[&]{return j->done;});
    auto s=j->result.text;
    // Providers may append a display label. IPC choice values remain stable IDs.
    if((verb=="choices" || verb=="read") && choice>=0 && a.inputs[choice].type>=Choice && j->result.ok){
        std::istringstream rows(s);s.clear();
        for(std::string row;std::getline(rows,row);){if(!s.empty())s+='\n';s+=row.substr(0,row.find('\t'));}
    }
    std::replace(s.begin(),s.end(),'\n',char(31));std::replace(s.begin(),s.end(),'\r',' ');
    return std::string(j->result.ok?"ok ":"err ")+s;
}
void serve() {
    // Only this Windows user, only this machine. No debug IPC service is needed.
    HANDLE token; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return;
    DWORD n=0;GetTokenInformation(token,TokenUser,nullptr,0,&n);std::vector<char> info(n);
    bool got=GetTokenInformation(token,TokenUser,info.data(),n,&n)!=FALSE;CloseHandle(token);if(!got)return;
    wchar_t* sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid,&sid))return;
    auto acl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR sd;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&sd,nullptr))return;
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    HANDLE pipe=CreateNamedPipeW(LR"(\\.\pipe\SoW.ModActions.v1)",PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,8192,0,&sa);
    LocalFree(sd);if(pipe==INVALID_HANDLE_VALUE)return;
    for(;;){
        if(!ConnectNamedPipe(pipe,nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED)break;
        std::string line;char buf[1024];DWORD read=0;
        while(line.size()<=8192 && line.find('\n')==line.npos && ReadFile(pipe,buf,sizeof(buf),&read,nullptr) && read)line.append(buf,read);
        std::string reply="err invalid request";
        auto nl=line.find('\n');
        if(nl!=line.npos && nl<=8192 && line.find('\0')==line.npos)try{reply=dispatch(line.substr(0,nl));}catch(...){reply="err dispatch failed";}
        reply+='\n';DWORD written;WriteFile(pipe,reply.data(),DWORD(reply.size()),&written,nullptr);
        FlushFileBuffers(pipe);DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
}
}
void StartPipe(){static std::once_flag once;std::call_once(once,[]{std::thread(serve).detach();});}
}
