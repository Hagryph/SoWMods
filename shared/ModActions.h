#pragma once
#include <cstdint>

namespace sowmod::actions {
enum Type : unsigned { Text, Integer, Boolean, Choice, MultiChoice };
struct Input {
    const char* key;
    const char* label;
    Type type;
    const char* initial=nullptr; // nullptr = required, otherwise IPC may omit it
    int min=0,max=0;
    const char* options=nullptr; // comma-separated stable IDs, or use Choices below
};
using Emit=void(*)(void*,const char*);
// Strings are borrowed for the duration of the call only. No STL objects cross DLLs.
using Run=bool(*)(const char* const* values,Emit,void*);
using Choices=bool(*)(unsigned field,const char* const* values,const char* search,Emit,void*);
struct Action {
    const char* key;
    const char* label;
    bool needsSave;
    const Input* inputs;
    unsigned count;
    Run run;
    Choices choices=nullptr;
};
struct Registrar {
    unsigned version;
    void* context;
    bool (*add)(void*,const Action*);
    bool Add(const Action& action) const {return add(context,&action);}
};
using Register=bool(*)(const Registrar*);
}
