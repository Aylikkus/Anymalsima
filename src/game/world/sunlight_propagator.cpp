#include "sunlight_propagator.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "light_volume.hpp"
#include "render/ui_context.hpp"
#include "sky_height_map.hpp"

SunLightPropagator::SunLightPropagator(ChunkManager& chunks,
                                       const DataStorage<Block>& blocks,
                                       ThreadPool& pool, SkyHeightMap* sky)
    : chunks(chunks), blocks(blocks), pool(pool), sky(sky) {
    // A changed heightmap (edits, tree growth, overrides) sweeps the
    // column again; its spreads rerun only where direct light changed.
    if (sky)
        skyConnection = sky->changed.connect([this](uint64_t column) {
            if (!columnChunks.contains(column) || columnChunks[column].empty())
                return;
            columnRevisions[column] = ++nextRevision;
            pendingDirect.insert(column);
        });
    readyConnection = chunks.becameReady.connect(
        [this](const ChunkCoord& coord) { ready(coord); });
    removedConnection = chunks.removed.connect(
        [this](const ChunkCoord& coord) { removed(coord); });
    chunks.forEachReady(
        [this](const ChunkCoord& coord, Chunk&) { ready(coord); });
}

SunLightPropagator::~SunLightPropagator() {
    chunks.becameReady.disconnect(readyConnection);
    chunks.removed.disconnect(removedConnection);
    if (sky) sky->changed.disconnect(skyConnection);
    for (auto& job : directJobs) job.future.wait();
    for (auto& job : indirectJobs) job.future.wait();
}

bool SunLightPropagator::offset(ChunkCoord coord, int dx, int dy, int dz,
                                ChunkCoord& out) const {
    const int64_t x = chunks.wrap().wrapChunk(int64_t{coord.x} + dx);
    const int64_t y = int64_t{coord.y} + dy;
    const int64_t z = int64_t{coord.z} + dz;
    if (x < 0 || z < 0 || y < 0 || y >= Y_CHUNKS ||
        x > std::numeric_limits<unsigned>::max() ||
        z > std::numeric_limits<unsigned>::max())
        return false;
    out = {static_cast<unsigned>(x), static_cast<unsigned>(y),
           static_cast<unsigned>(z)};
    return true;
}

uint8_t SunLightPropagator::opacity(const Tile& tile) const {
    return tileOpacity(tile, blocks);
}

void SunLightPropagator::invalidateAround(const ChunkCoord& coord) {
    // A positive indirect contribution travels at most 14 tiles, so only
    // this chunk and its 26 immediate neighbors can depend on its sources.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord neighbor;
                if (!offset(coord, dx, dy, dz, neighbor)) continue;
                auto it = states.find(neighbor);
                if (it == states.end()) continue;
                it->second.revision = ++nextRevision;
                it->second.complete = false;
                pendingIndirect.insert(neighbor);
            }
}

void SunLightPropagator::ready(const ChunkCoord& coord) {
    states[coord] = State{};
    const auto column = columnKey(coord.x, coord.z);
    columnChunks[column].insert(coord);
    columnRevisions[column] = ++nextRevision;
    pendingDirect.insert(column);
    invalidateAround(coord);
}

void SunLightPropagator::removed(const ChunkCoord& coord) {
    states.erase(coord);
    const auto column = columnKey(coord.x, coord.z);
    columnChunks[column].erase(coord);
    requested.erase(coord);
    pendingIndirect.erase(coord);
    invalidateAround(coord);
    // Results of sweeps in flight for this column are stale now; the rest
    // of the column is recomputed on the next update. Unloading a region
    // removes whole columns slab by slab: marking each time walked every
    // remaining slab and its neighbours (quadratic, tens of milliseconds
    // per region border), and an emptied column needs nothing at all.
    columnRevisions[column] = ++nextRevision;
    removedColumns.insert(column);
}

void SunLightPropagator::markColumnDirty(unsigned cx, unsigned cz) {
    const auto key = columnKey(cx, cz);
    columnRevisions[key] = ++nextRevision;
    pendingDirect.insert(key);
    const auto& members = columnChunks[key];
    if (members.empty()) {
        pendingDirect.erase(key);
        return;
    }
    for (const auto& coord : members) {
        auto& state = states.at(coord);
        state.direct.fill(0);
        state.directKnown = false;
        // No remesh: light reaches the GPU through light volumes, refreshed
        // when the recomputed light is published.
        invalidateAround(coord);
    }
}

