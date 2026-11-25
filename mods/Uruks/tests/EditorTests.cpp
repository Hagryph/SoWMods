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
        Put(member.at(0x3a0), army.header.at()); // Non-null authored builder, like Butcher_Ambush.
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
        Check(std::none_of(snapshot.fields.begin(),snapshot.fields.end(),[](const auto& f){return f.kind==Kind::View || f.kind==Kind::Picker;}));
        auto level = std::find_if(snapshot.fields.begin(), snapshot.fields.end(), [](const auto& f) { return f.kind == Kind::Level; });
        Check(level != snapshot.fields.end() && level->value == "11" && level->choices.empty());
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
        auto& list=field(Kind::Traits);
        Check(list.choices.empty()); // an equipped picker is not a source of eligibility
        Check(Enabled(list) && !SetOption(nullptr,list.id,1,nullptr,0));
        Check(SetApply(nullptr,list.id,nullptr,0));
        HagUI_PopupState blocked{};
        State(nullptr,0,0,&blocked);
        Check(blocked.dirty && !blocked.canSave); // fixture has no supported runtime
        Discard(nullptr);
        list.choices.clear();
        g_snapshot.traitRules={};
        Check(Enabled(list) && SetApply(nullptr,list.id,nullptr,0));
        Discard(nullptr);
        list.choices={Choice{11,"A"},Choice{12,"B"},Choice{13,"C"}};
        auto& rules=g_snapshot.traitRules;
        rules.groups={{11,{77}},{12,{}},{13,{77}}};
        rules.sources={{1,{{11,12,13}}},{1,{{12}}}};
        Check(rules.permits({12,11}) && !rules.permits({11,13}) && !rules.permits({11,11}));
        engine::TraitRules alternatives{{{1,{}},{2,{}},{3,{}}},{{2,{{1,2},{2,3}}}}};
        Check(!alternatives.permits({1,3}) && alternatives.permits({1,2}) && alternatives.permits({2,3}));
        alternatives.sources={{1,{{1,2,3}}}};
        Check(!alternatives.permits({1,2}) && alternatives.permits({3}));
        Check(!list.choices[0].selected && HandlesSet(nullptr,list.id));
        uint64_t ab[]{1,2},ac[]{1,3};
        Check(SetOption(nullptr,list.id,2,ab,1) && !SetOption(nullptr,list.id,3,ab,1));
        Check(!SetApply(nullptr,list.id,ac,2) && !g_draft.dirty());
        Check(SetApply(nullptr,list.id,ab,2) && g_draft.ready());
        Check(ReadChoice(nullptr,list.id,0,&choice) && choice.selected);
        Check(SetApply(nullptr,list.id,nullptr,0) && g_draft.edits.at(Kind::Traits).second.pickers.empty());
        Discard(nullptr);
        Check(Stamp(member.at())==original);
        char message[192]{};
        auto appearanceId=candidate(Kind::Appearance,9900);
        Check(Scene(nullptr,appearanceId,9900,message)==1 && queuedFn==RunScene);
        Check(wantedScene.choice==9900 && !g_draft.dirty() && !g_pending);
        // Closing before the game thread gets a tick must cancel, not draw a stale model.
        Check(Scene(nullptr,0,0,message)==0 && !wantedScene.session);
        queuedFn(nullptr);
        Check(!sceneQueued && !lease.actor && Stamp(member.at())==original);
        queuedFn=nullptr;
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
        // Constrained generation is independent of the old captain's trait list.
        recipe::Catalog db;
        db.classes={100,101}; db.rarities={200,201}; db.epicCaps={{200,0},{201,1}};
        db.definitions={20,21,22,23,24,25}; db.epics={22,25};
        db.categories={{20,0},{21,1},{22,3},{25,3}};
        db.producers={{20,{{40,{9},{100},{}},{41,{8},{100},{}}}},
                      {21,{{42,{9},{},{}}}},{22,{{43,{},{},{}}}},{25,{{44,{},{},{}}}}};
        recipe::Recipe request{100,201};
        request.traits[0]={20}; request.traits[1]={21}; request.traits[3]={22};
        auto legal=[](const auto&){return true;};
        Check(recipe::resolve(db,request,[](const auto& ids){return std::ranges::find(ids,41)!=ids.end();}));
        request.specialization=101; Check(!recipe::resolve(db,request,legal));
        request.specialization=100; request.rarity=200; Check(!recipe::resolve(db,request,legal));
        request.rarity=201; request.traits[3].push_back(25); Check(!recipe::resolve(db,request,legal));
        request.traits[3].pop_back();
        std::vector<Ptr> keep;
        Check(recipe::retained(db,{{40,20},{50,23},{51,0},{42,21}},keep));
        Check(keep==std::vector<Ptr>({50,51}));
        Check(!recipe::retained(db,{{50,999}},keep));
        Ptr generated[]={20,21,22,23}; Check(recipe::matches(db,request,generated,4));
        generated[2]=25; Check(!recipe::matches(db,request,generated,4));
        db.trees={{300,301},{310,311}}; db.types={{300,302},{310,312}}; db.tribes[400]={49};
        db.kits[100].emplace_back().allowedTypes={302};
        db.kits[101].emplace_back().allowedTypes={312};
        db.managedTraits={24}; db.managedPickers={49}; db.categories[24]=3;
        db.producers[24]={{49,{},{},{}}};
        request.base=300;request.tribe=400;
        Ptr full[]={20,21,22,23,24};
        Check(recipe::matches(db,request,full,5,{24}) && !recipe::matches(db,request,full,5));
        Check(!recipe::matches(db,request,full,4,{24}));
        request.traits[3].push_back(24); Check(!recipe::resolve(db,request,legal));
        Check(recipe::retained(db,{{49,0},{50,23}},keep) && keep==std::vector<Ptr>{50});
        g_snapshot.recipeCatalog=db;
        auto make=[&](Kind k,const char* label,std::vector<Choice> c){
            auto& f=Add(g_snapshot,label,"",k);f.choices=std::move(c);return f.id;
        };
        auto spec=make(Kind::Class,"Class",{{100,"A"},{101,"B"}});
        auto rare=make(Kind::Rarity,"Rarity",{{200,"Common"},{201,"Epic"}});
        auto base=make(Kind::Base,"Base",{{300,"Archer"},{310,"Defender"}});
        auto tribe=make(Kind::Tribe,"Tribe",{{400,"Dark"}});
        auto immunity=make(Kind::Immunities,"Immunities",{{20,"I"}});
        auto bonus=make(Kind::Bonuses,"Bonuses",{{22,"Epic trait"}});
        auto regen=make(Kind::Regenerate,"Replacement",{});
        Check(Apply(nullptr,spec,1) && Apply(nullptr,rare,1));
        Check(!ReadChoice(nullptr,base,1,&choice) && !Apply(nullptr,base,2));
        Check(!Enabled(field(Kind::Regenerate)) && Apply(nullptr,base,1) && Apply(nullptr,tribe,1));
        auto supplyProfile=[&](){
            auto job=std::unique_ptr<Task>(static_cast<Task*>(queuedContext)); queuedContext=nullptr;
            Check(job && job->op==Op::Profile && !g_draft.profile);
            auto profile=std::make_shared<engine::GenerationPlan>();
            profile->eligible={{20,{41}},{22,{43}}};
            profile->singles={20,22};
            g_draft.profile=profile;g_pending=false;
        };
        supplyProfile();
        uint64_t first[]{1};
        Check(SetApply(nullptr,immunity,first,1) && !SetOption(nullptr,bonus,1,nullptr,0));
        Check(!Apply(nullptr,spec,2) && !g_draft.ready());
        Check(Apply(nullptr,rare,2) && !SetOption(nullptr,bonus,1,nullptr,0));
        supplyProfile();
        Check(!g_draft.edits.contains(Kind::Immunities) && SetOption(nullptr,bonus,1,nullptr,0));
        Check(SetApply(nullptr,bonus,first,1));
        Check(!SetOption(nullptr,immunity,1,nullptr,0) && queuedFn==CheckOptions);
        auto batchContext=std::unique_ptr<std::shared_ptr<OptionBatch>>(static_cast<std::shared_ptr<OptionBatch>*>(queuedContext));
        queuedContext=nullptr;
        Check(latestBatch==*batchContext && latestBatch->recipe.traits[3]==std::vector<Ptr>{22});
        latestBatch->ready=true;latestBatch->valid=true;
        Check(!SetOption(nullptr,immunity,1,nullptr,0));
        latestBatch->allowed.insert(20);
        Check(SetOption(nullptr,immunity,1,nullptr,0) && SetApply(nullptr,immunity,first,1));
        Check(candidate(Kind::Regenerate,10100)==regen && Apply(nullptr,regen,10100));
        State(nullptr,0,0,&popup);Check(!popup.canSave);
        State(&recipePage,0,0,&popup);Check(popup.canSave);
        Save(nullptr);Check(!queuedContext);
        Check(SetApply(nullptr,bonus,nullptr,0) && !g_draft.ready());
        Check(!g_draft.edits.contains(Kind::Regenerate) && Stamp(member.at())==original);
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
