#include "snow_cover.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snow {
namespace {

bool sameCoord(const ChunkCoord& a, const ChunkCoord& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

}  // namespace

void SnowCover::rebuild(const SnowSettlement& settlement,
                        const SnowTerrain& terrain, uint8_t snowBlock) {
    std::unordered_map<ChunkCoord, ChunkEnvironment> cells;
    if (snowBlock) {
        for (const auto& [footprint, column] : settlement.columns()) {
            (void)footprint;
            const auto measured = settlement.level(column);
            if (!measured) continue;
            const double base = double(column.baseY) + column.baseLocal;
            double level = *measured;
            if (!(level > base + 1e-9)) continue;
            if (level - base < DustingDepth)
                level =
                    base + std::max(level - base, double(MinimumVisibleDepth));

            const int last =
                std::min(terrain.maxY() - 1, int(std::floor(level + 1e-9)));
            for (int y = column.baseY; y <= last; ++y) {
                const float localBase =
                    y == column.baseY ? column.baseLocal : 0.f;
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
                        tile_geometry::PieceId(column.baseY) *
                            Chunk::CellCount +
                        index);
                if (fill.empty()) continue;
                if (existing != chunk.end()) fill.append(*existing->second);
                chunk.insert_or_assign(
                    index, std::make_shared<const tile_geometry::CellGeometry>(
                               std::move(fill)));
            }
        }
    }

    auto next = std::make_shared<EnvironmentMap>();
    for (auto& [coord, chunk] : cells) {
        if (!chunk.empty())
            next->emplace(coord, std::make_shared<const ChunkEnvironment>(
                                     std::move(chunk)));
    }

    changed_.clear();
    for (const auto& [coord, chunk] : *environment_) {
        (void)chunk;
        if (!next->contains(coord)) changed_.push_back(coord);
    }
    for (auto& [coord, chunk] : *next) {
        if (!environment_->contains(coord)) {
            changed_.push_back(coord);
            continue;
        }
        bool differs = false;
        const auto& before = environment_->at(coord);
        const auto& after = next->at(coord);
        if (!before || !after || before->size() != after->size())
            differs = true;
        else
            for (const auto& [index, geometry] : *after)
                if (const auto found = before->find(index);
                    found == before->end() || *found->second != *geometry) {
                    differs = true;
                    break;
                }
        if (differs)
            changed_.push_back(coord);
        else
            chunk = before;
    }
    // The rebuild can remove a changed entry that the old map also had.
    std::sort(changed_.begin(), changed_.end(),
              [](const ChunkCoord& a, const ChunkCoord& b) {
                  if (a.x != b.x) return a.x < b.x;
                  if (a.y != b.y) return a.y < b.y;
                  return a.z < b.z;
              });
    changed_.erase(std::unique(changed_.begin(), changed_.end(),
                               [](const ChunkCoord& a, const ChunkCoord& b) {
                                   return sameCoord(a, b);
                               }),
                   changed_.end());

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