bool SunLightPropagator::isIdle() const {
    return removedColumns.empty() && pendingDirect.empty() &&
           pendingIndirect.empty() && requested.empty() && directJobs.empty() &&
           indirectJobs.empty();
}

std::optional<SunLightPropagator::DirectInput>
SunLightPropagator::snapshotColumn(uint64_t column) {
    const auto cx = static_cast<unsigned>(column >> 32);
    const auto cz = static_cast<unsigned>(column);
    sky::TilePtr map;
    if (sky) {
        map = sky->tile(cx, cz);
        if (!map) return std::nullopt;
    }
    int top = std::max(SEA_LEVEL_Y, chunks.getColumnTopY(cx, cz));
    DirectInput input{column, columnRevisions.at(column), 0, {}, map};
    for (const auto& coord : columnChunks.at(column)) {
        Slab slab{coord, {}};
        chunks.withChunkLocked(coord, [&](const Chunk& chunk) {
            for (int z = 0; z < Z; ++z)
                for (int y = 0; y < Y; ++y)
                    for (int x = 0; x < X; ++x) {
                        const auto& tile = chunk.tiles[z][y][x];
                        slab.opacity[index(x, y, z)] = opacity(tile);
                        if (!tile.isAir())
                            top = std::max(top,
                                           static_cast<int>(coord.y) * Y + y);
                    }
        });
        input.slabs.push_back(std::move(slab));
    }
    input.topSlab = std::clamp(top / Y, 0, Y_CHUNKS - 1);
    std::sort(
        input.slabs.begin(), input.slabs.end(),
        [](const Slab& a, const Slab& b) { return a.coord.y > b.coord.y; });
    return input;
}

SunLightPropagator::DirectResult SunLightPropagator::sweep(DirectInput input) {
    DirectResult result{input.column, input.revision, {}, {}};
    if (input.slabs.empty()) return result;
    if (input.sky) {
        // Each run of loaded slabs starts from the heightmap's direct light
        // at its top tile and continues down through its own tiles; nothing
        // is missing, nothing is requested.
        std::array<uint8_t, X * Z> sun{};
        int above = -1;  // slab Y of the previous (higher) loaded slab
        for (const auto& slab : input.slabs) {
            const int cy = static_cast<int>(slab.coord.y);
            if (cy != above - 1)
                for (int i = 0; i < X * Z; ++i)
                    sun[size_t(i)] = input.sky->direct(i, cy * Y + Y - 1);
            above = cy;
            ChunkLight output{slab.coord, {}, false};
            for (int y = Y - 1; y >= 0; --y)
                for (int z = 0; z < Z; ++z)
                    for (int x = 0; x < X; ++x) {
                        auto& incoming = sun[size_t(z * X + x)];
                        const int i = index(x, y, z);
                        output.light[i] = incoming;
                        incoming = static_cast<uint8_t>(
                            std::max(0, int(incoming) - slab.opacity[i]));
                    }
            result.chunks.push_back(std::move(output));
        }
        return result;
    }
    std::array<uint8_t, X * Z> sun;
    sun.fill(15);
    size_t next = 0;
    bool blind = false;
    const int lowest = static_cast<int>(input.slabs.back().coord.y);
    const int highest =
        std::max(input.topSlab, static_cast<int>(input.slabs.front().coord.y));
    for (int cy = highest; cy >= lowest; --cy) {
        if (next == input.slabs.size() ||
            input.slabs[next].coord.y != static_cast<unsigned>(cy)) {
            if (cy <= input.topSlab &&
                std::any_of(sun.begin(), sun.end(),
                            [](auto v) { return v > 0; })) {
                result.missing.push_back(
                    {static_cast<unsigned>(input.column >> 32),
                     static_cast<unsigned>(cy),
                     static_cast<unsigned>(input.column)});
                // Never invent sky through an unavailable slab. Its arrival
                // invalidates this column and resumes the sweep from the top.
                // The chunks below are blind meanwhile: collect() keeps their
                // estimate instead of publishing this zero.
                sun.fill(0);
                blind = true;
            }
            continue;
        }
        const auto& slab = input.slabs[next++];
        ChunkLight output{slab.coord, {}, blind};
        for (int y = Y - 1; y >= 0; --y)
            for (int z = 0; z < Z; ++z)
                for (int x = 0; x < X; ++x) {
                    auto& incoming = sun[z * X + x];
                    const int i = index(x, y, z);
                    output.light[i] = incoming;
                    incoming = static_cast<uint8_t>(
                        std::max(0, int(incoming) - slab.opacity[i]));
                }
        result.chunks.push_back(std::move(output));
    }
    return result;
}

