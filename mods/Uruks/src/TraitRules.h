#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace uruk::engine {
struct TraitSource {
    unsigned capacity{};
    // The engine chooses one eligible table, then draws from that table.
    std::vector<std::set<std::uintptr_t>> tables;
};
struct TraitRules {
    using Id=std::uintptr_t;
    std::map<Id,std::vector<Id>> groups;
    std::vector<TraitSource> sources;
    bool permits(const std::vector<Id>& picks) const {
        if (picks.empty()) return true;
        std::set<Id> seen,held;
        using Route=std::pair<size_t,size_t>;
        std::vector<std::vector<Route>> routes;
        for (Id p:picks) {
            auto it=groups.find(p);
            if (it==groups.end() || !seen.insert(p).second) return false;
            for (Id group:it->second) if (!held.insert(group).second) return false;
            if (held.size()>64) return false;
            auto& r=routes.emplace_back();
            for (size_t i=0;i<sources.size();++i) if (sources[i].capacity)
                for (size_t j=0;j<sources[i].tables.size();++j)
                    if (sources[i].tables[j].contains(p)) r.emplace_back(i,j);
            if (r.empty()) return false;
        }
        std::sort(routes.begin(),routes.end(),[](const auto& a,const auto& b){return a.size()<b.size();});
        std::vector<unsigned> used(sources.size());
        std::vector<size_t> table(sources.size());
        // A picker can occur in several lists. Do not reject a combination just
        // because a greedy assignment used the only slot for a later picker.
        auto assign=[&](auto&& self,size_t k)->bool {
            if (k==routes.size()) return true;
            for (auto [i,j]:routes[k]) {
                if (used[i]>=sources[i].capacity || (used[i] && table[i]!=j)) continue;
                table[i]=j; ++used[i];
                if (self(self,k+1)) return true;
                --used[i];
            }
            return false;
        };
        return assign(assign,0);
    }
};
}
