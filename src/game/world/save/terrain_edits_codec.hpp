#pragma once

#include "serialization/section_container.hpp"
#include "world/terrain_edits.hpp"

// World-level terrain edit index (terrain_edits.dat), a SectionContainer:
//   "EDIT" v1: u32 count, then per edited chunk column
//              {u32 chunkX, u32 chunkZ, 256 x i16 groundY,
//               256 x u8 groundBlock, 256 x i16 waterY,
//               u16 slabCount, {u16 slabY, u32 size, chunk_codec blob}...}
// It mirrors edits stored in region files so distant terrain can show them
// for regions that are not loaded. Unknown sections are kept in `extra`.
namespace terrain_edits_codec {

struct File {
    EditMap columns;
    SectionContainer extra;
};

Bytes encode(const EditMap& columns, const SectionContainer& extra = {});
// Throws SerializationError on malformed or unsupported data.
File decode(ByteView bytes);

}  // namespace terrain_edits_codec
