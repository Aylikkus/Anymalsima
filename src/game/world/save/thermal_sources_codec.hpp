#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "serialization/section_container.hpp"

// World-level index of movable thermal item sources (thermal_sources.dat), a
// SectionContainer:
//   "HEAT" v1: u32 count, then per source
//              {f64 x, f64 y, f64 z, string item, u64 key, u8 carried}
// Block thermal sources are derived from tiles and are not saved here.
// Emission values are re-read from the item definitions on load, so editing
// the data applies to saved items too. Unknown sections stay in `extra`.
namespace thermal_sources_codec {

struct Record {
    std::array<double, 3> position{0, 0, 0};
    std::string item;
    uint64_t key = 0;
    bool carried = false;
    bool operator==(const Record&) const = default;
};

struct File {
    std::vector<Record> records;
    SectionContainer extra;
};

Bytes encode(std::vector<Record> records, const SectionContainer& extra = {});
// Throws SerializationError on malformed or unsupported data.
File decode(ByteView bytes);

}  // namespace thermal_sources_codec
