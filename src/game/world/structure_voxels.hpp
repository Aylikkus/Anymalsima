#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "consts.hpp"
#include "tile_box.hpp"
#include "world_wrap.hpp"

// Voxels of structures at distance (docs/handoff/structures.md): any kind of
// structure (massive trees today; towns and castles later) describes its
// voxels through a Source, and StructureLod draws them as 16^3 slabs beyond
// the loaded chunks. Slabs come at three levels: full tiles (level 0, the
// same voxels the chunks hold, so the hand-over is seamless) and cells of 2
// or 4 tiles for far parts. The level is chosen per slab by its distance from
// the eye, so one structure a kilometre wide is detailed only near the
// player. Pure; runs on the pool.
namespace structure_voxels {

inline constexpr int Side = Chunk::SIZE_X;
static_assert(Chunk::SIZE_X == Chunk::SIZE_Y && Chunk::SIZE_Y == Chunk::SIZE_Z);
inline constexpr size_t Cells = size_t(Side) * Side * Side;
inline size_t cellIndex(int x, int y, int z) {
    return size_t((z * Side + y) * Side + x);
}

// Slab keys: level 0 slabs are ordinary chunk coordinates; coarser ones
// count slabs of 16 cells and carry the level in the high bits of y.
inline constexpr unsigned LevelShift = 12;
inline constexpr int MaxLevel = 2;
inline int levelOf(const ChunkCoord& c) { return int(c.y >> LevelShift); }
inline ChunkCoord baseOf(ChunkCoord c) {
    c.y &= (1u << LevelShift) - 1;
    return c;
}
inline ChunkCoord withLevel(ChunkCoord c, int level) {
    c.y |= unsigned(level) << LevelShift;
    return c;
}
// Tiles per side of a slab of the level.
inline int spanOf(int level) { return Side << level; }

// The voxels of one slab: a cell is solid when its bit is set, with `block`,
// or the per-cell id in `blocks` (Cells entries) for structures of several
// blocks.
struct Slab {
    std::bitset<Cells> bits;
    uint8_t block = 0;
    std::vector<uint8_t> blocks;  // empty: every set cell is `block`
    // Full, with full slabs on all six sides: no visible face, so no proxy
    // (kept so the neighbours' border faces stay hidden).
    bool enclosed = false;