SunLightPropagator::IndirectInput SunLightPropagator::snapshotHalo(
    const ChunkCoord& coord) {
    constexpr int count = HX * HY * HZ;
    IndirectInput input{coord, states.at(coord).revision,
                        std::vector<uint8_t>(count, 255),
                        std::vector<uint8_t>(count, 0)};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord neighbor;
                if (!offset(coord, dx, dy, dz, neighbor)) continue;
                const auto it = states.find(neighbor);
                if (it == states.end()) continue;
                chunks.withChunkLocked(neighbor, [&](const Chunk& chunk) {
                    for (int z = 0; z < Z; ++z)
                        for (int y = 0; y < Y; ++y)
                            for (int x = 0; x < X; ++x) {
                                const int hx = HALO + dx * X + x;
                                const int hy = HALO + dy * Y + y;
                                const int hz = HALO + dz * Z + z;
                                if (hx < 0 || hx >= HX || hy < 0 || hy >= HY ||
                                    hz < 0 || hz >= HZ)
                                    continue;
                                const auto i = haloIndex(hx, hy, hz);
                                input.opacity[i] =
                                    opacity(chunk.tiles[z][y][x]);
                                input.light[i] =
                                    it->second.direct[index(x, y, z)];
                            }
                });
            }
    return input;
}

SunLightPropagator::IndirectResult SunLightPropagator::spread(
    IndirectInput input) {
    // Descending buckets process the strongest paths first. Every edge loses
    // at least one level, so no source outside the halo can affect the target.
    std::array<std::vector<int>, 16> buckets;
    for (int i = 0; i < HX * HY * HZ; ++i)
        if (input.light[i] > std::max<uint8_t>(1, input.opacity[i]))
            buckets[input.light[i]].push_back(i);
    for (int level = 15; level > 0; --level) {
        for (const int i : buckets[level]) {
            if (input.light[i] != level) continue;
            const int outgoing = level - std::max(1, int(input.opacity[i]));
            if (outgoing <= 0) continue;
            const int x = i % HX, y = (i / HX) % HY, z = i / (HX * HY);
            const int neighbors[6] = {
                x > 0 ? i - 1 : -1,       x + 1 < HX ? i + 1 : -1,
                y > 0 ? i - HX : -1,      y + 1 < HY ? i + HX : -1,
                z > 0 ? i - HX * HY : -1, z + 1 < HZ ? i + HX * HY : -1};
            for (const int n : neighbors) {
                if (n < 0 || input.opacity[n] == 255 ||
                    input.light[n] >= outgoing)
                    continue;
                input.light[n] = static_cast<uint8_t>(outgoing);
                buckets[outgoing].push_back(n);
            }
        }
    }
    IndirectResult result{input.coord, input.revision, {}};
    for (int z = 0; z < Z; ++z)
        for (int y = 0; y < Y; ++y)
            for (int x = 0; x < X; ++x)
                result.light[index(x, y, z)] =
                    input.light[haloIndex(x + HALO, y + HALO, z + HALO)];
    return result;
}

