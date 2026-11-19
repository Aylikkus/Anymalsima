#pragma once

#include <map>
#include <memory>
#include <set>

#include "tile.hpp"

struct RootVoxel {
    uint64_t owner;
    uint8_t block, cover, coverState;  // displaced natural substrate
};

struct RootTerrain {
    std::map<uint16_t, RootVoxel> roots;
    std::set<uint16_t> edited;
};

struct Chunk {
    static constexpr int SIZE_X = 16;
    static constexpr int SIZE_Y = 16;
    static constexpr int SIZE_Z = 16;
    Tile tiles[SIZE_Z][SIZE_Y][SIZE_X];  // [z][y][x]
    // Legacy slabs cannot distinguish unmodified terrain from same-material
    // player replacements. Preserve that uncertainty across later saves.
    bool legacyRootTerrain = false;
    bool legacyRootShape = false;
    std::shared_ptr<const RootTerrain> rootTerrain;

    static constexpr uint16_t cell(int x, int y, int z) {
        return uint16_t((z * SIZE_Y + y) * SIZE_X + x);
    }
    const RootVoxel* rootAt(uint16_t cell) const {
        if (!rootTerrain) return nullptr;
        const auto it = rootTerrain->roots.find(cell);
        return it == rootTerrain->roots.end() ? nullptr : &it->second;
    }
    bool rootEdited(uint16_t cell) const {
        return rootTerrain && rootTerrain->edited.contains(cell);
    }
    void recordRoot(uint16_t cell, uint64_t owner, const Tile& tile) {
        changeRootTerrain().roots.emplace(
            cell, RootVoxel{owner, tile.block_id, tile.cover, tile.coverState});
    }
    bool markRootEdited(uint16_t cell) {
        if (legacyRootTerrain || rootEdited(cell)) return false;
        auto& data = changeRootTerrain();
        data.roots.erase(cell);
        data.edited.insert(cell);
        return true;
    }
    void restoreRootSubstrate(const GrownBlockSet& grown) {
        if (rootTerrain && !rootTerrain->roots.empty()) {
            auto& data = changeRootTerrain();
            for (const auto& [cell, root] : data.roots) {
                auto& tile = tiles[cell / (SIZE_X * SIZE_Y)]
                                  [(cell / SIZE_X) % SIZE_Y][cell % SIZE_X];
                if (!grown.test(tile.block_id)) continue;
                tile.block_id = root.block;
                tile.cover = root.cover;
                tile.coverState = root.coverState;
            }
            data.roots.clear();
        }
        legacyRootShape = false;
    }

   private:
    RootTerrain& changeRootTerrain() {
        if (!rootTerrain) rootTerrain = std::make_shared<RootTerrain>();
        if (!rootTerrain.unique())
            rootTerrain = std::make_shared<RootTerrain>(*rootTerrain);
        return *std::const_pointer_cast<RootTerrain>(rootTerrain);
    }
};
