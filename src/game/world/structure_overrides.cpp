#include "structure_overrides.hpp"

#include <algorithm>
#include <stdexcept>

namespace structure_voxels {
namespace {

constexpr int CoordinateLimit = 1 << 23;

TileBox united(const TileBox& a, const TileBox& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::min(a.z0, b.z0),
            std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::max(a.z1, b.z1)};
}

class OverrideSource final : public Source {
   public:
    OverrideSource(uint64_t id, const Override& record, SourcePtr original)
        : original(std::move(original)), recordRevision(record.revision) {
        key = id;
        revision = ((record.revision + 1) * 0x9e3779b97f4a7c15ULL) ^
                   (this->original ? this->original->revision : 0);
        if (this->original) bounds = this->original->bounds;
        for (const auto& [pos, voxel] : record.voxels) {
            const auto [x, y, z] = pos;
            patches[packSlab(x >> 4, y >> 4, z >> 4)].emplace_back(
                uint16_t(cellIndex(x & 15, y & 15, z & 15)), voxel);
            if (!voxel.pieces.empty())
                pieceBounds =
                    united(pieceBounds, {x, y, z, x + 1, y + 1, z + 1});
            patchBounds = united(patchBounds, {x, y, z, x + 1, y + 1, z + 1});
        }
        bounds = united(bounds, patchBounds);
        auto occupied = std::make_shared<Occupancy>();
        for (const auto& [pos, voxel] : record.voxels)
            occupied->add(pos[0] >> 4, pos[1] >> 4, pos[2] >> 4);
        patchOccupancy = std::move(occupied);
    }

    TileBox exactSkyBounds() const override {
        return original ? united(pieceBounds, original->exactSkyBounds())
                        : pieceBounds;
    }

    OccupancyPtr occupancy() const override {
        if (!original) return patchOccupancy;
        std::call_once(occupancyOnce, [&] {
            const auto base = original->occupancy();
            if (!base) return;
            auto combined = std::make_shared<Occupancy>(*base);
            for (int level = 0; level <= MaxLevel; ++level) {
                const auto& keys = patchOccupancy->slabs[size_t(level)];
                combined->slabs[size_t(level)].insert(keys.begin(), keys.end());
            }
            occupiedSlabs = std::move(combined);
        });
        return occupiedSlabs;
    }

    std::optional<Slab> rasterize(int level, int sx, int sy,
                                  int sz) const override {
        if (level == 0) return exact(sx, sy, sz);
        const int scale = 1 << level;
        const int span = spanOf(level);
        const TileBox box{sx * span,       sy * span,       sz * span,
                          (sx + 1) * span, (sy + 1) * span, (sz + 1) * span};
        if (box.intersect(patchBounds).empty())
            return original ? original->rasterize(level, sx, sy, sz)
                            : std::nullopt;
        Slab out;
        out.blocks.resize(Cells);
        std::array<uint8_t, Cells> counts{};
        for (int dz = 0; dz < scale; ++dz)
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx) {
                    const auto fine = exact(sx * scale + dx, sy * scale + dy,
                                            sz * scale + dz);
                    if (!fine) continue;
                    for (int z = 0; z < Side; ++z)
                        for (int y = 0; y < Side; ++y)
                            for (int x = 0; x < Side; ++x) {
                                const auto block =
                                    fine->tileAt(cellIndex(x, y, z));
                                if (!block) continue;
                                const auto i =
                                    cellIndex((dx * Side + x) / scale,
                                              (dy * Side + y) / scale,
                                              (dz * Side + z) / scale);
                                if (++counts[i] == 1) out.blocks[i] = block;
                                if (counts[i] * 4 >= scale * scale * scale)
                                    out.bits.set(i);
                            }
                }
        return out.bits.any() ? std::optional{std::move(out)} : std::nullopt;
    }

    std::optional<TileBox> changedSince(const Source& older) const override {
        const auto* prior = dynamic_cast<const OverrideSource*>(&older);
        const auto* oldBase = prior ? prior->original.get() : &older;
        TileBox changed =
            prior ? united(prior->patchBounds, patchBounds) : patchBounds;
        if (!original || !oldBase)
            return united(changed, united(bounds, older.bounds));
        if (original->revision == oldBase->revision) {
            if (prior && prior->recordRevision == recordRevision)
                return TileBox{};
            return changed;
        }
        const auto baseChange = original->changedSince(*oldBase);
        return baseChange ? std::optional{united(changed, *baseChange)}
                          : std::nullopt;
    }

