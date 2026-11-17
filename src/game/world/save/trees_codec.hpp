#pragma once

#include "serialization/section_container.hpp"
#include "world/tree_overrides.hpp"

// World-level tree overrides (trees.dat), a SectionContainer:
//   "TREE" v1: u32 count, then per record (sorted by key):
//              u64 key, u8 species, u32 seed, i32 anchorX, i32 anchorY,
//              i32 anchorZ, i32 birthDay, f32 headroom,
//              u16 cutCount, {i32 segment, f32 t, f32 age}...
// Unlike terrain_edits.dat and light_sources.dat this is truth, not derived
// data: cuts and planted trees exist nowhere else. Unknown sections are
// kept in `extra`.
namespace trees_codec {

struct File {
    TreeOverrideMap trees;
    SectionContainer extra;
};

Bytes encode(const TreeOverrideMap& trees, const SectionContainer& extra = {});
// Throws SerializationError on malformed or unsupported data.
File decode(ByteView bytes);

}  // namespace trees_codec
