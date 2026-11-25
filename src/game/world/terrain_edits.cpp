#include "terrain_edits.hpp"

#include <algorithm>

#include "save/chunk_codec.hpp"
#include "structure_voxels.hpp"

namespace {
using namespace structure_voxels;

// Keep the generated source intact for comparisons with actual chunk tiles.
// This view is used for the landmark's distant geometry and sky projection.
class MaskedStructure final : public Source {
   public:
    MaskedStructure(SourcePtr source, EditMap masks, WorldWrap wrap,
                    uint64_t editRevision, bool keepWounds)
        : source(std::move(source)),
          masks(std::move(masks)),
          wrap(wrap),
          keepWounds(keepWounds) {
        key = this->source->key;
        bounds = this->source->bounds;
        revision = this->source->revision ^
                   ((editRevision + 1) * 0x9e3779b97f4a7c15ULL);
    }

    OccupancyPtr occupancy() const override { return source->occupancy(); }

    std::optional<Slab> rasterize(int level, int sx, int sy,
                                  int sz) const override {
        auto out = source->rasterize(level, sx, sy, sz);
        if (!out || masks.empty()) return out;
        const int scale = 1 << level;
        const int x0 = sx * scale, y0 = sy * scale, z0 = sz * scale;
        for (int dz = 0; dz < scale; ++dz)
            for (int dx = 0; dx < scale; ++dx) {
                const auto* edit = editAt((x0 + dx) * Side, (z0 + dz) * Side);
                if (!edit) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    const auto* cells = edit->removals(uint16_t(y0 + dy));
                    if (!cells) continue;
                    if (!level) {
                        for (const auto cell : *cells) out->erase(cell);
                        continue;
                    }
                    adjustCoarse(*out, *cells, level, x0 + dx, y0 + dy, z0 + dz,
                                 dx, dy, dz);
                }
            }
        return out->bits.any() ? std::move(out) : std::nullopt;
    }

    void skyTops(const TileBox& columns,
                 std::vector<int>& tops) const override {
        source->skyTops(columns, tops);
        if (columns.empty() || masks.empty()) return;
        std::vector<int> next;
        for (int z = columns.z0; z < columns.z1; ++z)
            for (int x = columns.x0; x < columns.x1; ++x) {
                const auto* edit = editAt(x, z);
                if (!edit) continue;
                auto& top = tops[size_t(z - columns.z0) * columns.sizeX() +
                                 size_t(x - columns.x0)];
                const int lx = int(WorldWrap::mod(x, Side)), lz = z % Side;
                while (top >= 0 && edit->suppresses(lx, top, lz)) {
                    source->skyTops({x, columns.y0, z, x + 1, top, z + 1},
                                    next);
                    top = next.empty() ? INT_MIN : next[0];
                }
            }
    }

    std::optional<TileBox> changedSince(const Source& older) const override {
        const auto* masked = dynamic_cast<const MaskedStructure*>(&older);
        if (!masked || keepWounds != masked->keepWounds) return std::nullopt;
        auto changed = source->revision == masked->source->revision
                           ? std::optional{TileBox{}}
                           : source->changedSince(*masked->source);
        if (!changed || masks == masked->masks) return changed;
        const auto include = [&](const TileBox& box) {
            if (box.empty()) return;
            auto& b = *changed;
            b = b.empty()
                    ? box
                    : TileBox{std::min(b.x0, box.x0), std::min(b.y0, box.y0),
                              std::min(b.z0, box.z0), std::max(b.x1, box.x1),
                              std::max(b.y1, box.y1), std::max(b.z1, box.z1)};
        };
        const auto compare = [&](const MaskedStructure& a,
                                 const MaskedStructure& b) {
            for (const auto& [key, column] : a.masks) {
                const auto other = b.masks.find(key);
                if (other != b.masks.end() && other->second == column) continue;
                const int x = int(key >> 32) * Side,
                          z = int(uint32_t(key)) * Side;
                const int first =
                    wrap.enabled()
                        ? a.bounds.x0 -
                              int(WorldWrap::mod(a.bounds.x0 - x, wrap.tiles))
                        : x;
                for (const auto& [sy, cells] : column->removed) {
                    const auto* prior = other == b.masks.end()
                                            ? nullptr
                                            : other->second->removals(sy);
                    if (prior && *prior == cells) continue;
                    const int y = int(sy) * Side;
                    for (int sx = first; sx < a.bounds.x1; sx += wrap.tiles) {
                        include(TileBox{sx, y, z, sx + Side, y + Side, z + Side}
                                    .intersect(a.bounds));
                        if (!wrap.enabled()) break;
                    }
                }
            }
        };
        compare(*this, *masked);
        compare(*masked, *this);
        return changed;
    }