void SunLightPropagator::collect() {
    using namespace std::chrono_literals;
    for (size_t i = 0; i < directJobs.size();) {
        auto& job = directJobs[i];
        if (job.future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        const auto column = job.column;
        auto future = std::move(job.future);
        directJobs.erase(directJobs.begin() + i);
        DirectResult result;
        try {
            result = future.get();
        } catch (...) {
            pendingDirect.insert(column);
            throw;
        }
        if (columnRevisions.at(column) != result.revision) continue;
        for (auto& output : result.chunks) {
            auto it = states.find(output.coord);
            if (it == states.end()) continue;
            if (output.blind) {
                // Sky slabs above are still generating (tall structures raise
                // column tops by hundreds of tiles). Publishing the zero lower
                // bound would turn whole chunks black until they all arrive;
                // the tiles keep their estimate and neighbours spread from it.
                Light shown{};
                chunks.withChunkLocked(output.coord, [&](const Chunk& chunk) {
                    for (int z = 0; z < Z; ++z)
                        for (int y = 0; y < Y; ++y)
                            for (int x = 0; x < X; ++x)
                                shown[index(x, y, z)] =
                                    chunk.tiles[z][y][x]
                                        .lightLevel.sun_exposure;
                });
                const bool changed = !it->second.directKnown ||
                                     !it->second.blind ||
                                     it->second.direct != shown;
                it->second.direct = shown;
                it->second.directKnown = true;
                it->second.blind = true;
                if (!it->second.lit) it->second.estimated = true;
                if (changed) invalidateAround(output.coord);
                continue;
            }
            // A blind chunk dropped its spread job: requeue it even when the
            // sweep matches what it showed.
            const bool wasBlind = std::exchange(it->second.blind, false);
            const bool changed = wasBlind || !it->second.directKnown ||
                                 it->second.direct != output.light;
            it->second.direct = std::move(output.light);
            it->second.directKnown = true;
            if (changed) {
                invalidateAround(output.coord);
                // A chunk without any light yet gets a correct lower bound
                // immediately; indirect light refines it without waiting
                // for distant columns. A lit chunk keeps its previous full
                // light until the refined result replaces it, and so does a
                // chunk showing the estimate (blind sweeps before).
                if (!it->second.lit && !it->second.estimated && !sky)
                    publish(output.coord, it->second.direct);
            }
        }
        for (const auto& coord : result.missing)
            if (chunks.isRegistered(coord)) requested.insert(coord);
    }
    for (size_t i = 0; i < indirectJobs.size();) {
        auto& job = indirectJobs[i];
        if (job.future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        const auto coord = job.coord;
        auto future = std::move(job.future);
        indirectJobs.erase(indirectJobs.begin() + i);
        IndirectResult result;
        try {
            result = future.get();
        } catch (...) {
            pendingIndirect.insert(coord);
            throw;
        }
        const auto it = states.find(coord);
        if (it == states.end() || it->second.revision != result.revision)
            continue;
        publish(coord, result.light);
        it->second.complete = true;
        it->second.lit = true;
        it->second.estimated = false;
    }
}

bool SunLightPropagator::hasDirect(ChunkCoord coord) const {
    const auto it = states.find(coord);
    return it != states.end() && it->second.directKnown;
}

bool SunLightPropagator::isComplete(ChunkCoord coord) const {
    const auto it = states.find(coord);
    return it != states.end() && it->second.complete;
}

bool SunLightPropagator::isEstimated(ChunkCoord coord) const {
    const auto it = states.find(coord);
    // With a heightmap, the estimate stays until full light lands: the
    // direct lower bound has no bounce and is black under a wide limb.
    return it != states.end() && !it->second.lit &&
           (sky || it->second.estimated || !it->second.directKnown);
}

void SunLightPropagator::publish(ChunkCoord coord, const Light& values) {
    bool changed = false;
    chunks.withChunkLocked(coord, [&](Chunk& chunk) {
        for (int z = 0; z < Z; ++z)
            for (int y = 0; y < Y; ++y)
                for (int x = 0; x < X; ++x) {
                    auto& light = chunk.tiles[z][y][x].lightLevel;
                    const auto value = values[index(x, y, z)];
                    changed |= light.sun_exposure != value;
                    light.sun_exposure = value;
                }
    });
    // Light lives in GPU volumes sampled by the block shader, so a light
    // change refreshes those volumes without remeshing geometry.
    if (changed) chunks.markLightChanged(coord);
}

bool SunLightPropagator::hasQueuedNeighbor(const ChunkCoord& coord) const {
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord n;
                if ((dx || dy || dz) && offset(coord, dx, dy, dz, n) &&
                    chunks.isQueued(n))
                    return true;
            }
    return false;
}

bool SunLightPropagator::sweepPending(uint64_t column) const {
    return pendingDirect.contains(column) ||
           std::any_of(directJobs.begin(), directJobs.end(),
                       [&](const auto& job) { return job.column == column; });
}

bool SunLightPropagator::spreadIsIdentity(const ChunkCoord& coord) {
    const auto it = states.find(coord);
    if (it == states.end() || !it->second.directKnown) return false;
    if (sweepPending(columnKey(coord.x, coord.z))) return false;
    const auto& direct = it->second.direct;
    if (std::all_of(direct.begin(), direct.end(),
                    [](uint8_t v) { return v == 15; }))
        return true;
    // Opaque cells pass on max(0, level - 15) = 0, so with an opaque shell
    // no light other than the direct sweep reaches any cell.
    bool sealed = false;
    chunks.withChunkLocked(coord, [&](const Chunk& chunk) {
        const Tile* first = &chunk.tiles[0][0][0];
        sealed = std::all_of(first, first + VOLUME, [&](const Tile& tile) {
            return opacity(tile) >= 15;
        });
    });
    if (!sealed) return false;
    const int offsets[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const auto& o : offsets) {
        ChunkCoord n;
        // Outside the world: the halo is opaque padding (255).
        if (!offset(coord, o[0], o[1], o[2], n)) continue;
        if (!states.contains(n)) return false;
        bool covered = false;
        chunks.withChunkLocked(n, [&](const Chunk& chunk) {
            covered = true;
            for (int a = 0; a < 16 && covered; ++a)
                for (int b = 0; b < 16; ++b) {
                    const Tile& t = o[0] ? chunk.tiles[a][b][o[0] > 0 ? 0 : 15]
                                    : o[1]
                                        ? chunk.tiles[a][o[1] > 0 ? 0 : 15][b]
                                        : chunk.tiles[o[2] > 0 ? 0 : 15][a][b];
                    if (opacity(t) < 15) {
                        covered = false;
                        break;
                    }
                }
        });
        if (!covered) return false;
    }
    return true;
}

std::vector<uint64_t> SunLightPropagator::dependencies(ChunkCoord coord) const {
    std::vector<uint64_t> result;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord n;
                if (!offset(coord, dx, dy, dz, n) || !states.contains(n))
                    continue;
                const auto key = columnKey(n.x, n.z);
                if (std::find(result.begin(), result.end(), key) ==
                    result.end())
                    result.push_back(key);
            }
    return result;
}

