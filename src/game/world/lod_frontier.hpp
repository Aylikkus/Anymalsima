#pragma once

#include <algorithm>
#include <set>

#include "terrain_lod.hpp"

namespace TerrainLod {
// A coverage transaction: a frontier can change only when every replacement
// patch is drawable. Old coverage survives cancellation and partial arrivals.
class Frontier {
   public:
    uint64_t stage(std::vector<Patch> layout) {
        if (layout != target) {
            target = std::move(layout);
            ready.clear();
            ++revision;
        }
        return revision;
    }
    bool publish(uint64_t token, const Patch& patch) {
        if (token != revision ||
            std::find(target.begin(), target.end(), patch) == target.end())
            return false;
        ready.insert(patch);
        return true;
    }
    bool canCommit() const {
        return active != target && !target.empty() &&
               ready.size() == target.size();
    }
    bool commit() {
        if (!canCommit()) return false;
        active = target;
        return true;
    }
    const std::vector<Patch>& current() const { return active; }
    const std::vector<Patch>& pending() const { return target; }
    uint64_t token() const { return revision; }

   private:
    std::vector<Patch> active, target;
    std::set<Patch> ready;
    uint64_t revision = 0;
};
}  // namespace TerrainLod
