#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chunk.hpp"
#include "core/signal.hpp"
#include "save/region_file.hpp"
#include "structure_voxels.hpp"

// Differences of one chunk column (16x16 tiles) from generated terrain and
// structure sources.
//
// Distant terrain samples the ground override. Structure proxies draw
// additions and apply suppression masks above the ground.
struct ColumnEdit {
    static constexpr int Tiles = Chunk::SIZE_X * Chunk::SIZE_Z;
    static constexpr int index(int localX, int localZ) {
        return localZ * Chunk::SIZE_X + localX;
    }
    // Topmost solid world Y at or below the generated surface; -1 = none.
    std::array<int16_t, Tiles> groundY{};
    // Block id at groundY (0 when groundY is -1).
    std::array<uint8_t, Tiles> groundBlock{};
    // Top of the liquid above the ground plus one, or -1 without liquid.
    std::array<int16_t, Tiles> waterY{};
    // Added or replacement solid tiles above the generated surface. Each
    // blob contains only the positive differences.
    std::vector<std::pair<uint16_t, ChunkBlob>> structure;
    // Generated structure cells suppressed by an edit. Sorted by slab Y
    // and by structure_voxels::cellIndex within a slab.
    std::vector<std::pair<uint16_t, std::vector<uint16_t>>> removed;

    // Highest structure tile in the column, or -1.
    int structureTopY() const { return structureTop; }
    // Recomputes structureTopY from the blobs; call after filling structure.
    void finish();
    const ChunkBlob* slab(uint16_t slabY) const;
    const std::vector<uint16_t>* removals(uint16_t slabY) const;
    bool suppresses(int x, int y, int z) const;

   private:
    int structureTop = -1;
};

inline uint64_t editKey(unsigned chunkX, unsigned chunkZ) {
    return (uint64_t(chunkX) << 32) | chunkZ;
}

// Keyed by editKey(chunk column). World-wide, independent of regions, so a
// structure crossing region borders stays complete when one region unloads.
using EditMap = std::unordered_map<uint64_t, std::shared_ptr<const ColumnEdit>>;

// Main-thread owner of all edits that distant terrain must show. Readers on
// workers receive immutable copy-on-write snapshots.
class TerrainEdits {
   public:
    TerrainEdits() : map(std::make_shared<const EditMap>()) {}
    explicit TerrainEdits(EditMap initial);

    std::shared_ptr<const EditMap> snapshot() const { return map; }
    // Increases on every change.
    uint64_t revision() const { return rev; }
    std::shared_ptr<const ColumnEdit> find(uint64_t key) const;
    bool contains(uint64_t key) const { return map->contains(key); }
    size_t size() const { return map->size(); }

    void set(uint64_t key, std::shared_ptr<const ColumnEdit> edit);
    void erase(uint64_t key);
    // Apply the suppression masks to a generated landmark source. Positive
    // edit voxels continue to come from the edit's own slabs.
    structure_voxels::SourcePtr maskStructure(
        const structure_voxels::SourcePtr& source, const WorldWrap& wrap,
        bool keepWounds = false) const;

    // Column key, emitted after each set/erase.
    Signal<uint64_t> changed;
    // Unsaved changes exist; the owner clears it when it writes them.
    bool dirty = false;

   private:
    std::shared_ptr<const EditMap> map;
    size_t columnsWithRemovals = 0;
    struct Masked {
        std::weak_ptr<const structure_voxels::Source> result;
        uint64_t sourceRevision = 0;
        bool keepWounds = false;
    };
    mutable std::unordered_map<uint64_t, Masked> maskedStructures;
    uint64_t rev = 0;
};
