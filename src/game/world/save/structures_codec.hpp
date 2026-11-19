#pragma once

#include "serialization/section_container.hpp"
#include "world/structure_overrides.hpp"
#include "world/tree_overrides.hpp"

namespace structures_codec {

// structures.dat holds TREE, VOXL, and any kind-specific sections the
// current build does not interpret. All are authoritative world state.
struct File {
    TreeOverrideMap trees;
    structure_voxels::Overrides voxels;
    SectionContainer extra;
    bool migratedTrees = false;  // set only by save_catalog's legacy reader
};

Bytes encode(const TreeOverrideMap& trees,
             const structure_voxels::Overrides& voxels,
             const SectionContainer& extra = {});
File decode(ByteView bytes);

}  // namespace structures_codec
