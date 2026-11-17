#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "tree_rasterizer.hpp"
#include "tree_skeleton.hpp"

// Trees that differ from generation (docs/handoff/trees.md, "Tree state and
// persistence"): cuts made to generated trees, generated trees that died
// and were removed, and trees planted during play. Authoritative (saved in
// trees.dat); untouched generated trees are never stored, they are regenerated
// from the seed.
struct TreeRecord {
    // Planted trees have bit 62 set; others are generated tree keys
    // (TreeInstance::key).
    static constexpr uint64_t PlantedBit = 1ULL << 62;
    uint64_t key = 0;
    bool planted() const { return key & PlantedBit; }
    // Planted trees only: what their skeleton is built from.
    uint8_t species = 0;
    uint32_t seed = 0;
    TreeAnchor anchor;  // canonical X
    int32_t birthDay = 0;
    float headroom = TreeMaxHeight;
    // In order; indices refer to the skeleton with the earlier cuts applied
    // (applyTreeCuts). Ages are tree ages.
    std::vector<TreeCut> cuts;
    // A generated tree that died and left the world (it lost its base);
    // generation skips it. Removed planted trees are simply erased.
    bool removed = false;
    bool operator==(const TreeRecord&) const = default;
};
using TreeOverrideMap = std::unordered_map<uint64_t, TreeRecord>;
using TreeOverridesPtr = std::shared_ptr<const TreeOverrideMap>;
