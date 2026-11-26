#pragma once
#include <cstdint>

namespace sowmod::actions {
inline constexpr unsigned SchemaVersion=4;
using Emit=void(*)(void*,const char*);
using Choices=bool(*)(unsigned field,const char* const* values,const char* search,Emit,void*);
enum Refresh : unsigned { OnOpen=1, OnInputs=2, AfterRun=4, Periodic=8, OnDraft=16 };
struct Source {
    Choices pull=nullptr;
    unsigned when=OnOpen;
    std::uint32_t dependencies=0; // bit i watches input i
    unsigned intervalMs=0;
};
enum Type : unsigned { Text, Integer, Boolean, Choice, MultiChoice };
struct Input {
    const char* key;
    const char* label;
    Type type;
    const char* initial=nullptr; // nullptr = required, otherwise IPC may omit it
    int min=0,max=0;
    const char* options=nullptr; // comma-separated stable IDs, or use Choices below
    Source source{}; // choices: available IDs; scalar: current value
};
// Strings are borrowed for the duration of the call only. No STL objects cross DLLs.
using Run=bool(*)(const char* const* values,Emit,void*);
using EmitRow=void(*)(void*,const char* id,const char* label,const char* const* facets);
using Rows=bool(*)(EmitRow,void*);
struct Action {
    const char* key;
    const char* label;
    bool needsSave;
    const Input* inputs;
    unsigned count;
    Run run;
    Choices choices=nullptr;
    const char* menu=nullptr; // slash-separated submenu labels relative to this mod
    Run preset=nullptr; // optional subfunction: emit key=value lines to populate the draft
    const char* presetLabel=nullptr;
    bool ipcOnly=false,titleRow=false;
    bool (*show)()=nullptr;
    const char* const* facets=nullptr;
    unsigned facetCount=0;
    Rows rows=nullptr;
    const char* followup=nullptr;
    constexpr Action IpcOnly() const {auto a=*this;a.ipcOnly=true;return a;}
    constexpr Action TitleRow(bool(*visible)()) const {auto a=*this;a.titleRow=true;a.show=visible;return a;}
    template<unsigned N> constexpr Action Catalog(const char* const(&names)[N],Rows source) const {
        auto a=*this;a.facets=names;a.facetCount=N;a.rows=source;return a;
    }
    constexpr Action FollowedBy(const char* key) const {auto a=*this;a.followup=key;return a;}
};
struct Registrar {
    unsigned version;
    void* context;
    bool (*add)(void*,const Action*);
    bool (*bindReconcile)(void*,const char*,std::uint32_t,Run);
    bool Add(const Action& action) const {return add(context,&action);}
    bool Reconcile(const char* key,std::uint32_t onChange,Run rule) const {
        return version>=4 && bindReconcile && bindReconcile(context,key,onChange,rule);
    }
};
using Register=bool(*)(const Registrar*);
}
