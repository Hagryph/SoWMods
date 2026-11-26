# Registered mod actions

The loader requires a mod to export `SoWMod_RegisterActionsV3` before calling its optional
`SoWMod_Init`. Registration describes the actions; it must not read game state. Unregistered
legacy DLLs are logged and their init is skipped. Action keys are namespaced by the lowercase DLL stem.

Include `shared/ModActions.h`. Supply static-lifetime input descriptors, a handler, and optionally
a choice provider. Return false if any registration fails. Actions appear directly on the mod tab by default. Only an explicit `menu` creates a submenu.
No-input actions become buttons; a Boolean input with a current-value source becomes a checkbox.
`.Catalog(facetNames, rows)` supplies stable item IDs, labels, and facet values for the searchable
item list. `.TitleRow(predicate)` places a visible action flush right on the title row.
`.IpcOnly()` registers a handler without a control. `.FollowedBy("save")` binds a second button
on the same form to the preview's returned key=value inputs; changing the draft invalidates it.
Use IPC `layout` to inspect the generated hierarchy without screen capture. Paths are relative to the mod; IPC IDs do not change when moving an action.
Submods do not create windows, pages, or widgets. Both standalone and aggregate builds run the
submod boundary check.

```cpp
extern "C" __declspec(dllexport) bool SoWMod_RegisterActionsV3(
    const sowmod::actions::Registrar* r) {
    using namespace sowmod::actions;
    static constexpr Input args[]={{"enabled","Enabled",Boolean,"false"}};
    return r && r->version==SchemaVersion && r->Add({"set_feature","Enable feature",false,args,1,
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

`Input::source` binds a single widget to a provider with the same signature as a choice
provider. For choices it emits available IDs; for scalar inputs it emits the current value.
Register `OnOpen`, `OnInputs`, `AfterRun`, and/or `Periodic`; for `OnInputs` supply a bitmask
of earlier inputs to watch, for `Periodic` a visible-form interval of at least 250 ms.
Save-scoped reads are scheduled on the game thread. IPC `read ACTION FIELD` pulls the same
source on demand. `help` exposes the menu, triggers, dependencies and interval.
An optional action `preset` callback emits `key=value` lines to fill the form without
running the command. For example, `preset uruks.preview captain=Faction_MinasIthil:271`
returns the editable template. The same callback backs **Take Over Template** in the UI.

The HagIPC PowerShell client routes `mod` commands to the loader's current-user local named pipe.
This does not start or require the low-level TCP debugger.

```powershell
.\HagIPC\tools\hagipc.ps1 'mod list'
.\HagIPC\tools\hagipc.ps1 'mod run loader.load_save'
.\HagIPC\tools\hagipc.ps1 'mod run loader.load_status'
.\HagIPC\tools\hagipc.ps1 'mod read uruks.set_recruitment enabled'
.\HagIPC\tools\hagipc.ps1 'mod help uruks.preview'
.\HagIPC\tools\hagipc.ps1 'mod choices uruks.preview captain search=MinasIthil'
.\HagIPC\tools\hagipc.ps1 'mod run uruks.inspect captain=Faction_MinasIthil:271'
.\HagIPC\tools\hagipc.ps1 'mod run uruks.set_recruitment enabled=true'
```

Use `field=value` arguments, quoting the whole argument for values containing spaces.
Campaign loading is asynchronous. At cold start the loader asks the game to activate the sole
enumerated Steam user, waits for native save metadata, and continues the existing SinglePlayer
campaign. No key input, menu callbacks or desktop capture are used. It will not create a new
game or guess between multiple users. A queued reply is not success: check status for the
registered world session, or run `tools/sow_load_save.ps1` to wait for it.
Multi-choice fields take comma-separated record IDs. Captain preview returns a ticket;
only `run uruks.save ticket=...` commits it. Generating a new preview invalidates the
previous ticket. Changing the save, target identity, or captain baseline invalidates the commit.

`tools/test_mod_actions.ps1 -DiscoveryOnly` checks discovery and rejected inputs.
With a save open, the default test previews the Defender/Tank/Warmonger recipe and checks
the original captain was unchanged. `-SaveCaptain` explicitly commits and checks all
20 requested traits. It does not spawn items or unlock the ring.
