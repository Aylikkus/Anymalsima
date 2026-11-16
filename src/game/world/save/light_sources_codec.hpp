#pragma once

#include <vector>

#include "serialization/section_container.hpp"
#include "world/tile_pos.hpp"

// World-level index of far-reaching block lights (light_sources.dat), a
// SectionContainer:
//   "LITE" v1: u32 count, then per light {u32 x, u16 y, u32 z, u8 block}
// It mirrors emissive tiles stored in region files, so lighthouses glow and
// light distant terrain while their regions are not loaded. Emission values
// come from the block definitions on load, so editing JSON applies to saved
// lights too. Derived data: region files hold the truth. Unknown sections
// are kept in `extra`.
namespace light_sources_codec {

struct Record {
    TilePos tile;
    uint8_t block = 0;
    bool operator==(const Record&) const = default;
};

struct File {
    std::vector<Record> lights;
    SectionContainer extra;
};

Bytes encode(std::vector<Record> lights, const SectionContainer& extra = {});
// Throws SerializationError on malformed or unsupported data.
File decode(ByteView bytes);

}  // namespace light_sources_codec
