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
        FakeRecord weapon("Weapon_Look"), builder("Weapon_Builder"), item("Weapon_Item");
        weapon.Field("ItemSlot", 2, 3);
        builder.Field("WeaponData", weapon.header.at());
        item.Field("ItemBuilder", builder.header.at());
        Check(ItemFits(item.header.at(), 2));
        Check(!ItemFits(item.header.at(), 0));
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
        Check(!PickerAllowed(member.at(), picker.header.at(), 0));
        Check(PickerAllowed(member.at(), picker.header.at(), picker.header.at()));
        FakeRecord conflicting("Picker_Conflicting");
        conflicting.Field("ExclusiveGroup", 123);
        Check(!PickerAllowed(member.at(), conflicting.header.at(), 0));
        Check(PickerAllowed(member.at(), conflicting.header.at(), picker.header.at()));
        Put<uint64_t>(member.at(0x508), 1ull << 38);
        Check(!PickerAllowed(member.at(), picker.header.at(), picker.header.at()));
        Put<uint64_t>(member.at(0x508), 0);
        Snapshot snapshot;
        snapshot.session = 1;
        snapshot.selected = {army.header.at(), 0, 42};
        snapshot.targets.push_back({snapshot.selected, member.at(), "Test captain"});
        Catalog catalog;
        catalog["Faction/Traits/Picker"].push_back({picker.header.at(), "Picker_Test"});
        Capture(snapshot, catalog);
        Check(snapshot.fields.size() > 75);
        auto level = std::find_if(snapshot.fields.begin(), snapshot.fields.end(), [](const auto& f) { return f.kind == Kind::Level; });
        Check(level != snapshot.fields.end() && level->value == "11" && level->choices.size() == 85);
        auto slot = std::find_if(snapshot.fields.begin(), snapshot.fields.end(), [](const auto& f) { return f.kind == Kind::Picker; });
        Check(slot != snapshot.fields.end() && slot->value.find("Test_Ability") != std::string::npos);
        Check(InCatalog(catalog, "Faction/Traits/Picker", picker.header.at()));
        Check(!InCatalog(catalog, "Faction/Personalities", picker.header.at()));
        HagUIAPI api{};
        api.SaveSession = Session;
        g_ui = &api;
        g_snapshot = snapshot;
        HagUI_Property property{};
        Check(ReadField(nullptr, 0, &property));
        Check(property.editable);
        Check(ChoiceCount(nullptr, property.id) == 1);
        HagUI_Choice choice{};
        Check(ReadChoice(nullptr, property.id, 0, &choice) && choice.id == 1);
        Check(!Apply(nullptr, property.id, 999));
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
