#pragma once

#include "serialization/binary_stream.hpp"
#include "world/world.hpp"

// Binary world map (world_map.dat), a SectionContainer with:
//   "GENS" v1: WorldGenSettings
//   "RGNS" v1: u32 width, u32 height, then every Region field in row order
//   "RIVR" v1: river polylines (path, preview mesh, bounds)
// regionMeshes/riverMesh are preview-only and are not stored; a decoded
// world leaves them empty.
namespace world_map_codec {

Bytes encode(const World& world);
// Throws SerializationError on malformed or unsupported data.
World decode(ByteView bytes);

}  // namespace world_map_codec
