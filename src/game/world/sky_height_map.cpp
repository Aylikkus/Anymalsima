#include "sky_height_map.hpp"

#include <algorithm>
#include <climits>
#include <iostream>

#include "light_volume.hpp"
#include "sunlight_propagator.hpp"
#include "surface_sampler.hpp"
#include "world_editor.hpp"

namespace {
// Builds in flight; each is a few milliseconds (an elder's column: one
// skeleton projection per tree overlapping it).
constexpr size_t MaxJobs = 4;

struct BuildInputs {
    std::shared_ptr<const ChunkColumnInputs> terrain;
    structure_voxels::ColumnPtr structures;
    std::shared_ptr<const ColumnEdit> edit;
    std::shared_ptr<const sky::OpacityTable> opacity;
};

sky::Tile buildTile(const BuildInputs& in) {
    sky::TileInputs tile;
    tile.edit = in.edit;
    tile.structures = in.structures.get();
    tile.opacity = in.opacity.get();
    for (size_t i = 0; i < sky::Tile::Size; ++i) {
        tile.surface[i] = in.terrain->columns[i].surface_y;
        tile.water[i] = in.terrain->columns[i].water;
    }
    tile.structureTop =
        in.edit ? in.structures->skyTops(*in.edit) : in.structures->skyTops();
    return sky::build(tile);
}

void keepOpaqueStructureTops(const structure_voxels::Source& source,
                             const TileBox& columns,
                             const sky::OpacityTable& opacity,
                             std::vector<int>& tops) {
    using namespace structure_voxels;
    const auto exact = columns.intersect(source.exactSkyBounds());
    if (exact.empty()) return;
    std::unordered_map<ChunkCoord, std::optional<Slab>> slabs;
    for (int z = exact.z0; z < exact.z1; ++z)
        for (int x = exact.x0; x < exact.x1; ++x) {
            auto& top = tops[size_t(z - columns.z0) * columns.sizeX() +
                             size_t(x - columns.x0)];
            if (top < exact.y0 || top >= exact.y1) continue;
            const int sx = (x - int(WorldWrap::mod(x, Side))) / Side;
            const int highest = top;
            top = INT_MIN;
            for (int y = highest; y >= exact.y0; --y) {
                const ChunkCoord key{unsigned(sx), unsigned(y / Side),
                                     unsigned(z / Side)};
                auto [found, inserted] = slabs.try_emplace(key);
                if (inserted)
                    found->second = source.rasterize(0, sx, y / Side, z / Side);
                const auto& slab = found->second;
                if (!slab) continue;
                const auto cell = uint16_t(cellIndex(
                    int(WorldWrap::mod(x, Side)), y % Side, z % Side));
                if (!slab->bits.test(cell)) continue;
                const auto coverage =
                    slab->pieces.contains(cell)
                        ? voxel_light::Cell(voxel_light::coverPieces(
                              slab->solidCellAt(cell), opacity))
                        : voxel_light::Cell(opacity[slab->tileAt(cell)],
                                            slab->shapeAt(cell));
                if (voxel_light::verticalLoss(coverage) == sky::Opaque) {
                    top = y;
                    break;
                }
            }
            if (top == INT_MIN && exact.y0 > columns.y0) {
                std::vector<int> lower;
                source.skyTops({x, columns.y0, z, x + 1, exact.y0, z + 1},
                               lower);
                if (!lower.empty()) top = lower.front();
            }
        }
}

}  // namespace

SkyHeightMap::SkyHeightMap(ChunkManager& chunks, TerrainEdits& edits,
                           const DataStorage<Block>& blocks, ThreadPool& pool)
    : chunks(chunks), edits(edits), blocks(blocks), pool(pool) {
    if (auto* water = chunks.reservoirs())
        waterConnection =
            water->changed.connect([this](const ReservoirChange& change) {
                for (const auto& [key, entry] : entries) {
                    const int x = int(key >> 32) * Chunk::SIZE_X;
                    const int z = int(uint32_t(key)) * Chunk::SIZE_Z;
                    if (!change.affects(this->chunks.world().hydrology.get(), x,
                                        z, x + Chunk::SIZE_X, z + Chunk::SIZE_Z,
                                        this->chunks.wrap().tiles))
                        continue;
                    invalidate(key);
                    if (const auto saved = overridden.find(key);
                        saved != overridden.end() && !entry.queried)
                        for (const auto& [index, column] : saved->second)
                            observations[key].insert(index);
                }
            });
    auto table = std::make_shared<sky::OpacityTable>();
    table->fill(0);
    for (size_t id = 1; id <= blocks.items.size() && id < table->size(); ++id)
        (*table)[id] = blocks.get(int(id)).properties.opacity;
    opacity = std::move(table);
    editsConnection =
        edits.changed.connect([this](uint64_t key) { invalidate(key); });
    structuresConnection = chunks.columnStructuresChanged.connect(
        [this](uint64_t key) { invalidate(key); });
    releasedConnection = chunks.columnReleased.connect([this](uint64_t key) {
        if (entries.erase(key)) ++revision;
        snowTops.erase(key);
        queue.erase({unsigned(key >> 32), 0, unsigned(key)});
        observations.erase(key);
    });
    // A chunk arriving may hold tiles that changed while it was unloaded
    // (tree catch-up): check the overrides in its column again.
    readyConnection = chunks.becameReady.connect([this](const ChunkCoord& c) {
        const auto key = columnKey(c.x, c.z);
        const auto it = overridden.find(key);
        if (it == overridden.end()) return;
        for (const auto& [index, column] : it->second)
            observations[key].insert(index);
    });
}

