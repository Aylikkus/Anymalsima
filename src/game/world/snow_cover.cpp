#include "snow_cover.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace snow {
void SnowCover::rebuild(const SnowSettlement& settlement,
                        const SnowTerrain& terrain, uint8_t snowBlock,
                        const ChunkColumns* resident,
                        const ChunkColumns* changed) {
    std::unordered_map<ChunkCoord, ChunkEnvironment> cells;
    const auto project = [&](const SnowColumn& column) {
        const auto measured = settlement.level(column);
        if (!measured) return;
        const double base = double(column.baseY) + column.baseLocal;
        double level = *measured;
        if (!(level > base + 1e-9)) return;
        if (level - base < DustingDepth)
            level = base + std::max(level - base, double(MinimumVisibleDepth));

        const int last =
            std::min(terrain.maxY() - 1, int(std::floor(level + 1e-9)));
        for (int y = column.baseY; y <= last; ++y) {
            const float localBase = y == column.baseY ? column.baseLocal : 0.f;
            const float localTop =
                float(std::clamp(level - double(y), 0.0, 1.0));
            if (!(localTop > localBase + 1e-6f)) continue;
            auto cell = terrain.cell({column.x, y, column.z});
            if (!cell || cell->isFull()) break;
            const ChunkCoord coord{unsigned(column.x / Chunk::SIZE_X),
                                   unsigned(y / Chunk::SIZE_Y),
                                   unsigned(column.z / Chunk::SIZE_Z)};
            const uint16_t index =
                Chunk::cell(column.x % Chunk::SIZE_X, y % Chunk::SIZE_Y,
                            column.z % Chunk::SIZE_Z);
            auto& chunk = cells[coord];
            const auto existing = chunk.find(index);
            if (existing != chunk.end()) cell->append(*existing->second);
            auto fill = cell->freeSpaceBetween(
                localBase, localTop, snowBlock,
                EnvironmentPieceBase +
                    tile_geometry::PieceId(column.baseY) * Chunk::CellCount +
                    index);
            if (fill.empty()) continue;
            if (existing != chunk.end()) fill.append(*existing->second);
            chunk.insert_or_assign(
                index, std::make_shared<const tile_geometry::CellGeometry>(
                           std::move(fill)));
        }
    };
    const auto& columns = settlement.columns();
    const auto* selected = changed ? changed : resident;
    if (snowBlock && !selected) {
        for (const auto& [key, column] : columns) {
            (void)key;
            project(column);
        }
    } else if (snowBlock && !columns.empty()) {
        for (const auto& [cx, cz] : *selected) {
            if (resident && !resident->contains({cx, cz})) continue;
            const int x0 = int(cx) * Chunk::SIZE_X;
            const int z0 = int(cz) * Chunk::SIZE_Z;
            for (int x = x0; x < x0 + Chunk::SIZE_X; ++x) {
                auto entry = columns.lower_bound({x, z0, terrain.minY()});
                const auto end = columns.lower_bound(
                    {x, z0 + Chunk::SIZE_Z, terrain.minY()});
                for (; entry != end; ++entry) project(entry->second);
            }
        }
    }

    std::erase_if(cells, [](const auto& item) { return item.second.empty(); });
    auto next = changed ? std::make_shared<EnvironmentMap>(*environment_)
                        : std::make_shared<EnvironmentMap>();
    changed_.clear();
    const auto erase = [&](const ChunkCoord& coord) {
        if (!cells.contains(coord)) {
            next->erase(coord);
            changed_.push_back(coord);
        }
    };
    if (changed) {
        // Erase obsolete slabs only within the columns being republished.
        for (const auto& [x, z] : *changed)
            for (int y = 0; y < terrain.maxY(); y += Chunk::SIZE_Y) {
                const ChunkCoord coord{x, unsigned(y / Chunk::SIZE_Y), z};
                if (environment_->contains(coord)) erase(coord);
            }
    } else {
        for (const auto& [coord, chunk] : *environment_) {
            (void)chunk;
            erase(coord);
        }
    }
    for (auto& [coord, chunk] : cells) {
        const auto before = environment_->find(coord);
        if (before != environment_->end() &&
            std::ranges::equal(
                chunk, *before->second, [](const auto& a, const auto& b) {
                    return a.first == b.first && *a.second == *b.second;
                })) {
            next->insert_or_assign(coord, before->second);
            continue;
        }
        next->insert_or_assign(
            coord, std::make_shared<const ChunkEnvironment>(std::move(chunk)));
        changed_.push_back(coord);
    }
    std::sort(changed_.begin(), changed_.end(),
              [](const ChunkCoord& a, const ChunkCoord& b) {
                  return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
              });

    environment_ = std::move(next);
    ++revision_;
}

void SnowCover::clear() {
    changed_.clear();
    for (const auto& [coord, chunk] : *environment_) {
        (void)chunk;
        changed_.push_back(coord);
    }
    environment_ = std::make_shared<const EnvironmentMap>();
    ++revision_;
}

EnvironmentCell SnowCover::cellAt(TilePos pos) const {
    if (pos.x < 0 || pos.y < 0 || pos.z < 0) return {};
    const ChunkCoord coord{unsigned(pos.x / Chunk::SIZE_X),
                           unsigned(pos.y / Chunk::SIZE_Y),
                           unsigned(pos.z / Chunk::SIZE_Z)};
    const auto found = environment_->find(coord);
    if (found == environment_->end() || !found->second) return {};
    const uint16_t index = Chunk::cell(
        pos.x % Chunk::SIZE_X, pos.y % Chunk::SIZE_Y, pos.z % Chunk::SIZE_Z);
    const auto cell = found->second->find(index);
    return cell == found->second->end() ? EnvironmentCell{} : cell->second;
}

size_t SnowCover::cellCount() const {
    size_t total = 0;
    for (const auto& [coord, chunk] : *environment_) {
        (void)coord;
        if (chunk) total += chunk->size();
    }
    return total;
}

}  // namespace snow