ChunkCoord SunLightPropagator::beneficiary(ChunkCoord slab) const {
    // Sky prerequisites inherit a nearby dependent's height, rather than
    // becoming low-priority just because their slab is far above the camera.
    ChunkCoord best = slab;
    double distance = std::numeric_limits<double>::infinity();
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            ChunkCoord n;
            if (!offset(slab, dx, 0, dz, n)) continue;
            auto it = columnChunks.find(columnKey(n.x, n.z));
            if (it == columnChunks.end()) continue;
            for (const auto& coord : it->second) {
                const auto d = pendingIndirect.distance(coord, focus);
                if (d < distance ||
                    (d == distance && std::tie(coord.x, coord.y, coord.z) <
                                          std::tie(best.x, best.y, best.z))) {
                    best = coord;
                    distance = d;
                }
            }
        }
    return best;
}

void SunLightPropagator::dispatchSweeps(size_t capacity) {
    if (pendingDirect.empty()) return;
    // Nearest columns first (at the focus height); sweeps are cheap and
    // every spread waits for them.
    std::vector<std::pair<double, uint64_t>> order;
    order.reserve(pendingDirect.size());
    for (const auto column : pendingDirect) {
        if (std::any_of(directJobs.begin(), directJobs.end(),
                        [&](const auto& job) { return job.column == column; }))
            continue;
        const ChunkCoord anchor{static_cast<unsigned>(column >> 32),
                                static_cast<unsigned>(std::max(
                                    0.f, focus.y / float(Chunk::SIZE_Y))),
                                static_cast<unsigned>(column)};
        order.emplace_back(pendingIndirect.distance(anchor, focus), column);
    }
    std::sort(order.begin(), order.end());
    const auto started = std::chrono::steady_clock::now();
    for (const auto& [distance, column] : order) {
        if (directJobs.size() >= capacity ||
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(1))
            break;
        auto input = snapshotColumn(column);
        if (!input) continue;  // heightmap not built yet
        auto future = pool.submit_task(
            [input = std::move(*input)]() mutable {
                return sweep(std::move(input));
            },
            BS::pr::high);
        directJobs.push_back({column, std::move(future)});
        pendingDirect.erase(column);
    }
}