SkyHeightMap::~SkyHeightMap() {
    if (auto* water = chunks.reservoirs())
        water->changed.disconnect(waterConnection);
    if (editor) editor->blockChanged.disconnect(editConnection);
    edits.changed.disconnect(editsConnection);
    chunks.columnStructuresChanged.disconnect(structuresConnection);
    chunks.columnReleased.disconnect(releasedConnection);
    chunks.becameReady.disconnect(readyConnection);
    for (auto& job : jobs) job.future.wait();
    if (occlusionJob) occlusionJob->wait();
}

void SkyHeightMap::unwatch() {
    if (editor) editor->blockChanged.disconnect(editConnection);
    editor = nullptr;
}

void SkyHeightMap::watch(WorldEditor& source) {
    if (editor) editor->blockChanged.disconnect(editConnection);
    editor = &source;
    editConnection = source.blockChanged.connect([this](const BlockChange& c) {
        if (!c.piecesChanged && c.beforeShape == c.afterShape &&
            tileOpacity(c.before, blocks) == tileOpacity(c.after, blocks))
            return;
        const auto x = unsigned(chunks.wrap().wrapTile(c.pos.x));
        const auto z = unsigned(c.pos.z);
        observations[columnKey(x / Chunk::SIZE_X, z / Chunk::SIZE_Z)].insert(
            sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)));
    });
}

void SkyHeightMap::request(unsigned chunkX, unsigned chunkZ) {
    const auto key = columnKey(chunkX, chunkZ);
    if (!chunks.isRegistered({chunkX, 0, chunkZ})) return;
    auto [it, inserted] = entries.try_emplace(key);
    if (inserted || it->second.queried) invalidate(key);
    it->second.queried = false;
}

void SkyHeightMap::requestUnloaded(int64_t x, int64_t z) {
    constexpr size_t MaxQueried = 64;
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0 ||
        z >= int64_t(chunks.worldHeightRegions()) * TerrainTuning::RegionZ)
        return;
    const auto cx = unsigned(x / Chunk::SIZE_X);
    const auto cz = unsigned(z / Chunk::SIZE_Z);
    if (chunks.isRegistered({cx, 0, cz})) {
        request(cx, cz);
        return;
    }
    const auto key = columnKey(cx, cz);
    if (auto it = entries.find(key); it != entries.end()) {
        it->second.lastQuery = ++queryClock;
        return;
    }
    auto oldest = entries.end();
    size_t count = 0;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it->second.queried) continue;
        ++count;
        if (oldest == entries.end() ||
            it->second.lastQuery < oldest->second.lastQuery)
            oldest = it;
    }
    if (count >= MaxQueried) {
        queue.erase(
            {unsigned(oldest->first >> 32), 0, unsigned(oldest->first)});
        entries.erase(oldest);
        ++revision;
    }
    auto& entry = entries[key];
    entry.queried = true;
    entry.lastQuery = ++queryClock;
    queue.insert({cx, 0, cz});
}

void SkyHeightMap::invalidate(uint64_t key) {
    const auto entry = entries.find(key);
    if (entry == entries.end()) return;
    entry->second.generation = 0;
    queue.insert({unsigned(key >> 32), 0, unsigned(key)});
}

void SkyHeightMap::setOverrides(Overrides value) {
    overridden = std::move(value);
    ++revision;
    for (auto& [key, entry] : entries) refreshEffective(key, entry);
}

void SkyHeightMap::update() {
    // Wood grows with the day the chunks generate for.
    if (const int32_t today = chunks.woodDay(); today != day) {
        day = today;
        for (const auto& [key, entry] : entries)
            if (entry.hasTrees) invalidate(key);
    }
    if (treeRevision != chunks.treeRevision() ||
        structureRevision != chunks.generatedStructureRevision()) {
        treeRevision = chunks.treeRevision();
        structureRevision = chunks.generatedStructureRevision();
        for (const auto& [key, entry] : entries)
            if (entry.queried) invalidate(key);
    }
    collect();
    dispatch();
    observe();
}