   private:
    std::optional<Slab> exact(int sx, int sy, int sz) const {
        auto result =
            original ? original->rasterize(0, sx, sy, sz) : std::nullopt;
        const auto at = patches.find(packSlab(sx, sy, sz));
        if (at == patches.end()) return result;
        if (!result) result.emplace();
        if (result->blocks.empty()) result->blocks.assign(Cells, result->block);
        for (const auto& [cell, voxel] : at->second) {
            if (!voxel.pieces.empty()) {
                result->setPieces(cell, voxel.pieces);
                continue;
            }
            result->erase(cell);
            result->bits.set(cell, voxel.block != 0);
            result->blocks[cell] = voxel.block;
            result->setShape(cell, voxel.shape);
        }
        return result->bits.any() ? std::move(result) : std::nullopt;
    }

    SourcePtr original;
    uint64_t recordRevision;
    TileBox pieceBounds;
    TileBox patchBounds;
    std::unordered_map<uint64_t,
                       std::vector<std::pair<uint16_t, OverrideVoxel>>>
        patches;
    OccupancyPtr patchOccupancy;
    mutable std::once_flag occupancyOnce;
    mutable OccupancyPtr occupiedSlabs;
};

}  // namespace

void validateOverride(std::string_view kind, const Override& record) {
    if (kind.empty() || kind.size() > 64 ||
        record.mode > OverrideMode::Replace ||
        (record.mode == OverrideMode::Removed) != record.voxels.empty() ||
        record.anchorX < 0 || record.anchorX >= CoordinateLimit ||
        record.anchorZ < 0 || record.anchorZ >= CoordinateLimit)
        throw std::invalid_argument("invalid structure override");
    for (const auto& [pos, voxel] : record.voxels) {
        const auto [x, y, z] = pos;
        if (x < -CoordinateLimit || x >= CoordinateLimit || y < 0 ||
            y >= WORLD_HEIGHT_Y || z < 0 || z >= CoordinateLimit ||
            (!voxel.block && voxel.shape.code()) ||
            (record.mode == OverrideMode::Replace && !voxel.block))
            throw std::invalid_argument("invalid structure override voxel");
        if (voxel.pieces.empty()) continue;
        if (voxel.pieces.size() > Chunk::MaxPiecesPerCell)
            throw std::invalid_argument("too many structure override pieces");
        const auto& first = voxel.pieces.front().geometry;
        if (voxel.block != first.blockId ||
            voxel.shape != first.legacyShape().value_or(TileShape{}))
            throw std::invalid_argument(
                "invalid structure override projection");
        tile_geometry::CellGeometry geometry;
        std::optional<tile_geometry::PieceId> previous;
        for (const auto& piece : voxel.pieces) {
            if ((previous && piece.geometry.id <= *previous) ||
                piece.origin.kind > PieceOrigin::Environment ||
                geometry.tryAdd(piece.geometry) !=
                    tile_geometry::PiecePlacement::Added)
                throw std::invalid_argument("invalid structure override piece");
            previous = piece.geometry.id;
        }
    }
}

std::vector<Kind> applyOverrides(const std::vector<Kind>& generated,
                                 const Overrides& overrides) {
    if (overrides.empty()) return generated;
    std::vector<Kind> result;
    std::unordered_set<std::string> matched;
    const auto apply = [&](const std::string& id, const Kind* base) {
        const auto found = overrides.find(id);
        if (found == overrides.end()) {
            result.push_back(*base);
            return;
        }
        std::vector<Instance> instances =
            base ? base->instances() : std::vector<Instance>{};
        int reach = base ? base->reachTiles() : 0;
        std::unordered_map<uint64_t, size_t> byKey;
        for (size_t i = 0; i < instances.size(); ++i)
            byKey.emplace(instances[i].source->key, i);
        for (const auto& [key, record] : found->second) {
            validateOverride(id, record);
            const auto entry = byKey.find(key);
            if (record.mode == OverrideMode::Removed) {
                if (entry != byKey.end())
                    instances[entry->second].source.reset();
                continue;
            }
            const bool patch = record.mode == OverrideMode::Patch;
            if (patch && entry == byKey.end()) continue;
            Instance next = patch ? instances[entry->second]
                                  : Instance{nullptr, record.anchorX,
                                             record.anchorZ, record.landmark};
            next.source = std::make_shared<OverrideSource>(
                key, record, patch ? next.source : nullptr);
            const auto& b = next.source->bounds;
            reach =
                std::max({reach, next.anchorX - b.x0, b.x1 - 1 - next.anchorX,
                          next.anchorZ - b.z0, b.z1 - 1 - next.anchorZ});
            if (entry != byKey.end())
                instances[entry->second] = std::move(next);
            else
                instances.push_back(std::move(next));
        }
        result.emplace_back(reach, std::move(instances), id);
    };
    for (const auto& kind : generated) {
        apply(kind.id(), &kind);
        matched.insert(kind.id());
    }
    for (const auto& [id, records] : overrides)
        if (!matched.contains(id)) apply(id, nullptr);
    return result;
}

}  // namespace structure_voxels
