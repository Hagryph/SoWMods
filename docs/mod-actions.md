# Registered mod actions

The loader requires a mod to export `SoWMod_RegisterActions` before calling its optional
`SoWMod_Init`. Registration describes the actions; it must not read game state. Unregistered
legacy DLLs are logged and their init is skipped. Action keys are namespaced by the lowercase DLL stem.

Include `shared/ModActions.h`. Supply static-lifetime input descriptors, a handler, and optionally
a choice provider. Return false if any registration fails. Each mod gets a **Mod Actions** submenu:
the loader creates checkboxes, integer inputs, searchable choice and multi-choice pickers, and text
fields from these descriptors. Existing specialized editor views remain available.

```cpp
extern "C" __declspec(dllexport) bool SoWMod_RegisterActions(
    const sowmod::actions::Registrar* r) {
    using namespace sowmod::actions;
    static constexpr Input args[]={{"enabled","Enabled",Boolean,"false"}};
    return r && r->version==1 && r->Add({"set_feature","Enable feature",false,args,1,
        [](const char* const* v,Emit emit,void* ctx) {
            // Use the same implementation as the rest of the mod.
            return UpdateFeature(std::string_view(v[0])=="true",emit,ctx);
        }});
}
```

The handler receives inputs in registration order. A null default means required; use an empty
default for an optional empty selection. Dynamic choices emit one stable ID per call. Validate
dynamic IDs against the live catalog in the handler; the choice provider is for discovery, not
authorization. Save-scoped actions run on the game queue and recheck the session before execution.
Global actions must be thread-safe and must not call native gameplay functions.

The HagIPC PowerShell client routes `mod` commands to the loader's current-user local named pipe.
This does not start or require the low-level TCP debugger.

```powershell
.\HagIPC\tools\hagipc.ps1 'mod list'
.\HagIPC\tools\hagipc.ps1 'mod help uruks.preview'
.\HagIPC\tools\hagipc.ps1 'mod choices uruks.preview captain search=MinasIthil'
.\HagIPC\tools\hagipc.ps1 'mod run uruks.inspect captain=Faction_MinasIthil:271'
.\HagIPC\tools\hagipc.ps1 'mod run uruks.set_recruitment enabled=true'
```

Use `field=value` arguments, quoting the whole argument for values containing spaces.
Multi-choice fields take comma-separated record IDs. Captain preview returns a ticket;
only `run uruks.save ticket=...` commits it. Generating a new preview invalidates the
previous ticket. Changing the save, target identity, or captain baseline invalidates the commit.

`tools/test_mod_actions.ps1 -DiscoveryOnly` checks discovery and rejected inputs.
With a save open, the default test previews the Defender/Tank/Warmonger recipe and checks
the original captain was unchanged. `-SaveCaptain` explicitly commits and checks all
20 requested traits. It does not spawn items or unlock the ring.