void SkyHeightMap::dispatch() {
    if (queue.empty() || jobs.size() >= MaxJobs) return;
    queue.setPeriodX(chunks.wrap().tiles);
    for (const auto& coord : queue.ordered(focus, false)) {
        if (jobs.size() >= MaxJobs) break;
        const auto key = columnKey(coord.x, coord.z);
        const auto entry = entries.find(key);
        if (entry == entries.end()) {
            queue.erase(coord);
            continue;
        }
        auto& e = entry->second;
        if (e.queried) {
            // Leave the other slots available for the streamed world.
            if (jobs.size() >= MaxJobs - 1 ||
                std::any_of(jobs.begin(), jobs.end(),
                            [](const Job& job) { return job.unloaded; }))
                continue;
            auto source = chunks.snapshotColumnSources(coord.x, coord.z);
            queue.erase(coord);
            if (!source) continue;
            e.generation = ++nextGeneration;
            e.hasTrees = !chunks.treeSpecies().empty();
            jobs.push_back({key, e.generation,
                            pool.submit_task(
                                [source = std::move(source),
                                 edit = edits.find(key), opacity = opacity] {
                                    auto inputs = source();
                                    return buildTile({inputs.terrain,
                                                      inputs.structures, edit,
                                                      opacity});
                                },
                                BS::pr::low),
                            true});
            continue;
        }
        // Waits (stays queued) until terrain and trees are ready.
        const auto sources = chunks.columnSources(coord.x, coord.z);
        if (!sources || !sources->terrain) continue;
        queue.erase(coord);
        e.generation = ++nextGeneration;
        e.hasTrees = sources->hasTrees;
        BuildInputs inputs{sources->terrain, sources->structures,
                           edits.find(key), opacity};
        jobs.push_back(
            {key, e.generation,
             pool.submit_task(
                 [inputs = std::move(inputs)] { return buildTile(inputs); },
                 // Short, and every sweep of the column waits for it.
                 BS::pr::high)});
    }
}

void SkyHeightMap::collect() {
    using namespace std::chrono_literals;
    for (size_t i = 0; i < jobs.size();) {
        if (jobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(jobs[i]);
        jobs.erase(jobs.begin() + ptrdiff_t(i));
        sky::Tile tile;
        try {
            tile = job.future.get();
        } catch (const std::exception& error) {
            std::cerr << "Sky heightmap build failed: " << error.what() << '\n';
            const auto entry = entries.find(job.key);
            if (entry != entries.end() &&
                entry->second.generation == job.generation)
                invalidate(job.key);
            continue;
        }
        const auto entry = entries.find(job.key);
        if (entry == entries.end() ||
            entry->second.generation != job.generation)
            continue;
        entry->second.derived =
            std::make_shared<const sky::Tile>(std::move(tile));
        // Overrides the sources now agree with are dropped.
        if (const auto it = overridden.find(job.key); it != overridden.end()) {
            for (auto o = it->second.begin(); o != it->second.end();)
                if (entry->second.derived->column(o->first) == o->second) {
                    o = it->second.erase(o);
                    dirty = true;
                } else {
                    ++o;
                }
            if (it->second.empty()) overridden.erase(it);
        }
        refreshEffective(job.key, entry->second);
    }
}

void SkyHeightMap::refreshEffective(uint64_t key, Entry& entry) {
    if (!entry.derived) return;
    const auto it = overridden.find(key);
    auto effective = entry.derived;
    if (it != overridden.end()) {
        auto tile = std::make_shared<sky::Tile>(*entry.derived);
        for (const auto& [index, column] : it->second) tile->set(index, column);
        effective = std::move(tile);
    }
    if (entry.effective && *entry.effective == *effective) return;
    entry.effective = std::move(effective);
    applySnow(key, entry);
    ++revision;
    changed.emit(key);
}

sky::Column SkyHeightMap::scanLoaded(unsigned cx, unsigned cz, int lx, int lz,
                                     const sky::Column& known) {
    const int from =
        std::min(WORLD_HEIGHT_Y - 1,
                 std::max(known.top(), chunks.getColumnTopY(cx, cz)));
    // Opacity of the loaded slab being scanned, top-down; unloaded slabs
    // keep what the map knew there.
    int cachedSlab = -1;
    bool loaded = false;
    std::array<voxel_light::Cell, Chunk::SIZE_Y> slab{};
    return sky::scanColumn(from, 0, [&](int y) {
        const int s = y / Chunk::SIZE_Y;
        if (s != cachedSlab) {
            cachedSlab = s;
            loaded = chunks.withChunkLocked(
                {cx, unsigned(s), cz}, [&](const Chunk& chunk) {
                    for (int ly = 0; ly < Chunk::SIZE_Y; ++ly) {
                        const auto cell = Chunk::cell(lx, ly, lz);
                        if (chunk.hasCompoundAt(cell)) {
                            slab[size_t(ly)] =
                                voxel_light::Cell(voxel_light::coverPieces(
                                    chunk.solidCellAt(cell), *opacity));
                            continue;
                        }
                        slab[size_t(ly)] = {
                            tileOpacity(chunk.tiles[lz][ly][lx], blocks),
                            chunk.shapeAt(cell)};
                    }
                });
        }
        return loaded ? slab[size_t(y % Chunk::SIZE_Y)] : known.cellAt(y);
    });
}

void SkyHeightMap::observe() {
    for (auto it = observations.begin(); it != observations.end();) {
        const auto key = it->first;
        const auto entry = entries.find(key);
        // Not built yet: observed once it is (the column may also be
        // outside the requested area; then only loaded overrides change).
        if (entry == entries.end() || !entry->second.derived) {
            if (entry == entries.end() &&
                !chunks.isRegistered({unsigned(key >> 32), 0, unsigned(key)}))
                it = observations.erase(it);
            else
                ++it;
            continue;
        }
        const auto cx = unsigned(key >> 32), cz = unsigned(key);
        if (queue.contains({cx, 0, cz}) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const Job& job) {
                return job.key == key &&
                       job.generation == entry->second.generation;
            })) {
            ++it;
            continue;
        }
        bool any = false;
        for (const int index : it->second) {
            const auto& derived = *entry->second.derived;
            const auto before = entry->second.effective->column(index);
            const auto now = scanLoaded(cx, cz, index % Chunk::SIZE_X,
                                        index / Chunk::SIZE_X, before);
            if (now == before) continue;
            auto& columns = overridden[key];
            if (now == derived.column(index))
                columns.erase(index);
            else
                columns[index] = now;
            if (columns.empty()) overridden.erase(key);
            dirty = true;
            any = true;
        }
        if (any) refreshEffective(key, entry->second);
        it = observations.erase(it);
    }
}

