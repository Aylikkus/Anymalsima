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

// Differences of one chunk column (16x16 tiles) from generated terrain.
//
// The generator makes a tile solid iff y <= its generated surface, so an
// edited column splits into a ground override (the topmost solid tile at or
// below the generated surface: digging, removed mountains, filled pits) and
// structure voxels (every solid tile above the generated surface: built
// things). Distant terrain samples the ground; structure proxies draw the
// voxels.
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
    // Solid tiles above the generated surface: one chunk_codec blob per slab
    // (every other tile air), sorted by slab Y.
    std::vector<std::pair<uint16_t, ChunkBlob>> structure;

    // Highest structure tile in the column, or -1.
    int structureTopY() const { return structureTop; }
    // Recomputes structureTopY from the blobs; call after filling structure.
    void finish();
    const ChunkBlob* slab(uint16_t slabY) const;

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
    explicit TerrainEdits(EditMap initial)
        : map(std::make_shared<const EditMap>(std::move(initial))) {}

    std::shared_ptr<const EditMap> snapshot() const { return map; }
    // Increases on every change.
    uint64_t revision() const { return rev; }
    std::shared_ptr<const ColumnEdit> find(uint64_t key) const;
    bool contains(uint64_t key) const { return map->contains(key); }
    size_t size() const { return map->size(); }

    void set(uint64_t key, std::shared_ptr<const ColumnEdit> edit);
    void erase(uint64_t key);

    // Column key, emitted after each set/erase.
    Signal<uint64_t> changed;
    // Unsaved changes exist; the owner clears it when it writes them.
    bool dirty = false;

   private:
    std::shared_ptr<const EditMap> map;
    uint64_t rev = 0;
};
