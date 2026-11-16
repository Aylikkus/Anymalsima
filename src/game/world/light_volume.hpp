#pragma once

#include <array>
#include <cstdint>

#include "chunk.hpp"
#include "core/assets_api.hpp"

// Light attenuation of one tile on the 0-15 scale. Sunlight propagation and
// the GPU light volumes share this rule.
uint8_t tileOpacity(const Tile& tile, const DataStorage<Block>& blocks);

// The light field of one chunk, encoded for the GPU. Texel layout is X
// fastest, then Y (height), then Z, four channels per cell: R = sun
// exposure, G = artificial light intensity, both scaled 0-15 -> 0-255, and
// BA = artificial light chroma (BlockLightPropagator::encodeChroma, 128 =
// white). Fully opaque cells store zero light so trilinear filtering darkens
// concave corners (soft occlusion).
namespace LightVolume {
static constexpr int Channels = 4;
static constexpr int CellsX = Chunk::SIZE_X;
static constexpr int CellsY = Chunk::SIZE_Y;
static constexpr int CellsZ = Chunk::SIZE_Z;
// A one-cell border on every side holds the neighbors' edge cells, so the
// filter at a chunk face reads exactly what the adjacent chunk reads.
static constexpr int SizeX = CellsX + 2;
static constexpr int SizeY = CellsY + 2;
static constexpr int SizeZ = CellsZ + 2;

using Cells = std::array<uint8_t, CellsX * CellsY * CellsZ * Channels>;
using Volume = std::array<uint8_t, SizeX * SizeY * SizeZ * Channels>;

constexpr int cellIndex(int x, int y, int z) {
    return ((z * CellsY + y) * CellsX + x) * Channels;
}
constexpr int texelIndex(int x, int y, int z) {
    return ((z * SizeY + y) * SizeX + x) * Channels;
}
// Neighborhood slot of a chunk offset, each component in [-1, 1]. Slot 13 is
// the chunk itself.
constexpr int neighbor(int dx, int dy, int dz) {
    return ((dz + 1) * 3 + (dy + 1)) * 3 + (dx + 1);
}

// chroma: two bytes per cell in chunk cell order, or null for white light.
void encode(const Chunk& chunk, const DataStorage<Block>& blocks,
            const uint8_t* chroma, Cells& out);

// Assembles the bordered volume of chunks[13]. A missing neighbor (nullptr)
// repeats the center chunk's nearest edge cell, which neither invents sky
// nor darkens the seam until that neighbor arrives. chunks[13] must be set.
void build(const std::array<const Cells*, 27>& chunks, Volume& out);
}  // namespace LightVolume