void SkyHeightMap::applySnow(uint64_t key, Entry& entry) {
    entry.withSnow.reset();
    const auto found = snowTops.find(key);
    if (!entry.effective || found == snowTops.end()) return;
    auto combined = std::make_shared<sky::Tile>(*entry.effective);
    for (int i = 0; i < sky::Tile::Size; ++i) {
        const int snow = found->second[i];
        if (snow <= combined->opaqueTop(i)) continue;
        const auto substrate = combined->column(i);
        combined->set(i, sky::scanColumn(
                             std::max(snow, substrate.top()), snow, [&](int y) {
                                 return y == snow
                                            ? voxel_light::Cell(sky::Opaque)
                                            : substrate.cellAt(y);
                             }));
    }
    entry.withSnow = std::move(combined);
}

void SkyHeightMap::setSnowEnvironment(const EnvironmentMap& environment,
                                      const snow::ChunkColumns& columns,
                                      uint8_t block) {
    std::unordered_map<const tile_geometry::CellGeometry*, bool> opaque;
    for (const auto& [cx, cz] : columns) {
        SnowTops tops;
        tops.fill(sky::None);
        for (int cy = Y_CHUNKS - 1; cy >= 0; --cy) {
            const auto slab = environment.find({cx, unsigned(cy), cz});
            if (slab == environment.end()) continue;
            for (const auto& [cell, geometry] : *slab->second) {
                const int x = cell % Chunk::SIZE_X;
                const int y =
                    cy * Chunk::SIZE_Y + cell / Chunk::SIZE_X % Chunk::SIZE_Y;
                const int z = cell / (Chunk::SIZE_X * Chunk::SIZE_Y);
                auto& top = tops[sky::Tile::index(x, z)];
                if (top >= y) continue;
                auto [it, inserted] = opaque.try_emplace(geometry.get());
                if (inserted)
                    it->second =
                        voxel_light::overheadLoss(
                            *geometry, (*opacity)[block]) == sky::Opaque;
                if (it->second) top = int16_t(y);
            }
        }
        const auto key = columnKey(cx, cz);
        const auto prior = snowTops.find(key);
        if (prior != snowTops.end() && prior->second == tops) continue;
        if (std::ranges::all_of(tops, [](int y) { return y == sky::None; })) {
            if (snowTops.erase(key) == 0) continue;
        } else {
            snowTops.insert_or_assign(key, tops);
        }
        if (const auto entry = entries.find(key); entry != entries.end())
            applySnow(key, entry->second);
        ++revision;
        changed.emit(key);
    }
}

