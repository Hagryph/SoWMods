#include "GameOffsets.h"
#include "Settings.h"
#include <cstring>

namespace uruk {
namespace {
using Address = std::uintptr_t;
template<class T> T at(Address p, size_t offset) { return *reinterpret_cast<const T*>(p + offset); }
volatile LONG* ownedCell = nullptr;
LONG originalPrice = 0;

volatile LONG* FindCost(Address db) {
    if (!db) return nullptr;
    auto categories = at<Address>(db,0x50);
    auto count = at<unsigned>(db,0x48);
    if (!categories || count > 10000) return nullptr;
    for (unsigned i=0; i<count; ++i) {
        auto cat = categories + 0x58ull*i;
        auto label = at<const char*>(cat,0);
        if (!label || std::strcmp(label,"Faction/Contracts/Contract")) continue;
        auto rows = at<Address>(cat,0x38);
        auto n = at<unsigned>(cat,0x28);
        if (!rows || n > 100000) return nullptr;
        for (unsigned j=0; j<n; ++j) {
            auto record = rows + 0x28ull*j;
            if (at<Address>(record,0x18)!=cat || (at<unsigned char>(record,1)&0x10)) continue;
            auto name = at<const char*>(record,0x20);
            if (!name || std::strcmp(name,"ReassignmentOrders")) continue;
            auto fields = at<Address>(record,8), cells = at<Address>(record,0x10);
            if (!fields || !cells) return nullptr;
            for (unsigned k=0; k<at<unsigned char>(record,0); ++k) {
                auto f = fields + 16ull*k;
                // GameDB hash of "Cost"; one scalar on the reassignment contract.
                if (at<unsigned>(f,0)!=0x8b8af85d || (at<unsigned char>(f,10)&63)!=3 ||
                    at<unsigned short>(f,12)!=1) continue;
                return reinterpret_cast<volatile LONG*>(cells + 8ull*at<unsigned short>(f,14));
            }
        }
        break;
    }
    return nullptr;
}
}

// Only invoked on the game thread. Resolve afresh rather than writing a pointer from a previous world.
bool ApplyGarrisonCost(bool free) {
    __try {
        auto cell = FindCost(at<Address>(game::FromRVA(game::kGameDatabase),0));
        if (!cell) return false;
        if (cell!=ownedCell) ownedCell=nullptr;
        if (free) {
            LONG current=*cell;
            if (current==0) return true;
            if (current<0 || InterlockedCompareExchange(cell,0,current)!=current) return false;
            originalPrice=current;
            ownedCell=cell;
        } else if (ownedCell) {
            // Don't undo a price change made by another mod.
            InterlockedCompareExchange(cell,originalPrice,0);
            ownedCell=nullptr;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
