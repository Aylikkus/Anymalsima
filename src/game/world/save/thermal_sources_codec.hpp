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
//   "ROOM" v1: u32 count, then per significant room
//              {i32 x, i32 y, i32 z, f32 air_c, f32 wall_c, f64 last_day}
//
// Block thermal sources are derived from tiles and are not saved here. Room
// records hold the wall heat store and its timestamp; a room whose air and
// wall both sit at the ground baseline is omitted. Emission values and a
// thermostat's configured mode/target are re-read from the definitions on
// load, so editing the data applies to saved items and blocks too. Unknown
// sections stay in `extra`.
namespace thermal_sources_codec {

struct Record {
    std::array<double, 3> position{0, 0, 0};
    std::string item;
    uint64_t key = 0;
    bool carried = false;
    bool operator==(const Record&) const = default;
};

struct RoomRecord {
    int x = 0;
    int y = 0;
    int z = 0;
    float airC = 0;
    float wallC = 0;
    double lastDay = 0;
    bool operator==(const RoomRecord&) const = default;
};

struct File {
    std::vector<Record> records;
    std::vector<RoomRecord> rooms;
    SectionContainer extra;
};

Bytes encode(std::vector<Record> records, const SectionContainer& extra = {});
// Adds the room heat-store section. Kept as an overload so existing callers
// that pass only the section container stay source-compatible.
Bytes encode(std::vector<Record> records, std::vector<RoomRecord> rooms,
             const SectionContainer& extra = {});
// Throws SerializationError on malformed or unsupported data.
File decode(ByteView bytes);

}  // namespace thermal_sources_codec