void SkyHeightMap::setSnowSurface(
    std::shared_ptr<const snow::polar::SurfaceSnapshot> value,
    const snow::ChunkColumns& columns) {
    const bool newBaseline = !snowSurface ||
                             snowSurface->initialPacks != value->initialPacks ||
                             snowSurface->fixture != value->fixture;
    const auto heightTile = [](int32_t height) {
        return (height + snow::UnitsPerCubicMetre - 1) /
               snow::UnitsPerCubicMetre;
    };
    const auto differs = [&](const auto& nextMap, const auto& priorMap) {
        return std::ranges::any_of(columns, [&](const auto& key) {
            const auto next = nextMap.find(key);
            const auto prior = priorMap.find(key);
            if (next == nextMap.end() || prior == priorMap.end()) return true;
            return !std::ranges::equal(*next->second, *prior->second,
                                       [&](const auto& a, const auto& b) {
                                           return a.first == b.first &&
                                                  heightTile(a.second) ==
                                                      heightTile(b.second);
                                       });
        });
    };
    const bool editsChanged =
        snowSurface && differs(value->edits, snowSurface->edits);
    const bool looseChanged =
        snowSurface && differs(value->loose, snowSurface->loose);
    if (newBaseline || looseChanged) occlusionTerrain.reset();
    snowSurface = std::move(value);
    if (newBaseline || editsChanged || looseChanged) ++revision;
}

std::optional<int> SkyHeightMap::rawTop(int x, int z) const {
    x = int(chunks.wrap().wrapTile(x));
    if (x < 0 || z < 0) return std::nullopt;
    const auto entry =
        entries.find(columnKey(x / Chunk::SIZE_X, z / Chunk::SIZE_Z));
    if (entry == entries.end() || !entry->second.effective) return std::nullopt;
    return entry->second.effective->top(
        sky::Tile::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z));
}

int SkyHeightMap::snowOpaqueTop(int x, int z) const {
    x = int(chunks.wrap().wrapTile(x));
    if (x < 0 || z < 0) return sky::None;
    const auto found =
        snowTops.find(columnKey(x / Chunk::SIZE_X, z / Chunk::SIZE_Z));
    if (found == snowTops.end()) return sky::None;
    return found
        ->second[sky::Tile::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z)];
}

sky::TilePtr SkyHeightMap::tile(unsigned chunkX, unsigned chunkZ) const {
    const auto it = entries.find(columnKey(chunkX, chunkZ));
    if (it == entries.end()) return nullptr;
    return it->second.withSnow ? it->second.withSnow : it->second.effective;
}

std::optional<sky::Column> SkyHeightMap::column(int64_t x, int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0) return std::nullopt;
    const auto t =
        tile(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (!t) return std::nullopt;
    return t->column(
        sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)));
}

std::optional<int> SkyHeightMap::top(int64_t x, int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0) return std::nullopt;
    const auto t =
        tile(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (!t) return std::nullopt;
    return t->top(
        sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)));
}

std::optional<uint8_t> SkyHeightMap::directSky(int64_t x, int y,
                                               int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0) return std::nullopt;
    const auto t =
        tile(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (!t) return std::nullopt;
    return t->direct(
        sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)), y);
}

std::optional<uint8_t> SkyHeightMap::skyAt(int64_t x, int y, int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0 || x > INT_MAX || z > INT_MAX) return std::nullopt;
    if (y < 0) return 0;
    if (y >= WORLD_HEIGHT_Y) return sky::Opaque;
    const ChunkCoord coord{unsigned(x / Chunk::SIZE_X),
                           unsigned(y / Chunk::SIZE_Y),
                           unsigned(z / Chunk::SIZE_Z)};
    if (voxelLight && !voxelLight->isEstimated(coord))
        if (const auto* voxel = chunks.tileAt(int(x), y, int(z)))
            return voxel->lightLevel.sun_exposure;

    const auto direct = directSky(x, y, z);
    if (!direct || *direct == sky::Opaque) return direct;
    // Find nearby direct sources at this height, tracing the same face
    // losses as a voxel spread in reverse. Below the recorded opaque top
    // the heightmap has no cavity geometry, so assume space for this estimate.
    constexpr int Range = sky::Opaque - 1;
    constexpr int Side = 2 * Range + 1;
    struct Sample {
        voxel_light::Cell cell;
        uint8_t direct = voxel_light::Unknown;
        uint8_t cost = sky::Opaque;
    };
    std::array<Sample, Side * Side> samples;
    const auto index = [](int dx, int dz) {
        return (dz + Range) * Side + dx + Range;
    };
    const auto sample = [&](int dx, int dz) -> Sample& {
        auto& out = samples[index(dx, dz)];
        if (out.direct == voxel_light::Unknown) {
            const auto col = column(x + dx, z + dz);
            out.direct = col ? col->direct(y) : 0;
            if (col && y >= col->opaque) out.cell = col->cellAt(y);
        }
        return out;
    };
    std::array<std::vector<int>, sky::Opaque> queue;
    queue[0].push_back(index(0, 0));
    sample(0, 0).cost = 0;
    int best = *direct;
    for (int cost = 0; cost + best < sky::Opaque; ++cost) {
        for (const int at : queue[cost]) {
            const auto& target = samples[at];
            if (target.cost != cost) continue;
            best = std::max(best, int(target.direct) - cost);
            if (cost + 1 + best >= sky::Opaque) continue;
            const int dx = at % Side - Range, dz = at / Side - Range;
            for (const int face : {0, 1, 4, 5}) {
                const auto& step = voxel_light::Steps[face];
                auto& source = sample(dx + step[0], dz + step[2]);
                if (source.cost <= cost + 1) continue;
                const int next =
                    cost +
                    std::max(1, int(voxel_light::spreadLoss(
                                    source.cell, target.cell, Face(face ^ 1))));
                if (next + best >= sky::Opaque || source.cost <= next) continue;
                source.cost = uint8_t(next);
                queue[next].push_back(index(dx + step[0], dz + step[2]));
            }
        }
    }
    return uint8_t(best);
}