   private:
    const ColumnEdit* editAt(int x, int z) const {
        const auto wx = wrap.wrapTile(x);
        if (wx < 0 || z < 0) return nullptr;
        const auto found =
            masks.find(editKey(unsigned(wx / Side), unsigned(z / Side)));
        return found == masks.end() ? nullptr : found->second.get();
    }

    void adjustCoarse(Slab& coarse, const std::vector<uint16_t>& cells,
                      int level, int sx, int sy, int sz, int dx, int dy,
                      int dz) const {
        const int scale = 1 << level;
        const auto exact = source->rasterize(0, sx, sy, sz);
        if (!exact) return;
        auto remaining = exact->bits;
        std::bitset<Cells> touched;
        for (const int cell : cells) {
            if (!remaining.test(size_t(cell))) continue;
            remaining.reset(size_t(cell));
            touched.set(cellIndex((dx * Side + cell % Side) / scale,
                                  (dy * Side + cell / Side % Side) / scale,
                                  (dz * Side + cell / (Side * Side)) / scale));
        }
        // A carved tile should leave a trace even at 2 m and 4 m resolution.
        if (keepWounds) {
            coarse.bits &= ~touched;
            return;
        }
        const int cellsPerAxis = Side / scale;
        for (int z = 0; z < Side; z += scale)
            for (int y = 0; y < Side; y += scale)
                for (int x = 0; x < Side; x += scale) {
                    const auto coarseCell =
                        cellIndex(dx * cellsPerAxis + x / scale,
                                  dy * cellsPerAxis + y / scale,
                                  dz * cellsPerAxis + z / scale);
                    if (!touched.test(coarseCell) ||
                        !coarse.bits.test(coarseCell))
                        continue;
                    int count = 0;
                    uint8_t block = 0;
                    for (int lz = z; lz < z + scale; ++lz)
                        for (int ly = y; ly < y + scale; ++ly)
                            for (int lx = x; lx < x + scale; ++lx) {
                                const auto i = cellIndex(lx, ly, lz);
                                if (!remaining.test(i)) continue;
                                ++count;
                                if (!block || exact->blockAt(i) ==
                                                  coarse.blockAt(coarseCell))
                                    block = exact->blockAt(i);
                            }
                    if (count * 4 < scale * scale * scale) {
                        coarse.bits.reset(coarseCell);
                    } else if (block != coarse.blockAt(coarseCell)) {
                        if (coarse.blocks.empty())
                            coarse.blocks.assign(Cells, coarse.block);
                        coarse.blocks[coarseCell] = block;
                    }
                }
    }

    SourcePtr source;
    EditMap masks;
    WorldWrap wrap;
    bool keepWounds;
};
}  // namespace