    uint8_t blockAt(size_t cell) const {
        return blocks.empty() ? block : blocks[cell];
    }
    // Adds `other`'s cells where this slab is empty (overlapping structures:
    // the first one keeps its cells).
    void merge(const Slab& other);
};
using Slabs = std::unordered_map<ChunkCoord, Slab>;

// Packed unwrapped slab coordinates of one level (each within +-2^20).
inline uint64_t packSlab(int sx, int sy, int sz) {
    constexpr int Bias = 1 << 20;
    return uint64_t(uint32_t(sx + Bias)) << 42 |
           uint64_t(uint32_t(sy + Bias)) << 21 | uint64_t(uint32_t(sz + Bias));
}
// The slabs of each level that may hold voxels (packSlab of unwrapped slab
// coordinates; a coarse slab is listed when any of its tiles may be).
// Conservative: a listed slab may turn out empty, an unlisted one never
// holds voxels.
struct Occupancy {
    std::array<std::unordered_set<uint64_t>, MaxLevel + 1> slabs;
    bool contains(int level, int sx, int sy, int sz) const {
        return slabs[size_t(level)].contains(packSlab(sx, sy, sz));
    }
    // Adds a level-0 slab and its coarser parents.
    void add(int sx, int sy, int sz);
};
using OccupancyPtr = std::shared_ptr<const Occupancy>;

// A structure's voxels. Immutable: a structure that changes (growth, damage,
// building) is described by a new Source with the same key and another
// revision. Implementations must be safe to call from any thread.
class Source {
   public:
    virtual ~Source() = default;
    // Stable identity of the structure across revisions.
    uint64_t key = 0;
    // Differs whenever the voxels may differ.
    uint64_t revision = 0;
    // Tiles that may hold voxels (X contiguous around the structure, not
    // wrapped).
    TileBox bounds;
    // The slab at slab coordinates (sx, sy, sz) of `level` (unwrapped X, as
    // `bounds`), or nothing when it holds no voxel. Coarse cells of 2^level
    // tiles are solid when at least a quarter of their tiles are.
    virtual std::optional<Slab> rasterize(int level, int sx, int sy,
                                          int sz) const = 0;
    // Where this revision's voxels may differ from `older` (same key, an
    // earlier revision), in the same frame as `bounds`; std::nullopt when
    // anywhere (rebuild everything). A local change (a cut, damage, a new
    // room) rebuilds only the slabs it touches.
    virtual std::optional<TileBox> changedSince(const Source& older) const {
        (void)older;
        return std::nullopt;
    }
    // Where voxels may be (null: anywhere in `bounds`). Planning visits only
    // these slabs, so a sparse structure in a huge box (a massive tree's
    // limbs) is not planned and rasterized slab by empty slab. May be
    // costly: StructureLod calls it on the pool, once per source object.
    virtual OccupancyPtr occupancy() const { return nullptr; }
    // Seen from the sky (docs/handoff/sky-light.md): the highest voxel of
    // each tile column of `columns` (unwrapped X, as `bounds`; only voxels
    // with y0 <= y < y1 count), as tops[(z - z0) * width + (x - x0)] =
    // world Y, or INT_MIN where the column holds none. Exact (level 0).
    // The default rasterizes level-0 slabs from the top down, which is
    // slow for tall structures: kinds should answer directly (trees project
    // their skeleton). Thread-safe like rasterize.
    virtual void skyTops(const TileBox& columns, std::vector<int>& tops) const;
};
using SourcePtr = std::shared_ptr<const Source>;

// Distances (tiles) where slabs coarsen: level 0 below Limits[0], level 1
// below Limits[1], level 2 beyond; with Margin of hysteresis.
inline constexpr float Limits[MaxLevel] = {192, 640};
inline constexpr float Margin = 64;

struct Planned {
    ChunkCoord key;              // canonical X, level in y
    int sx = 0, sy = 0, sz = 0;  // unwrapped slab coordinates of the level
    float distance = 0;          // eye to the slab box
};

// The slabs covering `bounds`, each at the level its distance allows: cells
// of MaxLevel split into finer ones near the eye, so the result covers the
// box exactly once, without overlap. previous: the last plan's keys of the
// same structure (hysteresis; empty for none). Nearest first.
// Tiles of a planned slab (unwrapped X).
TileBox boxOf(const Planned& planned);

// occupancy: only these slabs are planned (null: every slab of bounds).
// Slabs intersecting fullDetail stay at level 0 for chunk handover.
std::vector<Planned> plan(const TileBox& bounds, double eyeX, double eyeY,
                          double eyeZ, const WorldWrap& wrap,
                          const std::unordered_set<ChunkCoord>& previous,
                          const Occupancy* occupancy = nullptr,
                          const TileBox& fullDetail = {});

// Canonical key of unwrapped slab coordinates (std::nullopt: outside a
// world that does not wrap).
std::optional<ChunkCoord> keyOf(int level, int sx, int sy, int sz,
                                const WorldWrap& wrap);

// Rasterizes every slab of `source` at one level (tests and tools; the
// renderer plans per slab).
Slabs rasterizeAll(const Source& source, int level, const WorldWrap& wrap);

// Sets `enclosed` on full slabs whose six same-level neighbours are full.
void markEnclosed(Slabs& slabs, const WorldWrap& wrap);

// What a structure casts on the ground below it, seen from the sky
// (SkyOcclusionMap): per column of FootprintLevel cells (Footprint::Cell
// tiles a side), the top of its highest solid cell. Any structure shades
// what lies under it this way: a tree's limbs, a castle's overhangs, a
// bridge. Built from the coarse rasterization, so parts thinner than about
// half a cell vanish; their voxel shade (one or two light levels) is faint
// anyway.
inline constexpr int FootprintLevel = MaxLevel;
struct Footprint {
    static constexpr int Cell = 1 << FootprintLevel;  // tiles
    // packColumn(cell x, cell z) of unwrapped cells (the source's bounds
    // frame) -> top tile Y of the highest solid cell (exclusive).
    std::unordered_map<uint64_t, int> tops;
    static uint64_t packColumn(int cx, int cz) {
        constexpr int64_t Bias = int64_t(1) << 30;
        return uint64_t(cx + Bias) << 32 | uint64_t(uint32_t(cz + Bias));
    }
    static std::pair<int, int> unpackColumn(uint64_t key) {
        constexpr int64_t Bias = int64_t(1) << 30;
        return {int(int64_t(key >> 32) - Bias),
                int(int64_t(uint32_t(key)) - Bias)};
    }
    bool operator==(const Footprint&) const = default;
};
using FootprintPtr = std::shared_ptr<const Footprint>;

// The footprint of `source` (occupancy: its occupied slabs, null for every
// slab of its bounds). With `previous` (an earlier revision's footprint)
// and `changed` (Source::changedSince), only the columns of slabs touching
// the changed box are rasterized again. May be costly: run it on the pool.
FootprintPtr footprint(const Source& source, const Occupancy* occupancy,
                       const Footprint* previous = nullptr,
                       const std::optional<TileBox>& changed = std::nullopt);

}  // namespace structure_voxels