std::shared_ptr<const SkyHeightMap::TerrainRaster> SkyHeightMap::sampleTerrain(
    int x0, int z0, const World& world, const IAssets& assets, WorldWrap wrap,
    std::shared_ptr<const TerrainRaster> terrain,
    const snow::polar::SurfaceSnapshot* snow) {
    constexpr int Size = SkyOcclusionMap::Size;
    constexpr int Extent = SkyOcclusionMap::Extent;
    constexpr int Cell = SkyOcclusionMap::TilesPerTexel;
    if (terrain && (terrain->initialPacks != bool(snow && snow->initialPacks) ||
                    terrain->fixture != (snow ? snow->fixture : std::nullopt)))
        terrain.reset();
    if (terrain && terrain->x0 == x0 && terrain->z0 == z0) return terrain;
    auto raster = std::make_shared<TerrainRaster>(TerrainRaster{
        x0, z0, bool(snow && snow->initialPacks),
        snow ? snow->fixture : std::nullopt,
        std::vector<float>(size_t(Size) * Size, SkyOcclusionMap::Open),
        std::vector<float>(size_t(Size) * Size, SkyOcclusionMap::Open)});
    // Sample generated terrain at texel centres. Reuse
    // the overlap when the camera moves to the next snap.
    std::optional<SurfaceSampler> sampler;
    if (!world.regions.empty()) {
        sampler.emplace(world, assets);
        if (snow) {
            auto baseline = std::make_shared<snow::polar::SurfaceSnapshot>();
            baseline->block = snow->block;
            baseline->initialPacks = snow->initialPacks;
            baseline->fixture = snow->fixture;
            baseline->day = snow::polar::PermanentOnly;
            sampler->setSnow(std::move(baseline));
        }
    }
    if (!world.regions.empty())
        for (int tz = 0; tz < Size; ++tz) {
            const int z = z0 + tz * Cell + Cell / 2;
            if (z < 0 || z >= world.height * TerrainTuning::RegionZ) continue;
            for (int tx = 0; tx < Size; ++tx) {
                const int x = x0 + tx * Cell + Cell / 2;
                const int wx = int(wrap.wrapTile(x));
                const int oldX =
                    terrain ? int(WorldWrap::mod(int64_t(x) - terrain->x0,
                                                 wrap.tiles))
                            : -1;
                const int oldZ = terrain ? z - terrain->z0 : -1;
                const auto i = size_t(tz) * Size + tx;
                if (oldX >= 0 && oldX < Extent && oldZ >= 0 && oldZ < Extent) {
                    const auto prior = size_t(oldZ / Cell) * Size + oldX / Cell;
                    raster->heights[i] = terrain->heights[prior];
                    raster->raw[i] = terrain->raw[prior];
                } else {
                    raster->raw[i] = float(sampler->height(wx, z) + 1);
                    raster->heights[i] =
                        std::ceil(sampler->exteriorHeight(wx, z));
                }
            }
        }
    return raster;
}

