#include "light_volume.hpp"

#include <algorithm>
#include <stdexcept>

#include "items/block.hpp"

uint8_t tileOpacity(const Tile& tile, const DataStorage<Block>& blocks) {
    const int block =
        tile.block_id ? blocks.get(tile.block_id).properties.opacity : 0;
    const int liquid = tile.isLiquid() ? 8 : 0;
    return static_cast<uint8_t>(std::clamp(std::max(block, liquid), 0, 15));
}

namespace LightVolume {

void encode(const Chunk& chunk, const DataStorage<Block>& blocks, Cells& out) {
    for (int z = 0; z < CellsZ; ++z)
        for (int y = 0; y < CellsY; ++y)
            for (int x = 0; x < CellsX; ++x) {
                const auto& tile = chunk.tiles[z][y][x];
                const int i = cellIndex(x, y, z);
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

void build(const std::array<const Cells*, 27>& chunks, Volume& out) {
    const Cells* center = chunks[neighbor(0, 0, 0)];
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
                const Cells* source = chunks[neighbor(ox, oy, oz)];
                if (!source) {
                    source = center;
                    cx = std::clamp(tx - 1, 0, CellsX - 1);
                    cy = std::clamp(ty - 1, 0, CellsY - 1);
                    cz = std::clamp(tz - 1, 0, CellsZ - 1);
                }
                const int s = cellIndex(cx, cy, cz);
                const int d = texelIndex(tx, ty, tz);
                out[d] = (*source)[s];
                out[d + 1] = (*source)[s + 1];
            }
}

}  // namespace LightVolume
