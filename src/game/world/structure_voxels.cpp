#include "structure_voxels.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>

#include "terrain_edits.hpp"

namespace structure_voxels {

SolidCellSnapshot Slab::solidCellAt(uint16_t cell) const {
    const auto id = tileAt(cell);
    const auto shape = shapeAt(cell);
    const ShapedTile base{Tile{.block_id = id, .lightLevel = {}}, shape};
    if (const auto it = pieces.find(cell); it != pieces.end())
        return {base, {}, it->second};
    if (id) return {base, CellPiece{{0, id, shape}, {PieceOrigin::Structure}}};
    return {base};
}

void Slab::setPieces(uint16_t cell, std::span<const CellPiece> replacement) {
    if (replacement.empty()) {
        erase(cell);
        return;
    }
    auto sorted = std::make_shared<std::vector<CellPiece>>(replacement.begin(),
                                                           replacement.end());
    std::ranges::sort(*sorted, {},
                      [](const CellPiece& piece) { return piece.geometry.id; });
    auto& first = sorted->front().geometry;
    const auto shape = first.legacyShape();
    if (sorted->size() == 1 && shape) first = {first.id, first.blockId, *shape};
    if (blocks.empty()) blocks.assign(Cells, block);
    blocks[cell] = first.blockId;
    bits.set(cell);
    setShape(cell, shape.value_or(TileShape{}));
    pieces.insert_or_assign(cell, std::move(sorted));
}

void Slab::erase(uint16_t cell) {
    bits.reset(cell);
    setShape(cell, {});
    pieces.erase(cell);
}

void Slab::merge(const Slab& other) {
    for (const auto& [cell, layer] : other.snow) snow.try_emplace(cell, layer);
    enclosed |= other.enclosed;
    const auto added = other.bits & ~bits;
    for (const auto& [cell, shape] : other.shapes)
        if (added.test(cell)) setShape(cell, shape);
    for (const auto& [cell, data] : other.pieces)
        if (added.test(cell)) pieces.emplace(cell, data);
    if (blocks.empty() && other.blocks.empty() && block == other.block) {
        bits |= other.bits;
        return;
    }
    if (blocks.empty()) blocks.assign(Cells, block);
    if (added.none()) return;
    for (size_t i = 0; i < Cells; ++i)
        if (added.test(i)) blocks[i] = other.blockAt(i);
    bits |= added;
}

void Occupancy::add(int sx, int sy, int sz) {
    for (int level = 0; level <= MaxLevel; ++level) {
        // Floor division by 2^level of possibly negative coordinates.
        const auto down = [&](int v) { return v >> level; };
        if (!slabs[size_t(level)]
                 .insert(packSlab(down(sx), down(sy), down(sz)))
                 .second &&
            level > 0)
            break;  // its parents are listed already
    }
}

std::optional<ChunkCoord> keyOf(int level, int sx, int sy, int sz,
                                const WorldWrap& wrap) {
    if (sy < 0 || sz < 0 || sy * spanOf(level) >= WORLD_HEIGHT_Y)
        return std::nullopt;
    int64_t x = sx;
    if (wrap.enabled()) {
        const int chunksX = wrap.tiles / spanOf(level);
        if (chunksX <= 0) return std::nullopt;
        x = WorldWrap::mod(x, chunksX);
    } else if (x < 0) {
        return std::nullopt;
    }
    return withLevel(ChunkCoord{unsigned(x), unsigned(sy), unsigned(sz)},
                     level);
}

namespace {

int floorDiv(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

TileBox slabBox(int level, int sx, int sy, int sz) {
    const int s = spanOf(level);
    return {sx * s, sy * s, sz * s, (sx + 1) * s, (sy + 1) * s, (sz + 1) * s};
}

}  // namespace

Kind::Kind(int reach, std::vector<Instance> instances, std::string id)
    : reach(reach), kindId(std::move(id)) {
    if (reach < 0) throw std::invalid_argument("Negative structure reach");
    for (auto& instance : instances) {
        if (!instance.source || instance.source->bounds.empty()) continue;
        const auto& b = instance.source->bounds;
        if (std::max({int64_t(instance.anchorX) - b.x0,
                      int64_t(b.x1) - 1 - instance.anchorX,
                      int64_t(instance.anchorZ) - b.z0,
                      int64_t(b.z1) - 1 - instance.anchorZ}) > reach)
            throw std::invalid_argument("Structure extends beyond its reach");
        if (instance.landmark) landmarks.push_back(instance.source);
        regions[{floorDiv(instance.anchorX, WorldWrap::RegionSize),
                 floorDiv(instance.anchorZ, WorldWrap::RegionSize)}]
            .push_back(std::move(instance));
    }
}

std::vector<Instance> Kind::instances() const {
    std::vector<Instance> out;
    for (const auto& [region, entries] : regions)
        out.insert(out.end(), entries.begin(), entries.end());
    return out;
}

void Kind::addTo(Column& column, const WorldWrap& wrap) const {
    constexpr int region = WorldWrap::RegionSize;
    const auto& b = column.box;
    const int x0 = floorDiv(b.x0 - reach, region);
    int x1 = floorDiv(b.x1 - 1 + reach, region);
    if (wrap.enabled()) x1 = std::min(x1, x0 + wrap.regions() - 1);
    const int z0 = floorDiv(b.z0 - reach, region);
    const int z1 = floorDiv(b.z1 - 1 + reach, region);
    if (uint64_t(x1 - x0 + 1) * uint64_t(z1 - z0 + 1) > regions.size()) {
        for (const auto& [anchor, entries] : regions) {
            const int64_t dx =
                WorldWrap::mod(int64_t(anchor.x) - x0, wrap.regions());
            if (dx < 0 || dx > x1 - x0 || anchor.y < z0 || anchor.y > z1)
                continue;
            for (const auto& instance : entries) column.add(instance, wrap);
        }
        return;
    }
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (const auto found = regions.find({int(wrap.wrapRegion(x)), z});
                found != regions.end())
                for (const auto& instance : found->second)
                    column.add(instance, wrap);
}

void Kind::appendLandmarks(std::vector<SourcePtr>& sources,
                           const WorldWrap& wrap, const Vec3& eye,
                           float range) const {
    for (const auto& source : landmarks) {
        const auto& b = source->bounds;
        const double halfX = b.sizeX() * 0.5;
        const double dx =
            std::max(0.0, std::abs(wrap.deltaX(b.x0 + halfX, eye.x)) - halfX);
        const double dz =
            std::max({0.0, double(b.z0) - eye.z, double(eye.z) - b.z1});
        if (dx * dx + dz * dz <= double(range) * range)
            sources.push_back(source);
    }
}

Column::Column(unsigned cx, unsigned cz)
    : box{int(cx) * Side, 0,
          int(cz) * Side, int(cx + 1) * Side,
          WORLD_HEIGHT_Y, int(cz + 1) * Side} {}

void Column::add(const Instance& instance, const WorldWrap& wrap) {
    if (!instance.source) return;
    if (!wrap.enabled()) {
        add(instance.source, 0);
        return;
    }
    const auto& b = instance.source->bounds;
    const int first = floorDiv(box.x0 - b.x1, wrap.tiles) + 1;
    const int last = floorDiv(box.x1 - 1 - b.x0, wrap.tiles);
    for (int copy = first; copy <= last; ++copy)
        add(instance.source, copy * wrap.tiles);
}

void Column::add(const SourcePtr& source, int shiftX) {
    if (!source) return;
    auto image = box;
    image.x0 -= shiftX;
    image.x1 -= shiftX;
    if (!source->bounds.intersect(image).empty())
        entries.push_back({source, image.x0 / Side});
}

void Column::orderByKey() {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return a.source->key < b.source->key;
                     });
}

