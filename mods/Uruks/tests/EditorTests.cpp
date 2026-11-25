// Compile the implementation into this test to exercise the private snapshot and memory boundary.
#include "../src/UrukEditor.cpp"
#include <array>
#include <iostream>
#include <source_location>

namespace uruk { void Log(const char*) {} }
void Check(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Editor test failed at line " + std::to_string(location.line()));
}
template<class T> void Put(uruk::Ptr address, T value) { std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value)); }
struct Memory {
    std::vector<unsigned char> bytes;
    explicit Memory(size_t count) : bytes(count) {}
    uruk::Ptr at(size_t offset = 0) { return reinterpret_cast<uruk::Ptr>(bytes.data()) + offset; }
};
struct FakeRecord {
    Memory header{0x28}, descriptors{16}, values{8};
    explicit FakeRecord(const char* name) { Put(header.at(0x20), reinterpret_cast<uruk::Ptr>(name)); }
    void Field(const char* name, uint64_t value, uint8_t type = 10) {
        Put<uint8_t>(header.at(), 1);
        Put(header.at(8), descriptors.at());
        Put(header.at(0x10), values.at());
        Put(descriptors.at(), uruk::FieldHash(name));
        Put(descriptors.at(0xa), type);
        Put<uint16_t>(descriptors.at(0xc), 1);
        Put(values.at(), value);
    }
};
uint64_t session = 1;
uint64_t Session() { return session; }
HagUI_GameTaskFn queuedFn{};
void* queuedContext{};
bool CaptureTask(HagUI_GameTaskFn fn,void* context) {
    if (queuedContext) return false;
    queuedFn=fn; queuedContext=context; return true;
}
int main() {
    try {
        using namespace uruk;
        Check(Read<Ptr>(0) == 0);
        Check(Read<Ptr>(1) == 0);
        Check(FieldHash("CombatProp") == 0x9ce11687);
        Check(FieldHash("ArmorSlot") == FieldHash("armorslot"));
        FakeRecord picker("Picker_Test"), trait("Test_Ability"), army("Faction_Test");
        picker.Field("ExclusiveGroup", 123);
        Check(Cell(picker.header.at(), "ExclusiveGroup") == 123);
        Check(Cell(picker.header.at(), "ExclusiveGroup", 1) == 0);
        Check(Cell(picker.header.at(), "ExclusiveGroup", 0, 3, 77) == 77);
        Memory database(0x88), category(0x58);
        const char* categoryName = "Faction/Traits/Picker";
        Put<uint32_t>(database.at(0x48), 1);
        Put(database.at(0x50), category.at());
        Put(category.at(), reinterpret_cast<Ptr>(categoryName));
        Put<uint32_t>(category.at(0x28), 1);
        Put(category.at(0x38), picker.header.at());
        Put(picker.header.at(0x18), category.at());
        Check(LoadCatalog(database.at()).at(categoryName).size() == 1);
        Memory member(0x528), slots(0x20), appearance(0x1c0);
        Put(member.at(0x178), army.header.at());
        Put(member.at(0x268), army.header.at());
        Put<uint64_t>(member.at(0x168), 42);
        Put(member.at(0x1e0), slots.at());
        Put(member.at(0x1e8), slots.at(0x20));
        Put(member.at(0x2d8), appearance.at());
        Put(slots.at(), picker.header.at());
        Put(slots.at(8), trait.header.at());
        Put<uint32_t>(member.at(0x4b8), 10);
        Check(VectorSize(member.at(0x1e0), 0x20) == 1);
        Snapshot snapshot;
        snapshot.session = 1;
        snapshot.selected = {army.header.at(), 0, 42};
        snapshot.targets.push_back({snapshot.selected, member.at(), "Test captain"});
        Catalog catalog;
        catalog["Faction/Traits/Picker"].push_back({picker.header.at(), "Picker_Test"});
        Capture(snapshot, catalog);
        Check(snapshot.fields.size() > 75);
        auto level = std::find_if(snapshot.fields.begin(), snapshot.fields.end(), [](const auto& f) { return f.kind == Kind::Level; });
        Check(level != snapshot.fields.end() && level->value == "11" && level->choices.empty());
        auto slot = std::find_if(snapshot.fields.begin(), snapshot.fields.end(), [](const auto& f) { return f.kind == Kind::Picker; });
        Check(slot != snapshot.fields.end() && slot->value.find("Test_Ability") != std::string::npos);
        Check(InCatalog(catalog, "Faction/Traits/Picker", picker.header.at()));
        Check(!InCatalog(catalog, "Faction/Personalities", picker.header.at()));
        HagUIAPI api{};
        api.SaveSession = Session;
        api.QueueGameTask = CaptureTask;
        g_ui = &api;
        g_snapshot = snapshot;
        HagUI_Property property{};
        Check(ReadField(nullptr, 0, &property));
        Check(property.editable);
        Check(ChoiceCount(nullptr, property.id) == 1);
        HagUI_Choice choice{};
        Check(ReadChoice(nullptr, property.id, 0, &choice) && choice.id == 1);
        Check(!Apply(nullptr, property.id, 999));
        // Supply generated candidates at the snapshot boundary; no game DLL or native setter runs here.
        g_snapshot.mayGenerate=true;
        auto field=[&](Kind k)->Field& {
            return *std::find_if(g_snapshot.fields.begin(),g_snapshot.fields.end(),[&](const auto& f){return f.kind==k;});
        };
        auto candidate=[&](Kind k,uint64_t ticket) {
            auto& f=field(k); f.choices={Choice{77,"Candidate"}};
            f.choices.front().ticket=ticket;
            return f.id;
        };
        auto original=Stamp(member.at());
        uint64_t titleId=candidate(Kind::Role,10000);
        Close(nullptr,titleId);
        Check(!Apply(nullptr,titleId,10000) && !g_draft.dirty());
        candidate(Kind::Role,10001);
        Check(Apply(nullptr,titleId,10001) && !g_draft.ready());
        HagUI_PopupState popup{};
        State(nullptr,field(Kind::Name).id,0,&popup);
        Check(popup.dirty && !popup.canSave && popup.needsRepair);
        Check(!Apply(nullptr,property.id,1)); // dirty drafts cannot change targets
        Check(Apply(nullptr,candidate(Kind::Name,10002),10002));
        Check(!g_draft.ready());
        Check(Apply(nullptr,candidate(Kind::Appearance,10003),10003) && g_draft.ready());
        Check(Apply(nullptr,candidate(Kind::Role,10004),10004) && !g_draft.ready());
        Check(field(Kind::Name).choices.empty() && field(Kind::Appearance).choices.empty());
        Discard(nullptr);
        Check(!g_draft.dirty() && Stamp(member.at())==original);
        auto& xp=field(Kind::Level);
        xp.choices={Choice{12,"13"},Choice{84,"85"}};
        xp.choices[1].disabled="Above cap";
        Check(!Apply(nullptr,xp.id,2));
        Check(Apply(nullptr,xp.id,1) && g_draft.ready());
        Check(Read<unsigned>(member.at(0x4b8))==10);
        Check(!Enabled(field(Kind::Advance)));
        Discard(nullptr);
        g_snapshot.mayAdvance=true;
        Check(Apply(nullptr,candidate(Kind::Advance,10005),10005));
        Check(!Enabled(field(Kind::Traits)));
        Check(!queuedContext);
        Save(nullptr);
        Check(queuedContext && queuedFn==Run && g_pending);
        auto* task=static_cast<Task*>(queuedContext);
        Check(task->op==Op::Save && task->selected==snapshot.selected && task->draft.edits.count(Kind::Advance));
        ++session;
        queuedFn(queuedContext); // load change before dispatch must avoid all native calls
        Check(!g_pending && Stamp(member.at())==original);
        --session;
        queuedContext=nullptr;
        Discard(nullptr);
        Put<unsigned>(member.at(0x4b8),11);
        Check(original!=Stamp(member.at()));
        session = 2;
        Check(ChoiceCount(nullptr, property.id) == 0);
        Check(!Apply(nullptr, property.id, 1));
        Check(Count(nullptr) == 1);
        Check(ReadField(nullptr, 0, &property) && !property.editable);
        Put(member.at(0x1e8), slots.at(1));
        Check(VectorSize(member.at(0x1e0), 0x20) == 0);
        std::cout << "Uruk editor snapshot, types and stale-session tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
