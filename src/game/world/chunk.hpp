#pragma once

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "tile.hpp"

struct RootVoxel {
    uint64_t owner;
    uint8_t block, cover, coverState;  // displaced natural substrate
    TileShape shape{};
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
    // v1–v4 slabs keep ambiguous water and dry edits as saved.
    bool projectsWater = false;
    // Legacy slabs also preserve uncertainty about terrain replacements.
    bool legacyRootTerrain = false;
    bool legacyRootShape = false;
    std::shared_ptr<const RootTerrain> rootTerrain;

    static constexpr uint16_t cell(int x, int y, int z) {
        return uint16_t((z * SIZE_Y + y) * SIZE_X + x);
    }
    bool hasShapes() const { return bool(shapes); }
    size_t shapeCount() const { return shapes ? shapes->cells.size() : 0; }
    TileShape shapeAt(uint16_t index) const {
        if (!shapes) return {};
        const auto it =
            std::lower_bound(shapes->cells.begin(), shapes->cells.end(), index);
        return it != shapes->cells.end() && *it == index
                   ? shapes->codes[it - shapes->cells.begin()]
                   : TileShape{};
    }
    template <typename F>
    void forEachShape(F&& visit) const {
        if (!shapes) return;
        for (size_t i = 0; i < shapes->cells.size(); ++i)
            visit(shapes->cells[i], shapes->codes[i]);
    }
    ShapedTile cellAt(int x, int y, int z) const {
        return {tiles[z][y][x], shapeAt(cell(x, y, z))};
    }
    // For live chunks, call under the same lock used for tile edits.
    void setCell(uint16_t index, Tile tile, TileShape shape = {}) {
        assert(index < SIZE_X * SIZE_Y * SIZE_Z);
        updateShape(index, tile.isSolid() ? shape : TileShape{});
        tiles[index / (SIZE_X * SIZE_Y)][(index / SIZE_X) % SIZE_Y]
             [index % SIZE_X] = tile;
    }
    // Bulk generation replaces the entire slab with cube-only terrain.
    void clearShapes() { shapes.reset(); }
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
            cell, RootVoxel{owner, tile.block_id, tile.cover, tile.coverState,
                            shapeAt(cell)});
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
                auto tile = tiles[cell / (SIZE_X * SIZE_Y)]
                                 [(cell / SIZE_X) % SIZE_Y][cell % SIZE_X];
                if (!grown.test(tile.block_id)) continue;
                tile.block_id = root.block;
                tile.cover = root.cover;
                tile.coverState = root.coverState;
                setCell(cell, tile, root.shape);
            }
            data.roots.clear();
        }
        legacyRootShape = false;
    }

   private:
    struct ShapeTable {
        std::vector<uint16_t> cells;
        std::vector<TileShape> codes;
    };
    std::shared_ptr<const ShapeTable> shapes;

    void updateShape(uint16_t index, TileShape shape) {
        const bool cube = shape.code() == 0;
        if (!shapes && cube) return;

        size_t offset = 0;
        bool found = false;
        if (shapes) {
            const auto it = std::lower_bound(shapes->cells.begin(),
                                             shapes->cells.end(), index);
            offset = it - shapes->cells.begin();
            found = it != shapes->cells.end() && *it == index;
            if ((found && shapes->codes[offset] == shape) || (!found && cube))
                return;
            if (cube && shapes->cells.size() == 1) {
                clearShapes();
                return;
            }
        }

        auto updated = !shapes ? std::make_shared<ShapeTable>()
                       : shapes.unique()
                           ? std::const_pointer_cast<ShapeTable>(shapes)
                           : std::make_shared<ShapeTable>(*shapes);
        if (cube) {
            updated->cells.erase(updated->cells.begin() + offset);
            updated->codes.erase(updated->codes.begin() + offset);
        } else if (found) {
            updated->codes[offset] = shape;
        } else {
            // Reserve both buffers before inserting to keep them in sync if
            // an allocation fails. Grow geometrically for bulk insertion.
            const size_t size = updated->cells.size();
            if (updated->cells.capacity() == size)
                updated->cells.reserve(std::max(size_t{1}, size * 2));
            if (updated->codes.capacity() == size)
                updated->codes.reserve(std::max(size_t{1}, size * 2));
            updated->cells.insert(updated->cells.begin() + offset, index);
            updated->codes.insert(updated->codes.begin() + offset, shape);
        }
        shapes = std::move(updated);
    }

    RootTerrain& changeRootTerrain() {
        if (!rootTerrain) rootTerrain = std::make_shared<RootTerrain>();
        if (!rootTerrain.unique())
            rootTerrain = std::make_shared<RootTerrain>(*rootTerrain);
        return *std::const_pointer_cast<RootTerrain>(rootTerrain);
    }
};