bool Column::mayContain(unsigned cy) const {
    if (empty() || cy >= Y_CHUNKS) return false;
    if (projectedTop()) return occupied.test(cy);
    const int base = int(cy) * Side;
    return std::any_of(entries.begin(), entries.end(), [&](const Entry& e) {
        return base < e.source->bounds.y1 && base + Side > e.source->bounds.y0;
    });
}

std::optional<Slab> Column::rasterize(unsigned cy) const {
    if (!mayContain(cy)) return std::nullopt;
    std::optional<Slab> result;
    const int sy = int(cy), sz = box.z0 / Side;
    for (const auto& entry : entries) {
        if (entry.source->bounds.intersect(slabBox(0, entry.sx, sy, sz))
                .empty())
            continue;
        const auto occupancy = entry.source->occupancy();
        if (occupancy && !occupancy->contains(0, entry.sx, sy, sz)) continue;
        auto slab = entry.source->rasterize(0, entry.sx, sy, sz);
        if (!slab || slab->bits.none()) continue;
        if (result)
            result->merge(*slab);
        else
            result = std::move(slab);
        if (result->bits.all()) break;
    }
    return result;
}

const Column::Tops& Column::skyTops() const {
    std::call_once(topsOnce, [&] {
        tops.fill(INT_MIN);
        occupied.reset();
        std::vector<int> projected;
        for (const auto& entry : entries) {
            auto image = box;
            image.x0 = entry.sx * Side;
            image.x1 = image.x0 + Side;
            entry.source->skyTops(image, projected);
            for (size_t i = 0; i < tops.size(); ++i)
                tops[i] = std::max(tops[i], projected[i]);
            const int top =
                *std::max_element(projected.begin(), projected.end());
            if (top == INT_MIN) continue;
            const auto occupancy = entry.source->occupancy();
            const int first =
                std::max(0, floorDiv(entry.source->bounds.y0, Side));
            for (int sy = first; sy <= top / Side; ++sy)
                if (!occupancy ||
                    occupancy->contains(0, entry.sx, sy, box.z0 / Side))
                    occupied.set(size_t(sy));
        }
        highest = *std::max_element(tops.begin(), tops.end());
    });
    return tops;
}

