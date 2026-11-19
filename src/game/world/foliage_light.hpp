#pragma once

#include <array>
#include <cstdint>

#include "block_light_propagator.hpp"
#include "chunk_manager.hpp"
#include "sky_height_map.hpp"

// Near-field block light of foliage instances (FloraRenderer, TreeRenderer):
// the 4-bit levels and the chroma of the voxel field, packed into one
// instance float (i_data3.y) that foliage.vert decodes. Chroma uses
// BlockLightPropagator::encodeChroma bytes (128, 128 = white), the same as
// light volumes, so plants and crowns take the colour of the light that
// tints the blocks around them.
namespace foliage_light {

// Use the voxel-resolution service when available, then its coarse map for
// render-only samples outside the built columns (distant tree crowns).
inline float sky(const ChunkManager& chunks, const SkyHeightMap* map, int x,
                 int y, int z) {
    if (!map) return 1.f;
    if (const auto level = map->skyAt(x, y, z)) return *level / 15.f;
    if (const auto coarse = map->skyOcclusion()) {
        const double centre = coarse->originX() + SkyOcclusionMap::Extent / 2;
        return coarse->sky(chunks.wrap().imageX(x + 0.5, centre), y + 0.5,
                           z + 0.5);
    }
    return 1.f;
}

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