void ColumnEdit::finish() {
    std::sort(structure.begin(), structure.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    structureTop = -1;
    for (const auto& [slabY, blob] : structure) {
        Chunk chunk;
        chunk_codec::decode(*blob, chunk);
        const int top = chunk_codec::highestSolidY(chunk);
        if (top >= 0)
            structureTop =
                std::max(structureTop, int(slabY) * Chunk::SIZE_Y + top);
    }
}

const ChunkBlob* ColumnEdit::slab(uint16_t slabY) const {
    const auto it = std::lower_bound(
        structure.begin(), structure.end(), slabY,
        [](const auto& entry, uint16_t y) { return entry.first < y; });
    return it != structure.end() && it->first == slabY ? &it->second : nullptr;
}

const std::vector<uint16_t>* ColumnEdit::removals(uint16_t slabY) const {
    const auto it = std::lower_bound(
        removed.begin(), removed.end(), slabY,
        [](const auto& entry, uint16_t y) { return entry.first < y; });
    return it != removed.end() && it->first == slabY ? &it->second : nullptr;
}

bool ColumnEdit::suppresses(int x, int y, int z) const {
    const auto* cells = removals(uint16_t(y / Chunk::SIZE_Y));
    return cells && std::binary_search(
                        cells->begin(), cells->end(),
                        structure_voxels::cellIndex(x, y % Chunk::SIZE_Y, z));
}

std::shared_ptr<const ColumnEdit> TerrainEdits::find(uint64_t key) const {
    const auto it = map->find(key);
    return it == map->end() ? nullptr : it->second;
}

TerrainEdits::TerrainEdits(EditMap initial)
    : map(std::make_shared<const EditMap>(std::move(initial))) {
    for (const auto& [key, edit] : *map)
        if (!edit->removed.empty()) ++columnsWithRemovals;
}

void TerrainEdits::set(uint64_t key, std::shared_ptr<const ColumnEdit> edit) {
    const auto old = find(key);
    const bool hadRemovals = old && !old->removed.empty();
    const bool hasRemovals = !edit->removed.empty();
    auto next = std::make_shared<EditMap>(*map);
    (*next)[key] = std::move(edit);
    map = std::move(next);
    columnsWithRemovals = columnsWithRemovals - hadRemovals + hasRemovals;
    maskedStructures.clear();
    ++rev;
    dirty = true;
    changed.emit(key);
}

void TerrainEdits::erase(uint64_t key) {
    const auto old = find(key);
    if (!old) return;
    auto next = std::make_shared<EditMap>(*map);
    next->erase(key);
    map = std::move(next);
    if (!old->removed.empty()) --columnsWithRemovals;
    maskedStructures.clear();
    ++rev;
    dirty = true;
    changed.emit(key);
}

structure_voxels::SourcePtr TerrainEdits::maskStructure(
    const structure_voxels::SourcePtr& source, const WorldWrap& wrap,
    bool keepWounds) const {
    if (!columnsWithRemovals && !keepWounds) return source;
    auto& cached = maskedStructures[source->key];
    if (cached.sourceRevision == source->revision &&
        cached.keepWounds == keepWounds)
        if (auto result = cached.result.lock()) return result;
    EditMap masks;
    const auto& b = source->bounds;
    for (const auto& [key, edit] : *map) {
        if (edit->removed.empty()) continue;
        const int64_t z = int64_t(uint32_t(key)) * Chunk::SIZE_Z;
        if (z >= b.z1 || z + Chunk::SIZE_Z <= b.z0) continue;
        const int64_t x = int64_t(key >> 32) * Chunk::SIZE_X;
        const auto from = wrap.enabled() ? wrap.wrapTile(x - b.x0) : x - b.x0;
        if ((from < b.sizeX() && from + Chunk::SIZE_X > 0) ||
            (wrap.enabled() && wrap.tiles - from < Chunk::SIZE_X))
            masks.emplace(key, edit);
    }
    auto result = masks.empty() && !keepWounds
                      ? source
                      : std::make_shared<const MaskedStructure>(
                            source, std::move(masks), wrap, rev, keepWounds);
    cached = {result, source->revision, keepWounds};
    return result;
}

WaterSurface ColumnEdit::waterAt(int index, WaterSurface water) const {
    const int cap = reservoirCeiling[index];
    const int fixed = waterY[index];
    if (fixed != GeneratedWater && !cap) {
        if (fixed == water.surfaceY && fixed > groundY[index] + 1) return water;
        water = {fixed, 0, water.reach, water.flow, true};
    } else if (cap) {
        if (water.surfaceY > cap) {
            water.surfaceY = cap;
            water.topFill = 255;
        }
        if (fixed > water.surfaceY) water = {fixed, 0, 0, {}, true};
    }
    return water.surfaceY > groundY[index] + 1 ? water : WaterSurface{};
}
