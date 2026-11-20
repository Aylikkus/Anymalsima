#include "sky_height_map.hpp"

#include <algorithm>
#include <climits>
#include <iostream>

#include "light_volume.hpp"
#include "sunlight_propagator.hpp"
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
    tile.opacity = in.opacity.get();
    for (size_t i = 0; i < sky::Tile::Size; ++i) {
        tile.surface[i] = in.terrain->columns[i].surface_y;
        tile.water[i] = in.terrain->columns[i].water;
    }
    tile.structureTop =
        in.edit ? in.structures->skyTops(*in.edit) : in.structures->skyTops();
    return sky::build(tile);
}
}  // namespace

SkyHeightMap::SkyHeightMap(ChunkManager& chunks, TerrainEdits& edits,
                           const DataStorage<Block>& blocks, ThreadPool& pool)
    : chunks(chunks), edits(edits), blocks(blocks), pool(pool) {
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
        if (tileOpacity(c.before, blocks) == tileOpacity(c.after, blocks))
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
    std::array<uint8_t, Chunk::SIZE_Y> slab{};
    return sky::scanColumn(from, 0, [&](int y) -> int {
        const int s = y / Chunk::SIZE_Y;
        if (s != cachedSlab) {
            cachedSlab = s;
            loaded = chunks.withChunkLocked(
                {cx, unsigned(s), cz}, [&](const Chunk& chunk) {
                    for (int ly = 0; ly < Chunk::SIZE_Y; ++ly)
                        slab[size_t(ly)] =
                            tileOpacity(chunk.tiles[lz][ly][lx], blocks);
                });
        }
        return loaded ? slab[size_t(y % Chunk::SIZE_Y)] : known.opacityAt(y);
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

sky::TilePtr SkyHeightMap::tile(unsigned chunkX, unsigned chunkZ) const {
    const auto it = entries.find(columnKey(chunkX, chunkZ));
    return it == entries.end() ? nullptr : it->second.effective;
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
    int best = *direct;
    const auto target = column(x, z);
    // Below the highest opaque tile the column has no cavity geometry.
    // Treat the queried point there as open for this lateral estimate.
    const int excess =
        y < target->opaque ? 0 : std::max(1, int(target->opacityAt(y))) - 1;
    // Heightmap approximation: horizontal 4-neighbour distance from direct
    // light. The column profile supplies attenuation, even under glass.
    for (int d = 1; d + excess < sky::Opaque - best; ++d)
        for (int dx = -d; dx <= d; ++dx) {
            const int dz = d - std::abs(dx);
            if (const auto light = directSky(x + dx, y, z + dz))
                best = std::max(best, int(*light) - d - excess);
            if (dz != 0)
                if (const auto light = directSky(x + dx, y, z - dz))
                    best = std::max(best, int(*light) - d - excess);
        }
    return uint8_t(best);
}

std::shared_ptr<const SkyHeightMap::TerrainRaster> SkyHeightMap::sampleTerrain(
    int x0, int z0, const World& world, const IAssets& assets, WorldWrap wrap,
    std::shared_ptr<const TerrainRaster> terrain) {
    constexpr int Size = SkyOcclusionMap::Size;
    constexpr int Extent = SkyOcclusionMap::Extent;
    constexpr int Cell = SkyOcclusionMap::TilesPerTexel;
    if (terrain && terrain->x0 == x0 && terrain->z0 == z0) return terrain;
    auto raster = std::make_shared<TerrainRaster>(TerrainRaster{
        x0, z0,
        std::vector<float>(size_t(Size) * Size, SkyOcclusionMap::Open)});
    // Sample generated terrain at texel centres. Reuse
    // the overlap when the camera moves to the next snap.
    std::unordered_map<uint64_t, TerrainNeighborhood> regions;
    const auto surface = [&](int x, int z) {
        const Vec2u region{unsigned(x / TerrainTuning::RegionX),
                           unsigned(z / TerrainTuning::RegionZ)};
        const uint64_t key = columnKey(region.x, region.y);
        auto it = regions.find(key);
        if (it == regions.end())
            it = regions
                     .emplace(key, TerrainNeighborhood::capture(world, region,
                                                                assets))
                     .first;
        return float(std::floor(TerrainSampler(it->second).surface(x, z)) + 1);
    };
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
                raster->heights[size_t(tz) * Size + tx] =
                    oldX >= 0 && oldX < Extent && oldZ >= 0 && oldZ < Extent
                        ? terrain->heights[size_t(oldZ / Cell) * Size +
                                           oldX / Cell]
                        : surface(wx, z);
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
        if (entry.effective) tiles.emplace(key, entry.effective);
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
                           occlusionTerrain};
    occlusionJob = pool.submit_task(
        [inputs = std::move(inputs), &world, &assets]() mutable {
            return buildOcclusion(std::move(inputs), world, assets);
        },
        BS::pr::low);
}

SkyHeightMap::OcclusionResult SkyHeightMap::buildOcclusion(
    OcclusionInputs inputs, const World& world, const IAssets& assets) {
    auto& [eye, structures, previous, tiles, edits, overrides, opacity, wrap,
           terrain] = inputs;
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
        next.push_back(std::move(projection));
    }
    auto map = std::make_shared<SkyOcclusionMap>();
    map->build(eye.x, eye.z, [&](int x0, int z0, std::vector<float>& heights) {
        constexpr int Size = SkyOcclusionMap::Size;
        constexpr int Extent = SkyOcclusionMap::Extent;
        constexpr int Cell = SkyOcclusionMap::TilesPerTexel;
        terrain =
            sampleTerrain(x0, z0, world, assets, wrap, std::move(terrain));
        heights = terrain->heights;

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