void SkyHeightMap::updateOcclusion(
    Vec3 eye, std::vector<structure_voxels::SourcePtr> structures,
    const World& world, const IAssets& assets) {
    using namespace std::chrono_literals;
    if (occlusionJob) {
        if (occlusionJob->wait_for(0s) != std::future_status::ready) return;
        try {
            auto result = occlusionJob->get();
            occlusion = std::move(result.map);
            projections = std::move(result.projections);
            occlusionTerrain = std::move(result.terrain);
        } catch (const std::exception& error) {
            std::cerr << "Sky occlusion failed: " << error.what() << '\n';
            occlusionSources.clear();
            occlusionRevision = 0;
        }
        occlusionJob.reset();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < nextOcclusionBuild) return;
    std::sort(structures.begin(), structures.end(),
              [](const auto& a, const auto& b) { return a->key < b->key; });
    const bool sameSources = std::equal(
        structures.begin(), structures.end(), occlusionSources.begin(),
        occlusionSources.end(), [](const auto& a, const auto& b) {
            return a->key == b->key && a->revision == b->revision;
        });
    if (occlusion && !occlusion->recentres(eye.x, eye.z) && sameSources &&
        occlusionRevision == revision &&
        occlusionEditRevision == edits.revision())
        return;

    std::unordered_map<uint64_t, sky::TilePtr> tiles;
    for (const auto& [key, entry] : entries)
        if (auto effective = tile(unsigned(key >> 32), unsigned(key)))
            tiles.emplace(key, std::move(effective));
    occlusionSources = structures;
    occlusionRevision = revision;
    occlusionEditRevision = edits.revision();
    nextOcclusionBuild = now + 250ms;
    OcclusionInputs inputs{eye,
                           std::move(structures),
                           projections,
                           std::move(tiles),
                           edits.snapshot(),
                           overridden,
                           opacity,
                           chunks.wrap(),
                           occlusionTerrain,
                           snowSurface};
    occlusionJob = pool.submit_task(
        [inputs = std::move(inputs), &world, &assets]() mutable {
            return buildOcclusion(std::move(inputs), world, assets);
        },
        BS::pr::low);
}