std::optional<int> Column::projectedTop() const {
    if (empty()) return INT_MIN;
    const int top = highest.load();
    return top == INT_MAX ? std::nullopt : std::optional{top};
}

Column::Tops Column::skyTops(const ColumnEdit& edit) const {
    auto result = skyTops();
    if (edit.removed.empty()) return result;
    std::vector<int> projected;
    for (int z = 0; z < Side; ++z)
        for (int x = 0; x < Side; ++x) {
            auto& top = result[size_t(z) * Side + x];
            while (top >= 0 && edit.suppresses(x, top, z)) {
                int next = INT_MIN;
                for (const auto& entry : entries) {
                    const int wx = entry.sx * Side + x, wz = box.z0 + z;
                    entry.source->skyTops({wx, 0, wz, wx + 1, top, wz + 1},
                                          projected);
                    if (!projected.empty()) next = std::max(next, projected[0]);
                }
                top = next;
            }
        }
    return result;
}

std::bitset<Y_CHUNKS> Column::exactSkySlabs() const {
    std::bitset<Y_CHUNKS> result;
    for (const auto& entry : entries) {
        auto image = box;
        image.x0 = entry.sx * Side;
        image.x1 = image.x0 + Side;
        const auto exact = image.intersect(entry.source->exactSkyBounds());
        if (exact.empty()) continue;
        for (int y = exact.y0 / Side; y <= (exact.y1 - 1) / Side; ++y)
            result.set(y);
    }
    return result;
}

