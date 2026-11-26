#include "snow_cover.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace snow {
namespace {

// A layer above the upward facets of a partial substrate (a ramp or a
// slab). Its top stays parallel to the facet instead of becoming a flat
// column above it.
std::shared_ptr<const tile_geometry::CellGeometry> drapedSnow(
    const tile_geometry::CellGeometry& substrate, float depth, uint8_t block) {
    auto result = std::make_shared<tile_geometry::CellGeometry>();
    uint32_t id = 0;
    for (const auto& face : substrate.exteriorFaces()) {
        const auto& normal = face.plane.normal;
        if (normal.y <= .2f) continue;
        const std::array planes{
            tile_geometry::Plane{{-normal.x, -normal.y, -normal.z},
                                 -face.plane.offset},
            tile_geometry::Plane{normal, face.plane.offset + depth * normal.y},
            tile_geometry::Plane{{-1, 0, 0}, 0},
            tile_geometry::Plane{{1, 0, 0}, 1},
            tile_geometry::Plane{{0, -1, 0}, 0},
            tile_geometry::Plane{{0, 1, 0}, 1},
            tile_geometry::Plane{{0, 0, -1}, 0},
            tile_geometry::Plane{{0, 0, 1}, 1}};
        if (result->tryAddConvex(0x5f000000u + id, block, planes) ==
            tile_geometry::PiecePlacement::Added)
            ++id;
    }
    return id ? result : nullptr;
}

}  // namespace

