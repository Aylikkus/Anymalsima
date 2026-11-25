#pragma once

#include "serialization/section_container.hpp"
#include "world/sky_column.hpp"

// Sky heightmap overrides (sky.dat, docs/handoff/sky-light.md), a
// SectionContainer:
//   "SKYO" v1: u32 chunk columns, then per column
//              {u32 chunkX, u32 chunkZ, u16 count,
//               {u8 tile index, i16 opaque, u8 overflow, u8 spans,
//                {i16 top, i16 bottom, u8 opacity}...}...}
//   v2 adds legacy shape codes. v3 appends optional piece coverage to the
//   opaque top and each span, using piece records with opacity as material.
// The rest of the heightmap is rebuilt from terrain, trees and terrain
// edits. Unknown sections are kept in `extra`.
namespace sky_codec {

struct File {
    sky::Overrides overrides;
    SectionContainer extra;
};

Bytes encode(const sky::Overrides& overrides,
             const SectionContainer& extra = {});
// Throws SerializationError on malformed or unsupported data.
File decode(ByteView bytes);

}  // namespace sky_codec