int stamp(Chunk& chunk, const Slab& slab) {
    int placed = 0;
    for (int z = 0; z < Side; ++z)
        for (int y = 0; y < Side; ++y)
            for (int x = 0; x < Side; ++x) {
                const size_t i = cellIndex(x, y, z);
                auto tile = chunk.tiles[z][y][x];
                if (!slab.bits.test(i) || !tile.isAir()) continue;
                if (const auto entry = slab.pieces.find(uint16_t(i));
                    entry != slab.pieces.end()) {
                    if (chunk.setPieces(uint16_t(i), *entry->second,
                                        {PieceOrigin::Structure},
                                        true) != CellPieceEdit::Applied)
                        throw std::invalid_argument("invalid structure pieces");
                } else {
                    tile.block_id = slab.blockAt(i);
                    chunk.setCell(uint16_t(i), tile, slab.shapeAt(uint16_t(i)));
                }
                ++placed;
            }
    return placed;
}

TileBox boxOf(const Planned& p) {
    return slabBox(levelOf(p.key), p.sx, p.sy, p.sz);
}

bool intersectsWrapped(const TileBox& box, const TileBox& other,
                       const WorldWrap& wrap) {
    if (box.empty() || other.empty()) return false;
    const int boxX = box.x0 + box.sizeX() / 2;
    const int otherX = other.x0 + other.sizeX() / 2;
    const int shift = int(wrap.deltaTile(otherX, boxX)) + boxX - otherX;
    auto image = other;
    image.x0 += shift;
    image.x1 += shift;
    return !image.intersect(box).empty();
}

std::vector<Planned> plan(const TileBox& bounds, double eyeX, double eyeY,
                          double eyeZ, const WorldWrap& wrap,
                          const std::unordered_set<ChunkCoord>& previous,
                          const Occupancy* occupancy, const TileBox& fullDetail,
                          std::span<const TileBox> casterDetail) {
    std::vector<Planned> out;
    if (bounds.empty()) return out;
    // Distance from the eye to the part of a cell the structure fills.
    const auto distance = [&](const TileBox& box) {
        const double hx = box.sizeX() * 0.5;
        const double cx = box.x0 + hx;
        const double dx = std::max(
            0.0,
            std::abs(wrap.enabled() ? wrap.deltaX(cx, eyeX) : cx - eyeX) - hx);
        const double dy =
            std::max({0.0, double(box.y0) - eyeY, eyeY - double(box.y1)});
        const double dz =
            std::max({0.0, double(box.z0) - eyeZ, eyeZ - double(box.z1)});
        return float(std::sqrt(dx * dx + dy * dy + dz * dz));
    };
    const auto had = [&](int level, int sx, int sy, int sz) {
        const auto key = keyOf(level, sx, sy, sz, wrap);
        return key && previous.contains(*key);
    };
    const auto split = [&](auto&& self, int level, int sx, int sy,
                           int sz) -> void {
        if (occupancy && !occupancy->contains(level, sx, sy, sz)) return;
        const auto part = bounds.intersect(slabBox(level, sx, sy, sz));
        if (part.empty()) return;
        const float d = distance(part);
        bool finer = false;
        if (level > 0) {
            // Was this cell drawn at this level or coarser last time? Then
            // it splits only once clearly inside the finer band.
            bool coarse = false;
            for (int l = level; l <= MaxLevel && !coarse; ++l) {
                const int shift = l - level;
                coarse = had(l, sx >> shift, sy >> shift, sz >> shift);
            }
            const float limit = Limits[level - 1];
            const float margin =
                previous.empty() ? 0.f : (coarse ? -Margin : Margin);
            finer = d < limit + margin;
            if (!finer) {
                const auto box = slabBox(level, sx, sy, sz);
                finer =
                    intersectsWrapped(box, fullDetail, wrap) ||
                    std::any_of(casterDetail.begin(), casterDetail.end(),
                                [&](const TileBox& detail) {
                                    return intersectsWrapped(box, detail, wrap);
                                });
            }
        }
        if (!finer) {
            if (const auto key = keyOf(level, sx, sy, sz, wrap))
                out.push_back({*key, sx, sy, sz, d});
            return;
        }
        for (int dz = 0; dz < 2; ++dz)
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                    self(self, level - 1, sx * 2 + dx, sy * 2 + dy,
                         sz * 2 + dz);
    };
    const int s = spanOf(MaxLevel);
    const int sy0 = std::max(0, floorDiv(bounds.y0, s)),
              sy1 = std::min((WORLD_HEIGHT_Y + s - 1) / s - 1,
                             floorDiv(bounds.y1 - 1, s));
    const int sz0 = std::max(0, floorDiv(bounds.z0, s)),
              sz1 = floorDiv(bounds.z1 - 1, s);
    for (int sz = sz0; sz <= sz1; ++sz)
        for (int sy = sy0; sy <= sy1; ++sy)
            for (int sx = floorDiv(bounds.x0, s);
                 sx <= floorDiv(bounds.x1 - 1, s); ++sx)
                split(split, MaxLevel, sx, sy, sz);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.distance < b.distance;
    });
    return out;
}

