#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "chunk.hpp"
#include "terrain_edits.hpp"

// Sky heightmap data (docs/handoff/sky-light.md): how each tile column looks
// from straight above. Sky light is overhead only, so a column's direct
// sunlight at any height follows from its highest opaque tile and the
// translucent tiles above it, without the chunks in between. Pure; builds
// run on the pool.
namespace sky {

// No tile (a column without any opaque tile, or an unset top).
inline constexpr int16_t None = INT16_MIN;
// Light lost per tile at or above this blocks the sky completely.
inline constexpr int Opaque = 15;
// Translucent runs kept per column; more mark the column `overflow`.
inline constexpr int MaxSpans = 8;

// Consecutive translucent tiles of one opacity (1-14), top..bottom
// inclusive.
struct Span {
    int16_t top = 0, bottom = 0;
    uint8_t opacity = 0;
    bool operator==(const Span&) const = default;
};

// One tile column seen from the sky.
struct Column {
    // Highest tile with opacity >= Opaque (None: none).
    int16_t opaque = None;
    // Translucent runs above `opaque`, top-down.
    std::vector<Span> spans;
    // More runs than MaxSpans: the lowest kept run's bottom is treated as
    // opaque, which is darker than the truth; consumers needing exact light
    // there sweep the loaded tiles instead.
    bool overflow = false;

    // Highest tile that attenuates sky light (None: none).
    int top() const { return spans.empty() ? opaque : spans.front().top; }
    // Direct sunlight (0-15) a tile at y receives: 15 minus the opacity of
    // every tile above it, as SunLightPropagator's sweep computes it. The
    // opaque tile itself is lit; tiles below it get 0.
    uint8_t direct(int y) const;
    // Opacity of the tile at y as far as the column knows it: 0 above its
    // top and in gaps between runs, 15 at and below `opaque`.
    uint8_t opacityAt(int y) const;
    bool operator==(const Column&) const = default;
};

// Builds a column from tiles listed top-down: opacity(y) for y from
// `fromY` downwards, until an opaque tile. Runs of equal opacity merge.
template <typename Opacity>
Column scanColumn(int fromY, int toY, Opacity&& opacity) {
    Column column;
    for (int y = fromY; y >= toY; --y) {
        const int o = opacity(y);
        if (o <= 0) continue;
        if (o >= Opaque || int(column.spans.size()) == MaxSpans + 1) {
            column.opaque = int16_t(y);
            break;
        }
        if (!column.spans.empty() && column.spans.back().opacity == o &&
            column.spans.back().bottom == y + 1) {
            column.spans.back().bottom = int16_t(y);
            continue;
        }
        column.spans.push_back({int16_t(y), int16_t(y), uint8_t(o)});
    }
    if (int(column.spans.size()) > MaxSpans) {
        // Too many runs: the extra one becomes the opaque floor.
        column.opaque = column.spans.back().top;
        column.spans.pop_back();
        column.overflow = true;
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
    uint8_t direct(int i, int y) const;
    bool overflow(int i) const { return (flags[i] & OverflowFlag) != 0; }
    // Highest attenuating tile of the whole chunk column (None: none).
    int highest() const;
    // Replaces one column (overrides).
    void set(int i, const Column& column);
    bool operator==(const Tile&) const = default;

   private:
    static constexpr uint8_t OverflowFlag = 1;
    std::array<int16_t, Size> opaque;
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
    // Generated surface per column (TerrainSampler surface_y): solid up to
    // it, sea water above it up to SEA_LEVEL_Y.
    std::array<int, Tile::Size> surface{};
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
