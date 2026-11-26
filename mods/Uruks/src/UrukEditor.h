#pragma once
#include "HagUIAPI.h"
#include "ModActions.h"
namespace uruk { bool RegisterEditorActions(const sowmod::actions::Registrar&); }
namespace uruk { void AddUrukEditor(int parent, HagUIAPI* ui); }