void Source::skyTops(const TileBox& columns, std::vector<int>& tops) const {
    const int width = columns.x1 - columns.x0;
    const int depth = columns.z1 - columns.z0;
    tops.assign(columns.empty() ? 0 : size_t(width) * size_t(depth), INT_MIN);
    const TileBox box = columns.intersect(bounds);
    if (box.empty()) return;
    const auto occupied = occupancy();
    const auto floorDiv = [](int a) {
        return a >= 0 ? a / Side : -((-a + Side - 1) / Side);
    };
    for (int sz = floorDiv(box.z0); sz <= floorDiv(box.z1 - 1); ++sz)
        for (int sx = floorDiv(box.x0); sx <= floorDiv(box.x1 - 1); ++sx) {
            // Tiles of the box in this slab column still without a top.
            const int tx0 = std::max(box.x0, sx * Side);
            const int tx1 = std::min(box.x1, (sx + 1) * Side);
            const int tz0 = std::max(box.z0, sz * Side);
            const int tz1 = std::min(box.z1, (sz + 1) * Side);
            int open = (tx1 - tx0) * (tz1 - tz0);
            for (int sy = floorDiv(box.y1 - 1); sy >= floorDiv(box.y0) && open;
                 --sy) {
                if (occupied && !occupied->contains(0, sx, sy, sz)) continue;
                const auto slab = rasterize(0, sx, sy, sz);
                if (!slab) continue;
                for (int z = tz0; z < tz1; ++z)
                    for (int x = tx0; x < tx1; ++x) {
                        int& top = tops[size_t(z - columns.z0) * width +
                                        size_t(x - columns.x0)];
                        if (top != INT_MIN) continue;
                        for (int ly = Side - 1; ly >= 0; --ly) {
                            const int y = sy * Side + ly;
                            if (y < box.y0 || y >= box.y1) continue;
                            if (slab->bits.test(cellIndex(x - sx * Side, ly,
                                                          z - sz * Side))) {
                                top = y;
                                --open;
                                break;
                            }
                        }
                    }
            }
        }
}

Slabs rasterizeAll(const Source& source, int level, const WorldWrap& wrap) {
    Slabs out;
    const auto& b = source.bounds;
    if (b.empty()) return out;
    const int s = spanOf(level);
    for (int sz = std::max(0, floorDiv(b.z0, s)); sz <= floorDiv(b.z1 - 1, s);
         ++sz)
        for (int sy = std::max(0, floorDiv(b.y0, s));
             sy <= floorDiv(b.y1 - 1, s); ++sy)
            for (int sx = floorDiv(b.x0, s); sx <= floorDiv(b.x1 - 1, s);
                 ++sx) {
                const auto key = keyOf(level, sx, sy, sz, wrap);
                if (!key) continue;
                if (auto slab = source.rasterize(level, sx, sy, sz))
                    out[*key] = std::move(*slab);
            }
    markEnclosed(out, wrap);
    return out;
}

