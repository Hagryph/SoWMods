#pragma once
#include <cstdint>

namespace sowmod::actions {
inline constexpr unsigned SchemaVersion=2;
using Emit=void(*)(void*,const char*);
using Choices=bool(*)(unsigned field,const char* const* values,const char* search,Emit,void*);
enum Refresh : unsigned { OnOpen=1, OnInputs=2, AfterRun=4, Periodic=8 };
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
};
struct Registrar {
    unsigned version;
    void* context;
    bool (*add)(void*,const Action*);
    bool Add(const Action& action) const {return add(context,&action);}
};
using Register=bool(*)(const Registrar*);
}
