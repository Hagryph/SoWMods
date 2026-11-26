// Compile the implementation into this test to exercise the private snapshot and memory boundary.
#include "../src/UrukEditor.cpp"
#include <array>
#include <iostream>
#include <source_location>

namespace uruk { std::string lastReport; void Log(const char*) {} bool WriteGenerationLog(const char* s) {lastReport=s;return true;} }
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
        {
            constexpr Ptr defender=101,archer=102;
            auto nativeCap=[](Ptr,bool follower,bool* capped){*capped=follower;return follower?2u:84u;};
            bool capped=false;
            Check(engine::detail::queryTreeCap(nativeCap,defender,true,&capped,defender)==84 && !capped);
            Check(engine::detail::queryTreeCap(nativeCap,defender,true,&capped,0)==2 && capped);
            Check(engine::detail::queryTreeCap(nativeCap,archer,true,&capped,defender)==2 && capped);
            Check(engine::detail::queryTreeCap(nativeCap,archer,false,&capped,defender)==84 && !capped);
        }
        Check(Read<Ptr>(0) == 0);
        Check(Read<Ptr>(1) == 0);
        Check(FieldHash("CombatProp") == 0x9ce11687);
        Check(FieldHash("ArmorSlot") == FieldHash("armorslot"));
        FakeRecord picker("Picker_Test"), trait("Test_Ability"), army("Faction_Test");
        FakeRecord localized("Test_MixedCase");
        const char* displayKey="IntRes_Trait_Test";
        localized.Field("test",reinterpret_cast<Ptr>(displayKey),4);
        Put<uint32_t>(localized.descriptors.at(),0xf51aff36);
        auto lookup=[](const char* key)->const wchar_t* {
            return strcmp(key,"IntRes_Trait_Test")==0 ? L"Frostsch\u00e4den" : nullptr;
        };
        Check(TraitLabel(localized.header.at(),lookup)=="Frostsch\xc3\xa4" "den");
        Check(TraitLabel(localized.header.at(),[](const char*){return L"{Color_Epic}Epic Determination</font>";})=="Epic Determination");
        Check(TraitLabel(localized.header.at(),[](const char*){return L"[ERROR: StringDatabase Not Initialized]";})=="Test Mixed Case");
        Check(TraitLabel(localized.header.at(),[](const char*){return L"IntRes_Trait_Test";})=="Test Mixed Case");
        Check(TraitLabel(trait.header.at(),lookup)=="Test Ability");
        FakeRecord demo("DEMO_ImmuneToCurse"), demoPicker("Picker_dEmO_ImmuneToCurse"), curse("Immune_Curse");
        Check(TraitLabel(demo.header.at(),lookup).empty());
        Check(PickerLabel(demoPicker.header.at()).empty());
        Check(TraitLabel(curse.header.at(),lookup)=="Immune Curse");
        Check(!DemoTraitId("Picker_Demoralize"));
        FakeRecord bloodlust("Hate_EnragedOrcs"), mercBloodlust("Easterling_Hate_EnragedOrcs");
        auto bloodlustLookup=[](const char*) { return L"Bloodlust"; };
        for (auto* record:{&bloodlust,&mercBloodlust}) {
            record->Field("test",reinterpret_cast<Ptr>(displayKey),4);
            Put<uint32_t>(record->descriptors.at(),0xf51aff36);
        }
        Check(TraitLabel(bloodlust.header.at(),bloodlustLookup)=="Bloodlust");
        Check(TraitLabel(mercBloodlust.header.at(),bloodlustLookup).empty());
        Check(UrukTraitDefinition("DLC4_Hate_Frost"));
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
        Put(category.at(0x38), demoPicker.header.at());
        Put(demoPicker.header.at(0x18), category.at());
        Check(LoadCatalog(database.at()).at(categoryName).empty());
        Put(category.at(0x38), picker.header.at());
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
        {
            FakeRecord reward("BaseClass_Defender"),setup("Traits_Defender"),track("Wrecker"),tier(""),list("Wrecker"),
                grant("Wrecker"),group(""),picker("Picker_Wrecker"),option(""),ability("Wrecker");
            auto link=[](FakeRecord& from,unsigned hash,FakeRecord& to){
                from.Field("",to.header.at());Put<uint32_t>(from.descriptors.at(),hash);
            };
            link(reward,0xcca3a85c,setup);link(setup,0x11407c64,track);link(track,0x31775628,tier);
            tier.descriptors.bytes.resize(32);tier.values.bytes.resize(16);
            link(tier,0x4c13006d,list);
            Put<uint8_t>(tier.header.at(),2);
            Put<uint32_t>(tier.descriptors.at(16),0x5cd1372c);Put<uint8_t>(tier.descriptors.at(26),2);
            Put<uint16_t>(tier.descriptors.at(28),1);Put<uint16_t>(tier.descriptors.at(30),1);
            Put<float>(tier.values.at(8),0.05f);
            link(list,0x0a535c57,grant);link(grant,0x5b7f9956,group);link(group,0x7abf91f5,picker);
            link(picker,0x6bd57b1d,option);link(option,0xbcb47686,ability);
            recipe::Catalog c;c.definitions.insert(ability.header.at());
            LoadClassRewards(c,reward.header.at(),{10},true);
            Check(c.categories.at(ability.header.at())==2);
            recipe::Recipe r;r.base=11;
            engine::TraitRules draws;
            auto eligible=[&](Ptr p){draws.groups[p]={};return true;};
            recipe::addClassRewardDraws(c,r,draws,eligible);Check(draws.sources.empty());
            r.base=10;recipe::addClassRewardDraws(c,r,draws,eligible);Check(draws.permits({picker.header.at()}));
            Put<float>(tier.values.at(8),1.0f);
            recipe::Catalog fixed;LoadClassRewards(fixed,reward.header.at(),{10},true);Check(fixed.classRewards.empty());
        }
        {
            recipe::Catalog prereq;
            prereq.hiddenDependencies={{1,{90}},{2,{91}},{3,{90}}};
            prereq.hiddenProducers={{90,{2}},{91,{3}}};
            std::vector<Ptr> inputs{1};
            unsigned budget=16;
            auto needsThree=[](const auto& all){return all.size()==3;};
            Check(recipe::withHiddenGrants(prereq,inputs,budget,needsThree) && inputs==std::vector<Ptr>({1,2,3}));
            inputs={1};prereq.categories[91]=0;budget=16;
            Check(!recipe::withHiddenGrants(prereq,inputs,budget,needsThree) && inputs==std::vector<Ptr>({1}));
            std::set<Ptr> required,forbidden;
            recipe::Recipe req;req.base=10;req.specialization=11;req.rarity=12;
            prereq.routes[1]={1,{},{99},{98}};
            Check(recipe::markerInputs(prereq,req,inputs,required,forbidden) && required.contains(99) && forbidden.contains(98));
            prereq.routes[1].forbidden.push_back(req.specialization);
            Check(!recipe::markerInputs(prereq,req,inputs,required,forbidden));
        }
        {
            Ptr slot=1,owned=2;
            unsigned links=0;
            auto publish=[&](bool valid,bool canLink=true){
                return engine::detail::publishCandidate(1,owned,valid,[&]{return &slot;},
                    [&](Ptr old,Ptr next){Check(old==1 && next==2);++links;return canLink;});
            };
            Check(!publish(false) && slot==1 && owned==2 && !links);
            slot=3;Check(!publish(true) && slot==3 && owned==2 && !links);
            slot=1;Check(!publish(true,false) && slot==1 && owned==2 && links==1);
            Check(publish(true) && slot==2 && owned==1 && links==2);
        }
        {
            Ptr source=9,candidate=0;
            int binds=0;
            auto check=[&](Ptr member,Ptr expected){return expected==(member==1?source:candidate);};
            auto bind=[&](Ptr dst,Ptr src){Check(dst==2 && src==1);candidate=source;++binds;};
            auto transfer=[&]{return engine::detail::transferAttachment(1,2,source,candidate,check,bind);};
            Check(!transfer() && candidate==9 && binds==1);
            Check(!transfer() && binds==1);
            candidate=8;Check(transfer() && candidate==8 && binds==1);
            source=candidate=0;Check(!transfer() && !candidate && binds==1);
            source=9;Check(engine::detail::transferAttachment(1,2,source,candidate,check,[](Ptr,Ptr){})!=nullptr);
            candidate=9;Check(engine::detail::transferAttachment(1,2,source,candidate,
                [](Ptr,Ptr){return false;},bind)!=nullptr && binds==1);
        }
        {
            // The seat and candidate can stay valid while the native observer
            // expires, moves, or switches to another encounter during generation.
            auto attempt=[](Ptr source,Ptr next,Ptr seatTarget,bool observerOk,bool expectSwap,int expectedBinds) {
                Ptr slot=1,owned=2;int binds=0;
                const bool swapped=engine::detail::publishCandidate(1,owned,true,[&]{return &slot;},
                    [&](Ptr old,Ptr candidate) {
                        auto read=[&](Ptr m){return m==old?source:next;};
                        return !engine::detail::handoffSeatAttachment(old,candidate,9,read,
                            [&](Ptr m,Ptr target){return (m==old || observerOk) && read(m)==target;},
                            [&](Ptr m,Ptr target)->const char* {return m==old && target==seatTarget?nullptr:"wrong seat";},
                            [&](Ptr dst,Ptr src){Check(src==old && dst==candidate);next=source;++binds;});
                    });
                Check(swapped==expectSwap && slot==(swapped?2:1) && owned==(swapped?1:2) && binds==expectedBinds);
            };
            attempt(0,0,0,true,true,0);
            attempt(10,0,10,true,true,1);
            attempt(10,10,10,true,true,0);
            attempt(0,9,9,true,true,0);
            attempt(0,9,10,true,false,0);
            attempt(0,9,9,false,false,0);
            attempt(0,11,11,true,false,0);
            attempt(10,0,9,true,false,0);
            attempt(10,9,10,true,false,0);
        }
        {
            recipe::Catalog kits;recipe::Recipe request;request.base=1;request.specialization=2;
            kits.types[1]=3;
            recipe::ClassKit poison,curse,strip;
            poison.reward=10;poison.addsMarkers={2,20};
            curse.reward=11;curse.addsMarkers={2,21};
            strip.reward=12;strip.removesMarkers={20};
            kits.kits[2]={poison,curse,strip};
            std::set<Ptr> denied;
            Check(recipe::classRewardPool(kits,request,{2,20},{21},denied) && denied==std::set<Ptr>{11,12});
            kits.markerGroups={{20,{99}},{21,{99}}};
            Check(recipe::classRewardPool(kits,request,{2,20},{},denied) && denied==std::set<Ptr>{11,12});
            kits.kits[2].erase(kits.kits[2].begin());
            Check(!recipe::classRewardPool(kits,request,{2,20},{21},denied));
        }
        {
            recipe::Catalog gates;
            gates.categories={{101,1},{102,1},{103,5},{104,0},{105,3}};
            recipe::indexWeaknessGate(gates,201,{101},{103});
            recipe::indexWeaknessGate(gates,202,{102},{104});
            recipe::indexWeaknessGate(gates,203,{105},{103});
            recipe::indexWeaknessGate(gates,204,{101,105},{103});
            recipe::indexWeaknessGate(gates,205,{102},{103,104});
            recipe::Recipe request;
            Check(!recipe::waiveWeaknessGate(gates,request,201));
            request.traits[1]={101,102};
            Check(recipe::waiveWeaknessGate(gates,request,201) && recipe::waiveWeaknessGate(gates,request,205));
            Check(!recipe::waiveWeaknessGate(gates,request,202) && !recipe::waiveWeaknessGate(gates,request,203) &&
                  !recipe::waiveWeaknessGate(gates,request,204));
        }
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
        {
            auto kit=db;
            recipe::Recipe draft{100,201};draft.base=300;draft.tribe=400;
            auto route=[&](Ptr trait,unsigned bucket,Ptr picker) {
                kit.definitions.insert(trait);kit.categories[trait]=bucket;
                kit.producers[trait].push_back({picker,{},{},{}});
            };
            route(81,2,71);route(82,2,72);route(83,2,73);route(84,2,74);
            // Like the combat picker, 75 can yield either resistance or immunity.
            route(85,7,71);route(85,7,75);route(86,0,75);
            engine::TraitRules draws;draws.groups={{72,{}},{73,{}}};draws.sources={{1,{{72,73}}}};
            recipe::classifyClassDraws(kit,std::move(draws),{71});
            draft.traits[2]={81};Check(!recipe::resolve(kit,draft,legal)); // direct grant
            draft.traits[2]={84};Check(!recipe::resolve(kit,draft,legal)); // not in selected pool
            draft.traits[2]={82,83};Check(!recipe::resolve(kit,draft,legal)); // only one draw
            draft.traits[2]={82};draft.traits[0]={86};Check(recipe::resolve(kit,draft,legal));
            Check(recipe::optionalResult(kit,draft,{81,82,86}) && !recipe::optionalResult(kit,draft,{81,82,85}));
        }
        {
            recipe::Catalog gear;
            gear.weaponSlots={{10,{0}},{11,{0}},{12,{0}},{13,{0}},{20,{9}},{21,{9}},{30,{2}}};
            gear.bothHands={10,12};gear.pairedWeapons={{12,{20}},{13,{21}},{20,{12}}};
            recipe::Loadout lock;
            auto allowed=[](Ptr){return true;};
            Check(recipe::lockWeapons(gear,{{9,{20}}},allowed,lock) && lock==recipe::Loadout{{0,12},{9,20}});
            Check(recipe::permitsGeneratedWeapon(gear,lock,12) && !recipe::permitsGeneratedWeapon(gear,lock,13));
            Check(recipe::lockWeapons(gear,{{9,{21}}},allowed,lock) && lock==recipe::Loadout{{9,21}});
            Check(recipe::permitsGeneratedWeapon(gear,lock,11) && recipe::permitsGeneratedWeapon(gear,lock,13) &&
                !recipe::permitsGeneratedWeapon(gear,lock,10) && !recipe::permitsGeneratedWeapon(gear,lock,12));
            Check(recipe::lockWeapons(gear,{{0,{10}}},allowed,lock) && lock==recipe::Loadout{{0,10},{9,0}});
            Check(!recipe::permitsGeneratedWeapon(gear,lock,21) && recipe::permitsGeneratedWeapon(gear,lock,30));
            Check(recipe::lockWeapons(gear,{{0,{10,11}},{9,{21}}},allowed,lock) &&
                lock==recipe::Loadout{{0,11},{9,21}});
            Check(!recipe::lockWeapons(gear,{{0,{10}},{9,{21}}},allowed,lock));
            Check(recipe::lockWeapons(gear,{{0,{11}}},allowed,lock) && lock.size()==1 &&
                recipe::permitsGeneratedWeapon(gear,lock,21) && !recipe::permitsGeneratedWeapon(gear,lock,20));
            Check(recipe::lockWeapons(gear,{},allowed,lock) && lock.empty());
            recipe::WeaponSlots clauses{{0,{10,11}}};
            Check(recipe::requireWeapons(clauses,{{0,{11,12}}}) && clauses.at(0)==std::set<Ptr>{11});
            Check(!recipe::requireWeapons(clauses,{{0,{12}}}));
            gear.ordinaryDraws.groups[1]={};gear.ordinaryDraws.sources={{1,{{1}}}};
            gear.roles[5].grants={2,3};gear.roles[6].grants={4};gear.roleLists={{5,99},{6,99}};
            Check(recipe::withRole(gear,{1},[](Ptr role){return !role;}));
            Check(recipe::withRole(gear,{1,2,3},[](Ptr role){return role==5;}));
            Check(!recipe::withRole(gear,{2,4},[](Ptr){return true;}));
            recipe::Recipe build;build.base=100;build.specialization=101;build.tribe=102;build.rarity=103;
            gear.trees[100]=104;gear.types[100]=105;gear.neverClass[101]=106;
            recipe::Encounter event;event.tribe=102;event.tree=104;event.rank=107;event.types={105};
            event.roles={5,6};event.grants={200};event.needsPicker={{201,202}};event.blocksPicker={201};
            gear.encounterPickers.insert(200);
            gear.roleMarkers={210};gear.roles[5].markers={210};gear.routes[202]={202,{},{210},{}};
            gear.rolePickers.insert(202);gear.roles[5].grants.push_back(202);
            gear.markerGroups={{210,{999}},{211,{999}}};
            Check(recipe::fitsEncounter(gear,build,event,107) && !recipe::fitsEncounter(gear,build,event,108));
            auto wrong=build;wrong.tribe=109;Check(!recipe::fitsEncounter(gear,wrong,event,107));
            event.markers={106};Check(!recipe::fitsEncounter(gear,build,event,107));event.markers.clear();
            auto joint=[&](Ptr offhand){
                unsigned left=64;std::vector<Ptr> roots{200};
                return recipe::withRewardRequirements(event,roots,left,[&](const auto& source,const auto& inputs){
                    return recipe::withRequiredRole(gear,build,&source,{},{200},{200},inputs,[&](Ptr role){
                        if (role!=5 || inputs!=std::vector<Ptr>{200,202}) return false;
                        gear.roles[role].weapons={12};
                        gear.traitWeapons[300]={offhand};
                        const Ptr active[]{300};
                        return recipe::planWeapons(gear,active,1,role,allowed,lock);
                    });
                });
            };
            Check(joint(20) && !joint(21));
            event.roles={6};Check(!joint(20));
            event.roles={5};event.needsPicker={{200}};Check(!joint(20));
            Check(!recipe::compatibleMarkers(gear,{210,211}));
        }
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
        api.abiVersion=14;
        api.SetEditorFooterGenerator=[](int,const char*,std::uint64_t(*)(void*)){};
        Check(!PageField(&recipePage,field(Kind::Regenerate)) && !FooterGenerator(nullptr));
        Check(!Enabled(field(Kind::Class)) && !ReadChoice(nullptr,spec,0,&choice) && !Apply(nullptr,spec,1));
        Check(Apply(nullptr,tribe,1) && Apply(nullptr,base,2));
        Check(!ReadChoice(nullptr,spec,0,&choice) && !Apply(nullptr,spec,1) && Apply(nullptr,spec,2));
        Check(Apply(nullptr,base,1) && !g_draft.edits.contains(Kind::Class) && g_draft.edits.contains(Kind::Tribe));
        Check(ReadChoice(nullptr,spec,0,&choice) && !ReadChoice(nullptr,spec,1,&choice));
        Check(!Enabled(field(Kind::Regenerate)) && Apply(nullptr,spec,1) && Apply(nullptr,rare,1));
        Close(nullptr,rare);
        Check(!queuedContext && Enabled(field(Kind::Immunities)) && FooterGenerator(nullptr)==regen);
        db.optionalOnly={40,41,42,49};
        Check(recipe::exclusions(db,{41})==std::vector<Ptr>({40,42}));
        auto result=BuildRecipe(g_draft);result.traits[0]={20};
        {
            Draft imported;
            Check(StageTemplate(g_snapshot,result,imported) && imported.fromTemplate &&
                  BuildRecipe(imported)==result && !imported.ready() && !imported.edits.contains(Kind::Regenerate));
            auto bad=result;bad.traits[0]={9999};
            Check(!StageTemplate(g_snapshot,bad,imported) && BuildRecipe(imported)==result);
            Task audit{};audit.draft.edits[Kind::Hates].second.pickers={trait.header.at()};
            Check(GenerationReport(audit,{},0,"Rejected") && lastReport.find("Test_Ability] - state unavailable")!=std::string::npos);
            Memory auditMember(0x530),active(8);
            Put(active.at(),trait.header.at());Put(auditMember.at(0x198),active.at());Put(auditMember.at(0x1a0),active.at(8));
            audit.draft.edits[Kind::Bonuses].second.pickers={curse.header.at()};
            Check(GenerationReport(audit,{},auditMember.at(),"Observed") &&
                  lastReport.find("Test_Ability] - active; hidden (no Army group)")!=std::string::npos &&
                  lastReport.find("Immune_Curse] - NOT ACTIVE")!=std::string::npos);
        }
        Check(recipe::optionalResult(db,result,{20,24,999}) && !recipe::optionalResult(db,result,{24,999}) &&
            !recipe::optionalResult(db,result,{20,21,24,999}));
        db.baseGrants[300]=61;db.baseGrants[100]=62;
        db.routes[49]={49,{83},{},{310}};
        db.routes[40]={40,{83},{},{}};
        auto fixed=recipe::coreInputs(db,BuildRecipe(g_draft));
        Check(fixed==std::vector<Ptr>({61,62,49}) && !recipe::fitsCore(db,fixed,{40}) && recipe::fitsCore(db,fixed,{41}));
        auto restricted=BuildRecipe(g_draft);restricted.base=310;
        Check(recipe::coreInputs(db,restricted)==std::vector<Ptr>{62});
        uint64_t first[]{1};
        auto checked=[&](uint64_t id,Ptr allowed) {
            Check(!SetOption(nullptr,id,1,nullptr,0) && queuedFn==CheckInputs && queuedContext);
            auto owner=std::unique_ptr<std::shared_ptr<InputCheck>>(static_cast<std::shared_ptr<InputCheck>*>(queuedContext));
            queuedContext=nullptr;
            Check(inputCheck==*owner && !g_pending);
            inputCheck->ready=inputCheck->valid=true;inputCheck->allowed.insert(allowed);
            Check(SetOption(nullptr,id,1,nullptr,0));
        };
        checked(immunity,20);
        Check(SetApply(nullptr,immunity,first,1) && !SetOption(nullptr,bonus,1,nullptr,0));
        Check(!Apply(nullptr,spec,2) && !g_draft.ready());
        Check(Apply(nullptr,rare,2) && !queuedContext);
        Check(!g_draft.edits.contains(Kind::Immunities));
        checked(bonus,22);
        Check(SetApply(nullptr,bonus,first,1));
        checked(immunity,20);
        Check(SetOption(nullptr,immunity,1,nullptr,0) && SetApply(nullptr,immunity,first,1));
        Check(candidate(Kind::Regenerate,10100)==regen && Apply(nullptr,regen,10100));
        State(nullptr,0,0,&popup);Check(!popup.canSave);
        State(&recipePage,0,0,&popup);Check(popup.canSave);
        Save(nullptr);Check(!queuedContext);
        checked(bonus,22);
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
