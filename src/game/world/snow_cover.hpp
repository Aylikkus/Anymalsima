#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "chunk.hpp"
#include "snow_settlement.hpp"

// Projects settled snow onto the actual free space of each cell. The result
// is an environmental solid layer shared with ChunkManager: it is combined
// with chunk pieces for meshing, picking, walking and light queries, but it is
// never written into a chunk save. Dusting and solid snow both come from the
// same deposited volume; a deposit shallower than DustingDepth is lifted to a
// small visible skin instead of a zero-thickness face.
namespace snow {

class SnowCover {
   public:
    // Below this depth the deposit is a visual dusting. At or above it the
    // exact free space up to the measured level becomes solid geometry.
    static constexpr float DustingDepth = .01f;
    // Upward offset of a sub-centimetre dusting so it cannot z-fight with the
    // supporting surface.
    static constexpr float MinimumVisibleDepth = .002f;
    // Piece IDs stay clear of the small IDs used by tile pieces.
    static constexpr tile_geometry::PieceId EnvironmentPieceBase = 0x40000000u;

    // With a change set, recalculate and publish only those columns.
    // Deselected residents keep their previous immutable slab snapshots.
    void rebuild(const SnowSettlement& settlement, const SnowTerrain& terrain,
                 uint8_t snowBlock, const ChunkColumns* resident = nullptr,
                 const ChunkColumns* changed = nullptr);
    void clear();

    const std::shared_ptr<const EnvironmentMap>& environment() const {
        return environment_;
    }
    EnvironmentCell cellAt(TilePos pos) const;
    // Chunks whose environment changed in the last rebuild (old and new).
    const std::vector<ChunkCoord>& changedChunks() const { return changed_; }
    uint64_t revision() const { return revision_; }
    size_t cellCount() const;

   private:
    std::shared_ptr<const EnvironmentMap> environment_ =
        std::make_shared<const EnvironmentMap>();
    std::vector<ChunkCoord> changed_;
    uint64_t revision_ = 0;
};

}  // namespace snow