void SnowCover::rebuild(const SnowSettlement& settlement,
                        const SnowTerrain& terrain, uint8_t snowBlock,
                        const ChunkColumns* resident,
                        const ChunkColumns* changed,
                        const std::map<ChunkColumn, Footprints>* footprints) {
    projectedLevels_.clear();
    std::unordered_map<ChunkCoord, ChunkEnvironment> cells;
    if (snowBlock && packedBlock_ != snowBlock) {
        tile_geometry::CellGeometry cube;
        cube.tryAdd({SnowSettlement::PackPieceBase, snowBlock, {}});
        packedCube_ = std::make_shared<const tile_geometry::CellGeometry>(
            std::move(cube));
        packedBlock_ = snowBlock;
    }
    // Snap the rendered top to 1 cm. The ledger keeps the exact height, but
    // sub-centimetre differences between neighbouring footprints otherwise
    // rasterize as a field of dark one-pixel side faces.
    constexpr Volume RenderStep = 20'000;
    const auto quantized = [](Volume top, Volume bottom) {
        return std::max(bottom + 1,
                        (top + RenderStep / 2) / RenderStep * RenderStep);
    };
    const auto projectPack = [&](int x, int z) {
        PackRun baseline;
        const auto runs = settlement.packRuns(x, z, baseline);
        if (runs.empty()) return;
        const int highestSolid = terrain.top(x, z);
        for (auto run : runs) {
            if (run.volume() < MinimumVisibleDepth * UnitsPerCubicMetre)
                run.top =
                    int32_t(std::min(run.bottom + Volume(MinimumVisibleDepth *
                                                         UnitsPerCubicMetre),
                                     terrain.maxY() * UnitsPerCubicMetre));
            run.top =
                int32_t(std::min(quantized(run.top, run.bottom),
                                 Volume(terrain.maxY()) * UnitsPerCubicMetre));
            const int first = int(run.bottom / UnitsPerCubicMetre);
            const int last = int((run.top - 1) / UnitsPerCubicMetre);
            for (int y = first; y <= last; ++y) {
                const auto substrate =
                    y > highestSolid
                        ? std::optional(tile_geometry::CellGeometry{})
                        : terrain.cell({x, y, z});
                if (!substrate || (!substrate->empty() && substrate->isFull()))
                    continue;
                const ChunkCoord coord{unsigned(x / Chunk::SIZE_X),
                                       unsigned(y / Chunk::SIZE_Y),
                                       unsigned(z / Chunk::SIZE_Z)};
                const uint16_t index = Chunk::cell(
                    x % Chunk::SIZE_X, y % Chunk::SIZE_Y, z % Chunk::SIZE_Z);
                const Volume base = y * UnitsPerCubicMetre;
                if (substrate->empty() && run.bottom <= base &&
                    run.top >= base + UnitsPerCubicMetre) {
                    cells[coord].emplace(index, packedCube_);
                    continue;
                }
                if (!substrate->empty()) {
                    const float depth =
                        float(run.top - base) / UnitsPerCubicMetre;
                    if (const auto layer =
                            drapedSnow(*substrate, depth, snowBlock)) {
                        cells[coord].emplace(index, std::move(layer));
                        continue;
                    }
                }
                auto fill = substrate->freeSpaceBetween(
                    float(run.bottom - base) / UnitsPerCubicMetre,
                    float(run.top - base) / UnitsPerCubicMetre, snowBlock,
                    SnowSettlement::PackPieceBase);
                if (!fill.empty())
                    cells[coord].emplace(
                        index,
                        std::make_shared<const tile_geometry::CellGeometry>(
                            std::move(fill)));
            }
        }
    };
    const auto project = [&](const SnowColumn& column) {
        const auto measured = settlement.level(column);
        if (!measured) return;
        projectedLevels_.emplace(SnowKey{column.x, column.z, column.baseY},
                                 *measured);
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
                SnowSettlement::loosePieceId(column.baseY, index));
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
    } else if (snowBlock) {
        for (const auto& [cx, cz] : *selected) {
            if (resident && !resident->contains({cx, cz})) continue;
            const int x0 = int(cx) * Chunk::SIZE_X;
            const int z0 = int(cz) * Chunk::SIZE_Z;
            if (footprints) {
                const auto dirty = footprints->find({cx, cz});
                if (dirty != footprints->end()) {
                    for (int y = 0; y < terrain.maxY(); y += Chunk::SIZE_Y) {
                        const ChunkCoord coord{cx, unsigned(y / Chunk::SIZE_Y),
                                               cz};
                        const auto prior = environment_->find(coord);
                        if (prior == environment_->end()) continue;
                        auto& slab =
                            cells.emplace(coord, *prior->second).first->second;
                        for (const auto& [x, z] : dirty->second)
                            for (int ly = 0; ly < Chunk::SIZE_Y; ++ly)
                                slab.erase(Chunk::cell(x - x0, ly, z - z0));
                    }
                    for (const auto& [x, z] : dirty->second) {
                        projectPack(x, z);
                        const auto end =
                            columns.upper_bound({x, z, terrain.maxY()});
                        for (auto it =
                                 columns.lower_bound({x, z, terrain.minY()});
                             it != end; ++it)
                            project(it->second);
                    }
                    continue;
                }
            }
            for (int x = x0; x < x0 + Chunk::SIZE_X; ++x) {
                for (int z = z0; z < z0 + Chunk::SIZE_Z; ++z) projectPack(x, z);
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
                    return a.first == b.first &&
                           (a.second == b.second || *a.second == *b.second);
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

namespace {

class LooseSnowSource final : public structure_voxels::Source {
   public:
    using Intervals = std::map<SnowKey, PackRun>;

    LooseSnowSource(uint64_t id, uint64_t version, uint8_t block,
                    Intervals intervals)
        : intervals(std::move(intervals)), block(block) {
        key = id;
        revision = version;
        auto occupied = std::make_shared<structure_voxels::Occupancy>();
        for (const auto& [point, run] : this->intervals) {
            const auto [x, z, support] = point;
            const int y0 = int(run.bottom / UnitsPerCubicMetre);
            const int y1 = int((run.top - 1) / UnitsPerCubicMetre) + 1;
            if (bounds.empty()) {
                bounds = {x, y0, z, x + 1, y1, z + 1};
            } else {
                bounds = {std::min(bounds.x0, x),  std::min(bounds.y0, y0),
                          std::min(bounds.z0, z),  std::max(bounds.x1, x + 1),
                          std::max(bounds.y1, y1), std::max(bounds.z1, z + 1)};
            }
            for (int sy = y0 / Chunk::SIZE_Y; sy <= (y1 - 1) / Chunk::SIZE_Y;
                 ++sy)
                occupied->add(x / Chunk::SIZE_X, sy, z / Chunk::SIZE_Z);
        }
        occupiedSlabs = std::move(occupied);
    }

    structure_voxels::OccupancyPtr occupancy() const override {
        return occupiedSlabs;
    }

    std::optional<structure_voxels::Slab> rasterize(int level, int sx, int sy,
                                                    int sz) const override {
        using namespace structure_voxels;
        const int scale = 1 << level, span = spanOf(level);
        const TileBox box{sx * span,       sy * span,       sz * span,
                          (sx + 1) * span, (sy + 1) * span, (sz + 1) * span};
        if (box.intersect(bounds).empty()) return std::nullopt;
        const Volume cellHeight = scale * UnitsPerCubicMetre;
        Slab slab;
        for (const auto& [point, run] : intervals) {
            const auto [x, z, support] = point;
            if (x < box.x0 || x >= box.x1 || z < box.z0 || z >= box.z1)
                continue;
            const int first =
                std::max(0, int(run.bottom / cellHeight - sy * Side));
            const int last =
                std::min(Side - 1, int((run.top - 1) / cellHeight - sy * Side));
            for (int y = first; y <= last; ++y) {
                const Volume base = (sy * Side + y) * cellHeight;
                const float low =
                    float(std::max(Volume(run.bottom) - base, Volume{0})) /
                    cellHeight;
                const float high =
                    float(std::min(Volume(run.top) - base, cellHeight)) /
                    cellHeight;
                const auto index = uint16_t(
                    cellIndex((x - box.x0) / scale, y, (z - box.z0) / scale));
                auto [at, inserted] = slab.snow.try_emplace(
                    index, Slab::SnowLayer{low, high, block});
                if (!inserted && high >= at->second.high) {
                    // A distant texel represents its uppermost exposed
                    // interval. Keep thin roofs thin at every scale.
                    if (low <= at->second.high)
                        at->second.low = std::min(low, at->second.low);
                    else
                        at->second.low = low;
                    at->second.high = high;
                }
            }
        }
        if (slab.snow.empty()) return std::nullopt;
        return slab;
    }

    void skyTops(const TileBox& columns,
                 std::vector<int>& tops) const override {
        tops.assign(size_t(columns.sizeX()) * columns.sizeZ(), INT_MIN);
        for (const auto& [point, run] : intervals) {
            const auto [x, z, support] = point;
            const int bottom = int(run.bottom / UnitsPerCubicMetre);
            const int top = int((run.top - 1) / UnitsPerCubicMetre);
            if (x < columns.x0 || x >= columns.x1 || z < columns.z0 ||
                z >= columns.z1 || bottom >= columns.y1 || top < columns.y0)
                continue;
            auto& height =
                tops[size_t(z - columns.z0) * columns.sizeX() + x - columns.x0];
            height = std::max(height, std::min(top, columns.y1 - 1));
        }
    }

   private:
    const Intervals intervals;
    uint8_t block;
    structure_voxels::OccupancyPtr occupiedSlabs;
};

}  // namespace

void SnowCover::updateLooseExterior(
    const SnowSettlement& settlement, const ChunkColumns& changed,
    const ChunkColumns& resident, uint8_t snowBlock,
    const std::map<ChunkColumn, Footprints>* footprints) {
    if (!snowBlock) return;
    constexpr Volume Step = 50'000;  // 5 cm for distant deposits
    const auto& columns = settlement.columns();
    for (const auto& column : changed) {
        const auto [cx, cz] = column;
        const int x0 = int(cx) * Chunk::SIZE_X, z0 = int(cz) * Chunk::SIZE_Z;
        const auto old = looseExterior_.find(column);
        std::map<SnowKey, PackRun> intervals;
        const Footprints* dirty = nullptr;
        if (footprints)
            if (const auto found = footprints->find(column);
                found != footprints->end())
                dirty = &found->second;
        for (int x = x0; x < x0 + Chunk::SIZE_X; ++x) {
            const auto end = columns.lower_bound({x, z0 + Chunk::SIZE_Z, 0});
            for (auto it = columns.lower_bound({x, z0, 0}); it != end; ++it) {
                const auto& [wx, z, y, local, volume] = it->second;
                const double base = y + double(local);
                const auto measured = projectedLevels_.find(it->first);
                const bool usePrior = !resident.contains(column) ||
                                      (dirty && !dirty->contains({wx, z})) ||
                                      measured == projectedLevels_.end();
                if (usePrior && old != looseExterior_.end()) {
                    const auto prior = old->second.intervals.find(it->first);
                    if (prior != old->second.intervals.end()) {
                        intervals.emplace(*prior);
                        continue;
                    }
                }
                const Volume bottom = std::llround(base * UnitsPerCubicMetre);
                const Volume top =
                    measured != projectedLevels_.end()
                        ? std::llround(measured->second * UnitsPerCubicMetre)
                        : bottom + volume;
                if (top <= bottom ||
                    bottom >= WORLD_HEIGHT_Y * UnitsPerCubicMetre)
                    continue;
                const auto quantized =
                    std::clamp((top + Step / 2) / Step * Step,
                               std::min(bottom + Volume(2'000),
                                        WORLD_HEIGHT_Y * UnitsPerCubicMetre),
                               WORLD_HEIGHT_Y * UnitsPerCubicMetre);
                intervals.emplace(it->first,
                                  PackRun{int32_t(bottom), int32_t(quantized)});
            }
        }
        if (intervals.empty()) {
            looseExterior_.erase(column);
            continue;
        }
        if (old != looseExterior_.end() && old->second.intervals == intervals)
            continue;

        const auto key = 0xe000000000000000ULL | (uint64_t(cx) << 24) | cz;
        auto source = std::make_shared<const LooseSnowSource>(
            key, ++exteriorRevision_, snowBlock, intervals);
        looseExterior_.insert_or_assign(
            column, LooseExterior{std::move(intervals), std::move(source)});
    }
}

void SnowCover::appendLooseExterior(
    std::vector<structure_voxels::SourcePtr>& sources, const WorldWrap& wrap,
    Vec3 eye, float range) const {
    const int x = int(std::floor(eye.x)), z = int(std::floor(eye.z));
    const int radius = int(std::ceil(range)) + Chunk::SIZE_X;
    const auto append = [&](int firstX, int lastX) {
        const auto first =
            looseExterior_.lower_bound({firstX / Chunk::SIZE_X, 0});
        const auto last =
            looseExterior_.upper_bound({lastX / Chunk::SIZE_X, UINT_MAX});
        for (auto it = first; it != last; ++it) {
            const auto& [column, exterior] = *it;
            const auto dx =
                wrap.deltaTile(int(column.first) * Chunk::SIZE_X, x);
            const auto dz = int(column.second) * Chunk::SIZE_Z - z;
            if (std::abs(dx) <= radius && std::abs(dz) <= radius)
                sources.push_back(exterior.source);
        }
    };
    if (!wrap.enabled()) {
        if (x + radius >= 0) append(std::max(0, x - radius), x + radius);
    } else if (2 * radius + Chunk::SIZE_X >= wrap.tiles) {
        append(0, wrap.tiles - 1);
    } else {
        const int first = wrap.wrapTile(x - radius),
                  last = wrap.wrapTile(x + radius);
        if (first > last) {
            append(first, wrap.tiles - 1);
            append(0, last);
        } else {
            append(first, last);
        }
    }
}

void SnowCover::clear() {
    looseExterior_.clear();
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