void SunLightPropagator::update() {
    pendingIndirect.setPeriodX(chunks.wrap().tiles);
    // Columns that lost chunks: recompute what remains of them (an emptied
    // column only drops its pending sweep).
    for (const auto column : removedColumns)
        markColumnDirty(unsigned(column >> 32), unsigned(column));
    removedColumns.clear();
    collect();
    const auto requests = requested;
    for (const auto& coord : requests) {
        if (!chunks.isRegistered(coord) ||
            chunks.requestLighting(coord, beneficiary(coord)))
            requested.erase(coord);
    }
    const size_t capacity = std::clamp<size_t>(pool.get_thread_count(), 1, 8);
    // At most half the slots: spreads wait for sweeps, but chunk arrivals
    // queue a sweep each, which would otherwise take every slot.
    if (sky) dispatchSweeps(std::max<size_t>(1, capacity / 2));
    const auto started = std::chrono::steady_clock::now();
    const auto normal = pendingIndirect.ordered(focus, false);
    std::vector<ChunkCoord> oldest;
    size_t submitted = 0;
    while (inFlightCount() < capacity) {
        if (submitted && std::chrono::steady_clock::now() - started >=
                             std::chrono::milliseconds(2))
            break;
        const bool background = (dispatches + 1) % 16 == 0;
        if (background && oldest.empty())
            oldest = pendingIndirect.ordered(focus, true);
        bool dispatched = false;
        for (const auto& coord : background ? oldest : normal) {
            if (!pendingIndirect.contains(coord) ||
                std::any_of(
                    indirectJobs.begin(), indirectJobs.end(),
                    [&](const auto& job) { return job.coord == coord; }))
                continue;
            // Blind and its column is not about to be swept again: nothing
            // to spread yet, the column's next sweep re-queues it. A pending
            // sweep is dispatched below, as this chunk's dependency.
            if (const auto state = states.find(coord);
                state != states.end() && state->second.blind &&
                !sweepPending(columnKey(coord.x, coord.z))) {
                pendingIndirect.erase(coord);
                continue;
            }
            if (spreadIsIdentity(coord)) {
                auto& state = states.at(coord);
                publish(coord, state.direct);
                state.complete = true;
                state.lit = true;
                state.estimated = false;
                pendingIndirect.erase(coord);
                continue;
            }
            bool waiting = false;
            std::optional<uint64_t> nextColumn;
            double nextDistance = std::numeric_limits<double>::infinity();
            for (auto column : dependencies(coord)) {
                const bool active = std::any_of(
                    directJobs.begin(), directJobs.end(),
                    [&](const auto& job) { return job.column == column; });
                waiting |= active || pendingDirect.contains(column);
                if (active || !pendingDirect.contains(column)) continue;
                ChunkCoord anchor{static_cast<unsigned>(column >> 32), coord.y,
                                  static_cast<unsigned>(column)};
                const auto distance = pendingIndirect.distance(anchor, focus);
                if (!nextColumn || distance < nextDistance ||
                    (distance == nextDistance && column < *nextColumn)) {
                    nextColumn = column;
                    nextDistance = distance;
                }
            }
            // A column whose heightmap is not built keeps this chunk
            // waiting (sky mode).
            std::optional<DirectInput> input;
            if (nextColumn) input = snapshotColumn(*nextColumn);
            if (input) {
                const auto column = *nextColumn;
                auto future = pool.submit_task(
                    [input = std::move(*input)]() mutable {
                        return sweep(std::move(input));
                    },
                    BS::pr::high);
                directJobs.push_back({column, std::move(future)});
                pendingDirect.erase(column);
                dispatched = true;
            } else if (!nextColumn && !waiting && !hasQueuedNeighbor(coord)) {
                // A neighbour still being generated would invalidate this
                // result on arrival; its direct lower bound shows meanwhile.
                auto input = snapshotHalo(coord);
                // Below meshing and direct sweeps: bounced light refines an
                // already lit chunk.
                auto future = pool.submit_task(
                    [input = std::move(input)]() mutable {
                        return spread(std::move(input));
                    },
                    BS::pr::normal);
                indirectJobs.push_back({coord, std::move(future)});
                pendingIndirect.erase(coord);
                dispatched = true;
            }
            if (dispatched) break;
        }
        if (!dispatched) break;
        ++submitted;
        ++dispatches;
    }
}
