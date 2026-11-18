#pragma once

#include <array>
#include <cstdint>

#include "block_light_propagator.hpp"
#include "chunk_manager.hpp"

// Near-field block light of foliage instances (FloraRenderer, TreeRenderer):
// the 4-bit levels and the chroma of the voxel field, packed into one
// instance float (i_data3.y) that foliage.vert decodes. Chroma uses
// BlockLightPropagator::encodeChroma bytes (128, 128 = white), the same as
// light volumes, so plants and crowns take the colour of the light that
// tints the blocks around them.
namespace foliage_light {

using Chroma = std::array<uint8_t, 2>;
inline constexpr Chroma White{128, 128};

// Chroma of the block light at a tile; white without a propagator or where
// only white light reaches.
inline Chroma at(const ChunkManager& chunks, const BlockLightPropagator* light,
                 int x, int y, int z) {
    if (!light || y < 0 || z < 0) return White;
    const int64_t wx = chunks.wrap().wrapTile(x);
    if (wx < 0) return White;
    const auto* chroma = light->chroma({unsigned(wx / Chunk::SIZE_X),
                                        unsigned(y / Chunk::SIZE_Y),
                                        unsigned(z / Chunk::SIZE_Z)});
    if (!chroma) return White;
    const int i =
        BlockLightPropagator::index(int(wx % Chunk::SIZE_X), y % Chunk::SIZE_Y,
                                    z % Chunk::SIZE_Z) *
        2;
    return {(*chroma)[i], (*chroma)[i + 1]};
}

// base, crown: block light levels 0..15 (plants pass the same level twice).
// An integer below 2^24, exact in a float: (base * 16 + crown) * 65536 +
// Co * 256 + Cg.
inline float pack(int base, int crown, Chroma chroma) {
    return float((uint32_t(base) * 16 + uint32_t(crown)) * 65536u +
                 uint32_t(chroma[0]) * 256u + chroma[1]);
}

}  // namespace foliage_light
