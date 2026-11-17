#pragma once

#include "serialization/binary_stream.hpp"
#include "world/flora.hpp"

// Per-chunk plant list, stored in the region file section "FLOR"
// (region_file.hpp) under the chunk's local index.
//
// v3: v2 with i32 treeDay and u16 treeVersion (TreeStamp) after
//     resolvedDay.
// v2: u8 version, i32 resolvedDay, u16 count, then per plant: u8 species,
//     u8 flags, u16 seed, u8 x, u8 z, u8 y, u8 rotation, i32 birthDay,
//     i32 harvestDay.
// v1: u8 version, u16 count, plants without harvestDay (resolvedDay 0).
namespace flora_codec {

inline constexpr uint8_t Version = 3;
// More than any recipe places in one chunk; guards malformed input.
inline constexpr size_t MaxInstances = 16384;

Bytes encode(const FloraLayer& layer, const TreeStamp& trees = {});
// Throws SerializationError on malformed input. trees (optional) receives
// the chunk's tree stamp (Never for v1/v2 data).
FloraLayer decode(ByteView data, TreeStamp* trees = nullptr);

}  // namespace flora_codec