SkyHeightMap::OcclusionResult SkyHeightMap::buildOcclusion(
    OcclusionInputs inputs, const World& world, const IAssets& assets) {
    auto& [eye, structures, previous, tiles, edits, overrides, opacity, wrap,
           terrain, snow] = inputs;
    Projections next;
    for (const auto& source : structures) {
        const auto same = std::find_if(
            previous.begin(), previous.end(), [&](const auto& cached) {
                return cached->source->key == source->key &&
                       cached->source->revision == source->revision;
            });
        if (same != previous.end()) {
            next.push_back(*same);
            continue;
        }
        auto projection = std::make_shared<Projection>();
        projection->source = source;
        projection->box = source->bounds;
        projection->box.y0 = std::max(0, projection->box.y0);
        projection->box.y1 = std::min(WORLD_HEIGHT_Y, projection->box.y1);
        if (projection->box.empty()) continue;
        source->skyTops(projection->box, projection->tops);
        keepOpaqueStructureTops(*source, projection->box, *opacity,
                                projection->tops);
        next.push_back(std::move(projection));
    }
    auto map = std::make_shared<SkyOcclusionMap>();
    map->build(eye.x, eye.z, [&](int x0, int z0, std::vector<float>& heights) {
        constexpr int Size = SkyOcclusionMap::Size;
        constexpr int Extent = SkyOcclusionMap::Extent;
        constexpr int Cell = SkyOcclusionMap::TilesPerTexel;
        terrain = sampleTerrain(x0, z0, world, assets, wrap, std::move(terrain),
                                snow.get());
        heights = terrain->heights;
        if (snow)
            for (const auto& [key, tops] : snow->edits) {
                const int x = int(key.first) * Chunk::SIZE_X;
                const int z = int(key.second) * Chunk::SIZE_Z;
                const int dx = int(WorldWrap::mod(int64_t(x) - x0, wrap.tiles));
                const int dz = z - z0;
                if (dx < 0 || dx >= Extent || dz < 0 || dz >= Extent) continue;
                for (const auto& [point, top] : *tops) {
                    const int lx = dx + point.first - x;
                    const int lz = dz + point.second - z;
                    if (lx % Cell != Cell / 2 || lz % Cell != Cell / 2)
                        continue;
                    const auto i = size_t(lz / Cell) * Size + lx / Cell;
                    heights[i] =
                        std::max(terrain->raw[i],
                                 std::ceil(float(double(top) /
                                                 snow::UnitsPerCubicMetre)));
                }
            }

        const auto add = [&](int x, int z, int top) {
            if (top < 0 || z < z0 || z >= z0 + Extent) return;
            int offset = int(WorldWrap::mod(int64_t(x) - x0, wrap.tiles));
            for (; offset >= 0 && offset < Extent;
                 offset += wrap.enabled() ? wrap.tiles : Extent) {
                auto& height =
                    heights[size_t((z - z0) / Cell) * Size + offset / Cell];
                height = std::max(height, float(top + 1));
            }
        };
        const auto inView = [&](uint64_t key) {
            const int x = int(unsigned(key >> 32)) * Chunk::SIZE_X;
            const int z = int(unsigned(key)) * Chunk::SIZE_Z;
            return z < z0 + Extent && z + Chunk::SIZE_Z > z0 &&
                   WorldWrap::mod(int64_t(x) - x0, wrap.tiles) < Extent &&
                   (wrap.enabled() || x + Chunk::SIZE_X > x0);
        };
        const auto addTile = [&](uint64_t key, const sky::Tile& tile) {
            const int x = int(unsigned(key >> 32)) * Chunk::SIZE_X;
            const int z = int(unsigned(key)) * Chunk::SIZE_Z;
            // The aligned chunk tile replaces the terrain
            // samples, including lowered ground and open texels.
            const int columnX =
                int(WorldWrap::mod(int64_t(x) - x0, wrap.tiles));
            for (int offset = columnX; offset >= 0 && offset < Extent;
                 offset += wrap.enabled() ? wrap.tiles : Extent)
                for (int lz = 0; lz < Chunk::SIZE_Z; lz += Cell)
                    for (int lx = 0; lx < Chunk::SIZE_X; lx += Cell)
                        heights[size_t((z + lz - z0) / Cell) * Size +
                                (offset + lx) / Cell] = SkyOcclusionMap::Open;
            for (int i = 0; i < sky::Tile::Size; ++i)
                add(x + i % Chunk::SIZE_X, z + i / Chunk::SIZE_X,
                    tile.opaqueTop(i));
        };
        for (const auto& [key, tile] : tiles)
            if (inView(key)) addTile(key, *tile);
        for (const auto& [key, edit] : *edits) {
            if (tiles.contains(key) || !inView(key)) continue;
            sky::TileInputs inputs;
            inputs.edit = edit;
            inputs.opacity = opacity.get();
            inputs.structureTop.fill(INT_MIN);
            auto tile = sky::build(inputs);
            if (const auto it = overrides.find(key); it != overrides.end())
                for (const auto& [i, column] : it->second) tile.set(i, column);
            addTile(key, tile);
            if (snow) {
                const int x = int(unsigned(key >> 32)) * Chunk::SIZE_X;
                const int z = int(unsigned(key)) * Chunk::SIZE_Z;
                for (int dz = Cell / 2; dz < Chunk::SIZE_Z; dz += Cell)
                    for (int dx = Cell / 2; dx < Chunk::SIZE_X; dx += Cell) {
                        const int lx = int(
                            WorldWrap::mod(int64_t(x + dx) - x0, wrap.tiles));
                        const int lz = z + dz - z0;
                        if (lx < 0 || lx >= Extent || lz < 0 || lz >= Extent)
                            continue;
                        const auto i = size_t(lz / Cell) * Size + lx / Cell;
                        const auto edited = snow->editedTop(x + dx, z + dz);
                        const float top =
                            edited ? float(double(*edited) /
                                           snow::UnitsPerCubicMetre)
                            : terrain->heights[i] > terrain->raw[i]
                                ? terrain->heights[i]
                                : 0.f;
                        if (top > 0)
                            add(x + dx, z + dz, int(std::ceil(top)) - 1);
                    }
            }
        }
        for (const auto& [key, columns] : overrides) {
            if (tiles.contains(key) || edits->contains(key) || !inView(key))
                continue;
            for (const auto& [i, column] : columns)
                add(int(unsigned(key >> 32)) * Chunk::SIZE_X +
                        i % Chunk::SIZE_X,
                    int(unsigned(key)) * Chunk::SIZE_Z + i / Chunk::SIZE_X,
                    column.opaque);
        }
        for (const auto& projection : next) {
            const auto& box = projection->box;
            const int width = box.x1 - box.x0;
            for (int z = std::max(box.z0, z0);
                 z < std::min(box.z1, z0 + Extent); ++z) {
                for (int x = box.x0; x < box.x1; ++x) {
                    const int top =
                        projection
                            ->tops[size_t(z - box.z0) * width + (x - box.x0)];
                    if (top < 0) continue;
                    const auto wx = wrap.wrapTile(x);
                    if (wx < 0 || z < 0) continue;
                    const auto key = columnKey(unsigned(wx / Chunk::SIZE_X),
                                               unsigned(z / Chunk::SIZE_Z));
                    if (tiles.contains(key)) continue;
                    const auto it = overrides.find(key);
                    if (it != overrides.end() &&
                        it->second.contains(sky::Tile::index(
                            int(wx % Chunk::SIZE_X), z % Chunk::SIZE_Z)))
                        continue;
                    add(x, z, top);
                }
            }
        }
    });
    return OcclusionResult{std::move(map), std::move(next), std::move(terrain)};
}

bool SkyHeightMap::isIdle() const {
    return queue.empty() && jobs.empty() && observations.empty();
}
