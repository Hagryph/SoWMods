// Compile the implementation into this test to exercise the private snapshot and memory boundary.
#include "../src/UrukEditor.cpp"
#include "../src/MemberCopy.h"
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
        const auto original=Stamp(member.at());
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
            using namespace engine::membercopy;
            Check(transferableBacking(0x1000,1) && transferableBacking(0,0));
            Check(!transferableBacking(0x1000,0) && !transferableBacking(0x1000,2) && !transferableBacking(0,1));
            std::array<std::byte,0x528> live{},preview{};
            preview.fill(std::byte{0x7b});
            auto original=live,candidate=preview;
            exchangeValues(live.data(),preview.data());
            for (auto e:excluded) for (unsigned i=0;i<e.size;++i) Check(live[e.offset+i]==original[e.offset+i]);
            Check(live[0x290]==std::byte{0x7b} && live[0x328]==std::byte{0x7b} && live[0x4f0]==std::byte{});
            exchangeValues(live.data(),preview.data());
            Check(live==original && preview==candidate);
            auto exchange=[&]{exchangeValues(live.data(),preview.data());};
            int gear=1;
            Check(commit(exchange,[&]{gear=2;return false;},[&]{gear=1;return true;})==Outcome::Restored);
            Check(gear==1 && live==original && preview==candidate);
            Check(commit(exchange,[]{return false;},[]{return false;})==Outcome::RollbackFailed && live==original);
            Check(commit(exchange,[&]{gear=2;return true;},[]{Check(false);return false;})==Outcome::Applied);
            Check(gear==2 && live[0x290]==candidate[0x290] && live[0x168]==original[0x168]);
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
        Snapshot g_snapshot; g_snapshot.recipeCatalog=db;
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
        db.optionalOnly={40,41,42,49};
        Check(recipe::exclusions(db,{41})==std::vector<Ptr>({40,42}));
        recipe::Recipe result;result.base=300;result.specialization=100;result.tribe=400;result.rarity=200;result.traits[0]={20};
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
        auto fixed=recipe::coreInputs(db,result);
        Check(fixed==std::vector<Ptr>({61,62,49}) && !recipe::fitsCore(db,fixed,{40}) && recipe::fitsCore(db,fixed,{41}));
        auto restricted=result;restricted.base=310;
        Check(recipe::coreInputs(db,restricted)==std::vector<Ptr>{62});
        Put<unsigned>(member.at(0x4b8),11);
        Check(original!=Stamp(member.at()));
        Put(member.at(0x1e8), slots.at(1));
        Check(VectorSize(member.at(0x1e0), 0x20) == 0);
        std::cout << "Uruk editor snapshot, types and stale-session tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
