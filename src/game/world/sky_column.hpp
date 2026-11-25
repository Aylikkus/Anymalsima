#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "chunk.hpp"
#include "terrain_edits.hpp"
#include "voxel_light.hpp"
#include "water_surface.hpp"

// Sky heightmap data (docs/handoff/sky-light.md): how each tile column looks
// from straight above. Sky light is overhead only, so a column's direct
// sunlight at any height follows from its highest opaque tile and the
// translucent tiles above it, without the chunks in between. Pure; builds
// run on the pool.
namespace sky {

// No tile (a column without any opaque tile, or an unset top).
inline constexpr int16_t None = INT16_MIN;
// Light lost per tile at or above this blocks the sky completely.
inline constexpr int Opaque = voxel_light::Opaque;
// Partially transmitting runs kept per column; more mark it `overflow`.
inline constexpr int MaxSpans = 8;

// Consecutive tiles of the same material opacity and profile, top..bottom
// inclusive. Their projected opacity is below 15.
struct Span {
    int16_t top = 0, bottom = 0;
    uint8_t opacity = 0;
    TileShape shape{};
    voxel_light::Cell cell() const { return {opacity, shape}; }
    bool operator==(const Span&) const = default;
};

// One tile column seen from the sky.
struct Column {
    // Highest tile whose overhead footprint stops all direct light (None:
    // none).
    int16_t opaque = None;
    TileShape opaqueShape{};
    // Translucent runs above `opaque`, top-down.
    std::vector<Span> spans;
    // More runs than MaxSpans: treat the first discarded run as opaque.
    // Loaded sweeps use the actual tiles.
    bool overflow = false;

    // Highest tile recorded in the column (None: none).
    int top() const { return spans.empty() ? opaque : spans.front().top; }
    // Direct sunlight (0-15) a tile at y receives: 15 minus the opacity of
    // every tile above it, as SunLightPropagator's sweep computes it. The
    // opaque tile itself is lit; tiles below it get 0.
    uint8_t direct(int y) const;
    // Opacity of the tile at y as far as the column knows it: 0 above its
    // top and in gaps between runs, 15 at and below `opaque`.
    voxel_light::Cell cellAt(int y) const;
    uint8_t opacityAt(int y) const { return cellAt(y).opacity; }
    bool operator==(const Column&) const = default;
};

// Scan profiles from `fromY` downward until the first tile that blocks
// overhead light. Merge runs of equal profile and material opacity.
template <typename Opacity>
Column scanColumn(int fromY, int toY, Opacity&& opacity) {
    Column column;
    for (int y = fromY; y >= toY; --y) {
        const voxel_light::Cell cell = opacity(y);
        const int o = voxel_light::verticalLoss(cell);
        if (!cell.opacity) continue;
        if (o >= Opaque) {
            column.opaque = int16_t(y);
            column.opaqueShape = cell.shape;
            break;
        }
        if (!column.spans.empty() && column.spans.back().cell() == cell &&
            column.spans.back().bottom == y + 1) {
            column.spans.back().bottom = int16_t(y);
            continue;
        }
        if (int(column.spans.size()) == MaxSpans) {
            column.opaque = int16_t(y);
            column.overflow = true;
            break;
        }
        column.spans.push_back(
            {int16_t(y), int16_t(y), cell.opacity, cell.shape});
    }
    return column;
}

// The columns of one chunk column (16x16 tile columns), indexed
// z * 16 + x. Packed: one top per column, runs in a shared list.
class Tile {
   public:
    static constexpr int Size = Chunk::SIZE_X * Chunk::SIZE_Z;
    static constexpr int index(int localX, int localZ) {
        return localZ * Chunk::SIZE_X + localX;
    }

    Tile();
    explicit Tile(const std::array<Column, Size>& columns);

    Column column(int i) const;
    int top(int i) const;
    int opaqueTop(int i) const { return opaque[size_t(i)]; }
    uint8_t direct(int i, int y) const;
    bool overflow(int i) const { return (flags[i] & OverflowFlag) != 0; }
    // Highest recorded tile of the whole chunk column (None: none).
    int highest() const;
    // Replaces one column (overrides).
    void set(int i, const Column& column);
    bool operator==(const Tile&) const = default;

   private:
    static constexpr uint8_t OverflowFlag = 1;
    std::array<int16_t, Size> opaque;
    std::vector<TileShape> opaqueShapes;
    std::array<uint16_t, Size> spanBegin{};
    std::array<uint8_t, Size> spanCount{};
    std::array<uint8_t, Size> flags{};
    std::vector<Span> spans;
};
using TilePtr = std::shared_ptr<const Tile>;

// Opacity by block id (Block::properties.opacity).
using OpacityTable = std::array<uint8_t, 256>;

// What a chunk column's sky columns are built from.
struct TileInputs {
    // Generated solid and water bounds, both sampled during generation.
    std::array<int, Tile::Size> surface{};
    std::array<WaterSurface, Tile::Size> water{};
    // Highest structure voxel per column (trees' wood and other generated
    // structures), INT_MIN where none. Structure voxels are opaque.
    std::array<int, Tile::Size> structureTop{};
    // Recorded gameplay edits of the column (TerrainEdits), or null.
    std::shared_ptr<const ColumnEdit> edit;
    const OpacityTable* opacity = nullptr;
};

Tile build(const TileInputs& inputs);

// Columns whose loaded tiles differ from what the sources give (saved in
// sky.dat): chunk column key (x << 32 | z) -> tile index -> column.
using Overrides = std::unordered_map<uint64_t, std::unordered_map<int, Column>>;

}  // namespace sky