void markEnclosed(Slabs& slabs, const WorldWrap& wrap) {
    const auto full = [&](ChunkCoord c, int dx, int dy, int dz) {
        const int level = levelOf(c);
        const auto base = baseOf(c);
        const auto key = keyOf(level, int(base.x) + dx, int(base.y) + dy,
                               int(base.z) + dz, wrap);
        if (!key) return false;
        const auto it = slabs.find(*key);
        return it != slabs.end() && it->second.fullCoverage();
    };
    for (auto& [coord, slab] : slabs)
        slab.enclosed = slab.fullCoverage() && full(coord, 1, 0, 0) &&
                        full(coord, -1, 0, 0) && full(coord, 0, 1, 0) &&
                        full(coord, 0, -1, 0) && full(coord, 0, 0, 1) &&
                        full(coord, 0, 0, -1);
}

FootprintPtr footprint(const Source& source, const Occupancy* occupancy,
                       const Footprint* previous,
                       const std::optional<TileBox>& changed) {
    auto out = std::make_shared<Footprint>();
    const auto& b = source.bounds;
    if (b.empty()) return out;
    constexpr int level = FootprintLevel;
    const int s = spanOf(level), cell = Footprint::Cell;
    const int sx0 = floorDiv(b.x0, s), sx1 = floorDiv(b.x1 - 1, s);
    const int sz0 = floorDiv(b.z0, s), sz1 = floorDiv(b.z1 - 1, s);
    const int sy0 = std::max(0, floorDiv(b.y0, s)),
              sy1 = std::min((WORLD_HEIGHT_Y + s - 1) / s - 1,
                             floorDiv(b.y1 - 1, s));
    // Slab columns to rasterize: all of them, or those under the change.
    int rx0 = sx0, rx1 = sx1, rz0 = sz0, rz1 = sz1;
    if (previous && changed) {
        rx0 = std::max(sx0, floorDiv(changed->x0, s));
        rx1 = std::min(sx1, floorDiv(changed->x1 - 1, s));
        rz0 = std::max(sz0, floorDiv(changed->z0, s));
        rz1 = std::min(sz1, floorDiv(changed->z1 - 1, s));
        const int perSlab = s / cell;
        for (const auto& [key, top] : previous->tops) {
            const auto [cx, cz] = Footprint::unpackColumn(key);
            const int sx = floorDiv(cx, perSlab), sz = floorDiv(cz, perSlab);
            if (sx >= rx0 && sx <= rx1 && sz >= rz0 && sz <= rz1) continue;
            out->tops.emplace(key, top);
        }
    }
    for (int sz = rz0; sz <= rz1; ++sz)
        for (int sx = rx0; sx <= rx1; ++sx) {
            // Top down: the first solid cell of a column is its top, and
            // lower slabs cannot raise it.
            std::bitset<size_t(Side) * Side> topped;
            for (int sy = sy1; sy >= sy0 && !topped.all(); --sy) {
                if (occupancy && !occupancy->contains(level, sx, sy, sz))
                    continue;
                const auto slab = source.rasterize(level, sx, sy, sz);
                if (!slab || slab->bits.none()) continue;
                for (int z = 0; z < Side; ++z)
                    for (int x = 0; x < Side; ++x) {
                        if (topped.test(size_t(z * Side + x))) continue;
                        for (int y = Side - 1; y >= 0; --y) {
                            if (!slab->bits.test(cellIndex(x, y, z))) continue;
                            out->tops.emplace(Footprint::packColumn(
                                                  sx * Side + x, sz * Side + z),
                                              (sy * Side + y + 1) * cell);
                            topped.set(size_t(z * Side + x));
                            break;
                        }
                    }
            }
        }
    return out;
}

}  // namespace structure_voxels
