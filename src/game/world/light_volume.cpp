#include "light_volume.hpp"

#include <algorithm>
#include <stdexcept>

#include "items/block.hpp"

uint8_t tileOpacity(const Tile& tile, const DataStorage<Block>& blocks) {
    const int block =
        tile.block_id ? blocks.get(tile.block_id).properties.opacity : 0;
    const int liquid = tile.liquid == LiquidType::Water
                           ? waterOpacity(tile.liquidLevel)
                           : (tile.isLiquid() ? 8 : 0);
    return static_cast<uint8_t>(std::clamp(std::max(block, liquid), 0, 15));
}

namespace LightVolume {

void encode(const Chunk& chunk, const DataStorage<Block>& blocks,
            const uint8_t* chroma, Cells& out) {
    for (int z = 0; z < CellsZ; ++z)
        for (int y = 0; y < CellsY; ++y)
            for (int x = 0; x < CellsX; ++x) {
                const auto& tile = chunk.tiles[z][y][x];
                const int i = cellIndex(x, y, z);
                const int c = ((z * CellsY + y) * CellsX + x) * 2;
                out[i + 2] = chroma ? chroma[c] : 128;
                out[i + 3] = chroma ? chroma[c + 1] : 128;
                if (tileOpacity(tile, blocks) >= 15) {
                    out[i] = out[i + 1] = 0;
                    continue;
                }
                out[i] =
                    static_cast<uint8_t>(tile.lightLevel.sun_exposure * 17);
                out[i + 1] =
                    static_cast<uint8_t>(tile.lightLevel.artificial_light * 17);
            }
}

namespace {
// Calls fn(texel x, y, z, source cells, cell x, y, z) for every texel of a
// bordered volume around chunks[13]. A missing neighbor repeats the center
// chunk's nearest edge cell.
template <typename CellArray, typename F>
void forBorderedVolume(const std::array<const CellArray*, 27>& chunks, F&& fn) {
    const CellArray* center = chunks[neighbor(0, 0, 0)];
    if (!center) throw std::invalid_argument("light volume without center");
    // Local coordinate l in [-1, cells] -> chunk offset and wrapped cell.
    auto split = [](int l, int cells, int& offset, int& cell) {
        offset = l < 0 ? -1 : (l >= cells ? 1 : 0);
        cell = l - offset * cells;
    };
    for (int tz = 0; tz < SizeZ; ++tz)
        for (int ty = 0; ty < SizeY; ++ty)
            for (int tx = 0; tx < SizeX; ++tx) {
                int ox, oy, oz, cx, cy, cz;
                split(tx - 1, CellsX, ox, cx);
                split(ty - 1, CellsY, oy, cy);
                split(tz - 1, CellsZ, oz, cz);
                const CellArray* source = chunks[neighbor(ox, oy, oz)];
                if (!source) {
                    source = center;
                    cx = std::clamp(tx - 1, 0, CellsX - 1);
                    cy = std::clamp(ty - 1, 0, CellsY - 1);
                    cz = std::clamp(tz - 1, 0, CellsZ - 1);
                }
                fn(tx, ty, tz, *source, cx, cy, cz);
            }
}
}  // namespace

void build(const std::array<const Cells*, 27>& chunks, Volume& out) {
    forBorderedVolume(chunks, [&](int tx, int ty, int tz, const Cells& source,
                                  int cx, int cy, int cz) {
        const int s = cellIndex(cx, cy, cz);
        const int d = texelIndex(tx, ty, tz);
        for (int c = 0; c < Channels; ++c) out[d + c] = source[s + c];
    });
}

void encodeCover(const Chunk& chunk, CoverCells& out) {
    for (int z = 0; z < CellsZ; ++z)
        for (int y = 0; y < CellsY; ++y)
            for (int x = 0; x < CellsX; ++x) {
                const auto& tile = chunk.tiles[z][y][x];
                out[coverCellIndex(x, y, z)] =
                    tile.cover ? uint8_t(1 + tile.coverDensity()) : 0;
            }
}

void buildCover(const std::array<const CoverCells*, 27>& chunks,
                CoverVolume& out) {
    // Raw values first (border included), then fill uncovered cells from
    // the covered tile below or above.
    CoverVolume raw;
    forBorderedVolume(chunks, [&](int tx, int ty, int tz,
                                  const CoverCells& source, int cx, int cy,
                                  int cz) {
        raw[coverTexelIndex(tx, ty, tz)] = source[coverCellIndex(cx, cy, cz)];
    });
    for (int tz = 0; tz < SizeZ; ++tz)
        for (int ty = 0; ty < SizeY; ++ty)
            for (int tx = 0; tx < SizeX; ++tx) {
                uint8_t value = raw[coverTexelIndex(tx, ty, tz)];
                if (!value && ty > 0)
                    value = raw[coverTexelIndex(tx, ty - 1, tz)];
                if (!value && ty + 1 < SizeY)
                    value = raw[coverTexelIndex(tx, ty + 1, tz)];
                out[coverTexelIndex(tx, ty, tz)] =
                    value ? uint8_t((value - 1) * 17) : 0;
            }
}

}  // namespace LightVolume
