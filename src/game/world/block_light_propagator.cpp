#include "block_light_propagator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "light_volume.hpp"

namespace {
constexpr int R = BlockLightPropagator::Range;
constexpr int Side = 2 * R + 1;  // per-emitter search box
constexpr int MaxCost = 63;
// Opacity of cells outside the loaded area: impassable, never lit.
constexpr uint8_t Unknown = 255;

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
}  // namespace

BlockLightPropagator::BlockLightPropagator(ChunkManager& chunks,
                                           const DataStorage<Block>& blocks,
                                           ThreadPool& pool,
                                           LightSourceRegistry* registry)
    : chunks(chunks), blocks(blocks), pool(pool), registry(registry) {
    for (const auto& block : blocks.items)
        if (block.light && block.light->emits())
            emissions[block.id] = &*block.light;
    waterConnection = chunks.waterChanged.connect(
        [this](const ChunkCoord& coord) { invalidateAround(coord, false); });
    readyConnection = chunks.becameReady.connect(
        [this](const ChunkCoord& coord) { ready(coord); });
    removedConnection = chunks.removed.connect(
        [this](const ChunkCoord& coord) { removed(coord); });
    if (registry)
        entityConnection = registry->entityChanged.connect(
            [this](const LightSource& source) { entityChanged(source); });
    chunks.forEachReady(
        [this](const ChunkCoord& coord, Chunk&) { ready(coord); });
}

BlockLightPropagator::~BlockLightPropagator() {
    chunks.waterChanged.disconnect(waterConnection);
    chunks.becameReady.disconnect(readyConnection);
    chunks.removed.disconnect(removedConnection);
    if (registry) registry->entityChanged.disconnect(entityConnection);
    if (editor) editor->blockChanged.disconnect(editorConnection);
    for (auto& job : jobs) job.future.wait();
}

void BlockLightPropagator::watch(WorldEditor& value) {
    if (editor) editor->blockChanged.disconnect(editorConnection);
    editor = &value;
    editorConnection =
        editor->blockChanged.connect([this](const BlockChange& change) {
            const auto x = chunks.wrap().wrapTile(change.pos.x);
            if (x < 0 || change.pos.y < 0 || change.pos.z < 0) return;
            rescans.insert({unsigned(x / Chunk::SIZE_X),
                            unsigned(change.pos.y / Chunk::SIZE_Y),
                            unsigned(change.pos.z / Chunk::SIZE_Z)});
        });
}

std::array<uint8_t, 2> BlockLightPropagator::encodeChroma(
    std::array<float, 3> color) {
    const float peak = std::max({color[0], color[1], color[2]});
    if (!(peak > 0)) return {128, 128};
    for (auto& c : color) c /= peak;
    const auto [r, g, b] = color;
    // YCoCg relative to luma: white is (0, 0); Co in [-2, 2], Cg in [-1, 1].
    const float y = (r + 2 * g + b) / 4;
    const float co = (r - b) / 2 / y;
    const float cg = (-r + 2 * g - b) / 4 / y;
    auto byte = [](float v) {
        return uint8_t(std::clamp(std::lround(128 + v), 1l, 255l));
    };
    return {byte(co * 63.5f), byte(cg * 127.f)};
}

bool BlockLightPropagator::offset(ChunkCoord coord, int dx, int dy, int dz,
                                  ChunkCoord& out) const {
    const int64_t x = chunks.wrap().wrapChunk(int64_t{coord.x} + dx);
    const int64_t y = int64_t{coord.y} + dy;
    const int64_t z = int64_t{coord.z} + dz;
    if (x < 0 || z < 0 || y < 0 || y >= Y_CHUNKS ||
        x > std::numeric_limits<unsigned>::max() ||
        z > std::numeric_limits<unsigned>::max())
        return false;
    out = {unsigned(x), unsigned(y), unsigned(z)};
    return true;
}

std::shared_ptr<const BlockLightPropagator::Inputs> BlockLightPropagator::scan(
    const ChunkCoord& coord) {
    auto inputs = std::make_shared<Inputs>();
    std::vector<uint8_t> opacity(Cells);
    bool uniform = true;
    const bool found = chunks.withChunkLocked(coord, [&](const Chunk& chunk) {
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    const Tile& tile = chunk.tiles[z][y][x];
                    const int i = index(x, y, z);
                    opacity[i] = tileOpacity(tile, blocks);
                    uniform &= opacity[i] == opacity[0];
                    if (tile.block_id && emissions[tile.block_id])
                        inputs->emitters.push_back(
                            {uint16_t(i), tile.block_id});
                }
    });
    if (!found) return nullptr;
    if (uniform)
        inputs->uniform = opacity[0];
    else
        inputs->opacity = std::move(opacity);
    return inputs;
}

bool BlockLightPropagator::hasEmitters(const ChunkCoord& coord) const {
    const auto it = states.find(coord);
    return it != states.end() && it->second.inputs &&
           !it->second.inputs->emitters.empty();
}

bool BlockLightPropagator::entityLightsNear(const ChunkCoord& coord) const {
    if (!registry) return false;
    bool near = false;
    const auto wrap = chunks.wrap();
    registry->forEachEntity([&](LightSourceRegistry::Id, const LightSource& s) {
        if (near || !s.emission.emits()) return;
        const double ox = coord.x * double(Chunk::SIZE_X);
        const double dx = wrap.deltaX(s.position[0], ox + 8) + 8;
        const double dy = s.position[1] - coord.y * double(Chunk::SIZE_Y);
        const double dz = s.position[2] - coord.z * double(Chunk::SIZE_Z);
        near = dx > -R - 1 && dx < Chunk::SIZE_X + R + 1 && dy > -R - 1 &&
               dy < Chunk::SIZE_Y + R + 1 && dz > -R - 1 &&
               dz < Chunk::SIZE_Z + R + 1;
    });
    return near;
}

void BlockLightPropagator::invalidateChunk(const ChunkCoord& coord) {
    auto it = states.find(coord);
    if (it == states.end()) return;
    it->second.revision = ++nextRevision;
    pending.insert(coord);
}

void BlockLightPropagator::invalidateAround(const ChunkCoord& coord,
                                            bool ownEmitters) {
    // An emitter lights cells within R < 16 tiles, so the light of a chunk
    // depends only on emitters in its 3^3 neighborhood, and those emitters'
    // paths stay inside their own 3^3 neighborhoods. A tile change in coord
    // can therefore affect chunks up to two away, but only around emitters
    // within one chunk of coord.
    invalidateChunk(coord);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord e;
                if (!offset(coord, dx, dy, dz, e)) continue;
                const bool source = (dx == 0 && dy == 0 && dz == 0)
                                        ? ownEmitters || hasEmitters(e)
                                        : hasEmitters(e);
                if (!source && !entityLightsNear(e)) {
                    // Light previously spread into this chunk from elsewhere
                    // still needs clearing when its source unloads.
                    if (auto it = states.find(e);
                        it != states.end() && it->second.lit)
                        invalidateChunk(e);
                    continue;
                }
                for (int ez = -1; ez <= 1; ++ez)
                    for (int ey = -1; ey <= 1; ++ey)
                        for (int ex = -1; ex <= 1; ++ex) {
                            ChunkCoord n;
                            if (offset(e, ex, ey, ez, n)) invalidateChunk(n);
                        }
            }
}

void BlockLightPropagator::reconcileRegistry(const ChunkCoord& coord,
                                             const Inputs& inputs) {
    if (!registry) return;
    const TilePos origin{int(coord.x) * Chunk::SIZE_X,
                         int(coord.y) * Chunk::SIZE_Y,
                         int(coord.z) * Chunk::SIZE_Z};
    std::unordered_set<TilePos> present;
    for (const auto& e : inputs.emitters) {
        const auto& emission = *emissions[e.block];
        const int x = e.cell % Chunk::SIZE_X,
                  y = (e.cell / Chunk::SIZE_X) % Chunk::SIZE_Y,
                  z = e.cell / (Chunk::SIZE_X * Chunk::SIZE_Y);
        const TilePos tile{origin.x + x, origin.y + y, origin.z + z};
        present.insert(tile);
        registry->setBlockLight(tile, e.block, emission,
                                openFaces(coord, x, y, z));
    }
    const TilePos end{origin.x + Chunk::SIZE_X, origin.y + Chunk::SIZE_Y,
                      origin.z + Chunk::SIZE_Z};
    for (const auto& tile : registry->blockLightsIn(origin, end))
        if (!present.contains(tile)) registry->removeBlockLight(tile);
}

uint8_t BlockLightPropagator::openFaces(const ChunkCoord& coord, int x, int y,
                                        int z) const {
    constexpr int Steps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    uint8_t mask = 0;
    for (int i = 0; i < 6; ++i) {
        int nx = x + Steps[i][0], ny = y + Steps[i][1], nz = z + Steps[i][2];
        const int ox = nx < 0 ? -1 : nx >= Chunk::SIZE_X ? 1 : 0;
        const int oy = ny < 0 ? -1 : ny >= Chunk::SIZE_Y ? 1 : 0;
        const int oz = nz < 0 ? -1 : nz >= Chunk::SIZE_Z ? 1 : 0;
        ChunkCoord n = coord;
        if ((ox || oy || oz) && !offset(coord, ox, oy, oz, n)) continue;
        const auto it = states.find(n);
        // Not loaded: assume open until it arrives and reconciles.
        if (it == states.end() || !it->second.inputs) {
            mask |= uint8_t(1u << i);
            continue;
        }
        const auto& inputs = *it->second.inputs;
        nx -= ox * Chunk::SIZE_X;
        ny -= oy * Chunk::SIZE_Y;
        nz -= oz * Chunk::SIZE_Z;
        const uint8_t opacity = inputs.opacity.empty()
                                    ? inputs.uniform
                                    : inputs.opacity[index(nx, ny, nz)];
        if (opacity < 15) mask |= uint8_t(1u << i);
    }
    return mask;
}

void BlockLightPropagator::reconcileAround(const ChunkCoord& coord) {
    // Border emitters of the face neighbors see this chunk's tiles too.
    constexpr int Faces[7][3] = {{0, 0, 0},  {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& f : Faces) {
        ChunkCoord n;
        if (!offset(coord, f[0], f[1], f[2], n)) continue;
        const auto it = states.find(n);
        if (it != states.end() && it->second.inputs)
            reconcileRegistry(n, *it->second.inputs);
    }
}

void BlockLightPropagator::ready(const ChunkCoord& coord) {
    auto& state = states[coord];
    state.inputs = scan(coord);
    reconcileAround(coord);
    invalidateAround(coord, hasEmitters(coord));
}

void BlockLightPropagator::removed(const ChunkCoord& coord) {
    const bool had = hasEmitters(coord);
    states.erase(coord);
    chromas.erase(coord);
    pending.erase(coord);
    rescans.erase(coord);
    // Neighbors lose its emitters and its open cells.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord n;
                if (!offset(coord, dx, dy, dz, n) || !states.contains(n))
                    continue;
                if (had || hasEmitters(n) || states.at(n).lit)
                    invalidateAround(n, false);
            }
}

void BlockLightPropagator::entityChanged(const LightSource& source) {
    const auto x = chunks.wrap().wrapX(source.position[0]);
    if (x < 0 || source.position[1] < 0 || source.position[2] < 0) return;
    const ChunkCoord coord{unsigned(x / Chunk::SIZE_X),
                           unsigned(source.position[1] / Chunk::SIZE_Y),
                           unsigned(source.position[2] / Chunk::SIZE_Z)};
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                ChunkCoord n;
                if (offset(coord, dx, dy, dz, n)) invalidateChunk(n);
            }
}

const BlockLightPropagator::Chroma* BlockLightPropagator::chroma(
    const ChunkCoord& coord) const {
    const auto it = chromas.find(coord);
    return it == chromas.end() ? nullptr : it->second.get();
}

bool BlockLightPropagator::isIdle() const {
    return pending.empty() && jobs.empty() && rescans.empty();
}

BlockLightPropagator::Input BlockLightPropagator::snapshot(
    const ChunkCoord& coord) {
    Input input{coord, states.at(coord).revision, {}, {}};
    constexpr int H = AreaSide / 2;
    for (int dz = -H; dz <= H; ++dz)
        for (int dy = -H; dy <= H; ++dy)
            for (int dx = -H; dx <= H; ++dx) {
                ChunkCoord n;
                if (!offset(coord, dx, dy, dz, n)) continue;
                const auto it = states.find(n);
                if (it == states.end()) continue;
                const int slot =
                    ((dz + H) * AreaSide + (dy + H)) * AreaSide + (dx + H);
                input.area[slot] = it->second.inputs;
                if (std::abs(dx) > 1 || std::abs(dy) > 1 || std::abs(dz) > 1 ||
                    !it->second.inputs)
                    continue;
                for (const auto& e : it->second.inputs->emitters) {
                    Emitter emitter;
                    emitter.x = dx * Chunk::SIZE_X + e.cell % Chunk::SIZE_X;
                    emitter.y = dy * Chunk::SIZE_Y +
                                (e.cell / Chunk::SIZE_X) % Chunk::SIZE_Y;
                    emitter.z = dz * Chunk::SIZE_Z +
                                e.cell / (Chunk::SIZE_X * Chunk::SIZE_Y);
                    // Only emitters within reach of the target box.
                    auto gap = [](int v, int size) {
                        return std::max({0, -v, v - (size - 1)});
                    };
                    const int gx = gap(emitter.x, Chunk::SIZE_X),
                              gy = gap(emitter.y, Chunk::SIZE_Y),
                              gz = gap(emitter.z, Chunk::SIZE_Z);
                    if (gx * gx + gy * gy + gz * gz > R * R) continue;
                    emitter.emission = *emissions[e.block];
                    if (emitter.emission.cone)
                        emitter.forward = emitter.emission.cone->direction;
                    emitter.chroma = encodeChroma(emitter.emission.color);
                    input.emitters.push_back(std::move(emitter));
                }
            }
    if (registry) {
        const auto wrap = chunks.wrap();
        const double ox = coord.x * double(Chunk::SIZE_X);
        registry->forEachEntity([&](LightSourceRegistry::Id,
                                    const LightSource& s) {
            if (!s.emission.emits()) return;
            Emitter emitter;
            emitter.x = int(std::floor(wrap.deltaX(s.position[0], ox + 8) + 8));
            emitter.y =
                int(std::floor(s.position[1])) - int(coord.y) * Chunk::SIZE_Y;
            emitter.z =
                int(std::floor(s.position[2])) - int(coord.z) * Chunk::SIZE_Z;
            if (emitter.x < -R || emitter.x >= Chunk::SIZE_X + R ||
                emitter.y < -R || emitter.y >= Chunk::SIZE_Y + R ||
                emitter.z < -R || emitter.z >= Chunk::SIZE_Z + R)
                return;
            emitter.emission = s.emission;
            emitter.forward = s.direction;
            emitter.chroma = encodeChroma(s.emission.color);
            input.emitters.push_back(std::move(emitter));
        });
    }
    return input;
}

namespace {
// Line of sight from the centre of the emitter cell to the centre of a
// target cell through the opacity grid (Amanatides-Woo traversal). Returns
// false when an opaque or unknown cell blocks it; otherwise adds the
// translucent penalty (opacity - 1 per cell crossed, as the flood fill
// charges) to `penalty`. Where the line passes exactly through an edge or a
// corner it is blocked only if every cell around that crossing blocks, so
// light does not leak through diagonal cracks of a wall but passes beside a
// single block.
template <typename Opacity>
bool lineOfSight(int ex, int ey, int ez, int dx, int dy, int dz,
                 const Opacity& opacity, int& penalty) {
    const int target[3] = {ex + dx, ey + dy, ez + dz};
    int cell[3] = {ex, ey, ez};
    const int d[3] = {dx, dy, dz};
    int step[3];
    double tMax[3], tDelta[3];
    for (int a = 0; a < 3; ++a) {
        step[a] = d[a] > 0 ? 1 : (d[a] < 0 ? -1 : 0);
        // From the cell centre, the first boundary is half a cell away.
        tDelta[a] = d[a] ? 1.0 / std::abs(d[a]) : 1e30;
        tMax[a] = d[a] ? 0.5 * tDelta[a] : 1e30;
    }
    while (true) {
        const double t = std::min({tMax[0], tMax[1], tMax[2]});
        if (t >= 1.0 - 1e-9) return true;  // reached the target cell
        bool tied[3];
        int ties = 0;
        for (int a = 0; a < 3; ++a) {
            tied[a] = std::abs(tMax[a] - t) < 1e-9;
            ties += tied[a];
        }
        if (ties > 1) {
            // Crossing an edge or a corner: blocked only when every cell the
            // line grazes (one step along each tied axis) blocks.
            bool all = true;
            for (int a = 0; a < 3 && all; ++a) {
                if (!tied[a]) continue;
                int n[3] = {cell[0], cell[1], cell[2]};
                n[a] += step[a];
                all = opacity(n[0], n[1], n[2]) >= 15;
            }
            if (all) return false;
        }
        for (int a = 0; a < 3; ++a)
            if (tied[a]) {
                cell[a] += step[a];
                tMax[a] += tDelta[a];
            }
        if (cell[0] == target[0] && cell[1] == target[1] &&
            cell[2] == target[2])
            return true;
        const uint8_t o = opacity(cell[0], cell[1], cell[2]);
        if (o >= 15) return false;
        penalty += std::max(0, int(o) - 1);
    }
}
}  // namespace

template <typename Opacity, typename Contribute>
void BlockLightPropagator::directLight(const Emitter& e, float limit,
                                       const Opacity& opacity,
                                       Contribute&& contribute) {
    // Only cells of the target chunk are written.
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int y = 0; y < Chunk::SIZE_Y; ++y)
            for (int x = 0; x < Chunk::SIZE_X; ++x) {
                const int dx = x - e.x, dy = y - e.y, dz = z - e.z;
                const float straight =
                    std::sqrt(float(dx * dx + dy * dy + dz * dz));
                if (straight >= limit) continue;
                const bool origin = dx == 0 && dy == 0 && dz == 0;
                // Opaque cells hold no light (LightVolume stores zero); a
                // lamp block's own cell stays dark too.
                if (opacity(x, y, z) >= 15) continue;
                int penalty = 0;
                if (!origin &&
                    !lineOfSight(e.x, e.y, e.z, dx, dy, dz, opacity, penalty))
                    continue;
                const float d = straight + float(penalty);
                if (d >= limit) continue;
                contribute(dx, dy, dz, d, origin);
            }
}

BlockLightPropagator::Result BlockLightPropagator::compute(Input input) {
    Result result{input.coord, input.revision, {}, nullptr};
    if (input.emitters.empty()) return result;

    // Target-relative cell -> opacity; the area spans [-32, 48) per axis.
    auto opacity = [&](int x, int y, int z) -> uint8_t {
        constexpr int Lo = -(AreaSide / 2) * 16, Hi = Lo + AreaSide * 16;
        if (x < Lo || y < Lo || z < Lo || x >= Hi || y >= Hi || z >= Hi)
            return Unknown;
        const int cx = (x - Lo) >> 4, cy = (y - Lo) >> 4, cz = (z - Lo) >> 4;
        const auto& chunk = input.area[(cz * AreaSide + cy) * AreaSide + cx];
        if (!chunk) return Unknown;
        if (chunk->opacity.empty()) return chunk->uniform;
        return chunk->opacity[index(x & 15, y & 15, z & 15)];
    };

    const bool colored =
        std::any_of(input.emitters.begin(), input.emitters.end(),
                    [](const Emitter& e) { return !e.emission.isWhite(); });
    // Light adds up across emitters, like the per-pixel far term, so the
    // hand-over between the two never jumps.
    std::vector<float> total(Cells, 0.f);
    std::vector<float> weight, co, cg;
    if (colored) {
        weight.assign(Cells, 0.f);
        co.assign(Cells, 0.f);
        cg.assign(Cells, 0.f);
    }

    std::vector<uint8_t> cost(Side * Side * Side);
    std::array<std::vector<int>, MaxCost + 1> buckets;
    auto box = [](int dx, int dy, int dz) {
        return ((dz + R) * Side + (dy + R)) * Side + (dx + R);
    };
    constexpr int Steps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

    for (const auto& e : input.emitters) {
        const auto& emission = e.emission;
        const float limit = std::min<float>(emission.radius, float(R));
        const bool far = emission.isFar();
        // Adds this emitter's light at distance d to a target cell.
        auto contribute = [&](int dx, int dy, int dz, float d, bool origin) {
            float value = emission.attenuation(d);
            if (emission.cone && !origin) {
                const float len = std::sqrt(float(dx * dx + dy * dy + dz * dz));
                value *= emission.coneFactor({dx / len, dy / len, dz / len},
                                             e.forward);
            }
            // Far lights hand over to the per-pixel term (which fades in
            // over the same band) at the near-field edge.
            if (far) value *= 1.f - smoothstep(R - 4.f, float(R), d);
            const int cell = index(e.x + dx, e.y + dy, e.z + dz);
            total[cell] += value;
            if (colored && value > 0) {
                weight[cell] += value;
                co[cell] += value * (e.chroma[0] - 128);
                cg[cell] += value * (e.chroma[1] - 128);
            }
        };
        if (far) {
            // Far lights are direct light, like their shadow maps: a cell is
            // lit when the straight line from the light reaches it, so the
            // near field and the per-pixel term agree where they hand over.
            directLight(e, limit, opacity, contribute);
            continue;
        }
        std::fill(cost.begin(), cost.end(), uint8_t(255));
        // Effective distance: straight line plus the path's excess over the
        // Manhattan distance (detours and translucent penalties). It never
        // decreases along a path, so pruning beyond `limit` is exact.
        auto effective = [](int dx, int dy, int dz, int pathCost) {
            const float straight =
                std::sqrt(float(dx * dx + dy * dy + dz * dz));
            const int manhattan = std::abs(dx) + std::abs(dy) + std::abs(dz);
            return straight + float(pathCost - manhattan);
        };
        cost[box(0, 0, 0)] = 0;
        buckets[0].push_back(box(0, 0, 0));
        for (int k = 0; k <= MaxCost; ++k) {
            auto& bucket = buckets[k];
            for (size_t b = 0; b < bucket.size(); ++b) {
                const int at = bucket[b];
                if (cost[at] != k) continue;
                const int dx = at % Side - R, dy = (at / Side) % Side - R,
                          dz = at / (Side * Side) - R;
                const int x = e.x + dx, y = e.y + dy, z = e.z + dz;
                const bool origin = dx == 0 && dy == 0 && dz == 0;
                const uint8_t own = opacity(x, y, z);
                // A lamp block keeps its own cell dark (it is opaque); an
                // entity light in open air lights its cell fully.
                if ((!origin || own < 15) && x >= 0 && y >= 0 && z >= 0 &&
                    x < Chunk::SIZE_X && y < Chunk::SIZE_Y &&
                    z < Chunk::SIZE_Z) {
                    contribute(dx, dy, dz, effective(dx, dy, dz, k), origin);
                }
                // Opaque tiles receive light on their faces but pass none on.
                if (!origin && own >= 15) continue;
                const int leave = origin ? 1 : std::max<int>(1, own);
                const int next = k + leave;
                if (next > MaxCost) continue;
                for (const auto& s : Steps) {
                    // A spotlight leaves its tile only through faces of its
                    // front hemisphere: one facing a wall lights nothing.
                    if (origin && emission.cone &&
                        s[0] * e.forward[0] + s[1] * e.forward[1] +
                                s[2] * e.forward[2] <=
                            1e-4f)
                        continue;
                    const int nx = dx + s[0], ny = dy + s[1], nz = dz + s[2];
                    if (std::abs(nx) > R || std::abs(ny) > R ||
                        std::abs(nz) > R)
                        continue;
                    const int n = box(nx, ny, nz);
                    if (cost[n] <= next) continue;
                    if (opacity(e.x + nx, e.y + ny, e.z + nz) == Unknown)
                        continue;
                    if (effective(nx, ny, nz, next) >= limit) continue;
                    cost[n] = uint8_t(next);
                    buckets[next].push_back(n);
                }
            }
            bucket.clear();
        }
    }

    for (int i = 0; i < Cells; ++i)
        result.light[i] =
            uint8_t(std::clamp(int(std::lround(total[i] * 15.f)), 0, 15));

    if (colored) {
        bool any = false;
        auto chroma = std::make_unique<Chroma>();
        for (int i = 0; i < Cells; ++i) {
            uint8_t a = 128, b = 128;
            if (weight[i] > 0 && result.light[i] > 0) {
                a = uint8_t(
                    std::clamp(std::lround(128 + co[i] / weight[i]), 1l, 255l));
                b = uint8_t(
                    std::clamp(std::lround(128 + cg[i] / weight[i]), 1l, 255l));
            }
            (*chroma)[i * 2] = a;
            (*chroma)[i * 2 + 1] = b;
            any |= a != 128 || b != 128;
        }
        if (any) result.chroma = std::move(chroma);
    }
    return result;
}

void BlockLightPropagator::publish(const ChunkCoord& coord,
                                   const std::array<uint8_t, Cells>& light,
                                   std::unique_ptr<Chroma> chroma) {
    bool changed = false;
    bool lit = false;
    chunks.withChunkLocked(coord, [&](Chunk& chunk) {
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    auto& level = chunk.tiles[z][y][x].lightLevel;
                    const auto value = light[index(x, y, z)];
                    changed |= level.artificial_light != value;
                    level.artificial_light = value;
                    lit |= value != 0;
                }
    });
    const auto it = chromas.find(coord);
    const Chroma* before = it == chromas.end() ? nullptr : it->second.get();
    if (bool(before) != bool(chroma) || (before && *before != *chroma))
        changed = true;
    if (chroma)
        chromas[coord] = std::move(chroma);
    else if (it != chromas.end())
        chromas.erase(it);
    if (auto s = states.find(coord); s != states.end()) s->second.lit = lit;
    // Light reaches the GPU through light volumes; no remesh.
    if (changed) chunks.markLightChanged(coord);
}

void BlockLightPropagator::collect() {
    using namespace std::chrono_literals;
    for (size_t i = 0; i < jobs.size();) {
        if (jobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto future = std::move(jobs[i].future);
        const auto coord = jobs[i].coord;
        jobs.erase(jobs.begin() + i);
        Result result;
        try {
            result = future.get();
        } catch (...) {
            invalidateChunk(coord);
            throw;
        }
        const auto it = states.find(coord);
        if (it == states.end() || it->second.revision != result.revision)
            continue;
        publish(coord, result.light, std::move(result.chroma));
    }
}

void BlockLightPropagator::update() {
    pending.setPeriodX(chunks.wrap().tiles);
    for (const auto& coord : rescans) {
        auto it = states.find(coord);
        if (it == states.end()) continue;
        const bool had = hasEmitters(coord);
        it->second.inputs = scan(coord);
        reconcileAround(coord);
        invalidateAround(coord, had || hasEmitters(coord));
    }
    rescans.clear();
    collect();

    const auto started = std::chrono::steady_clock::now();
    const size_t capacity = std::clamp<size_t>(pool.get_thread_count(), 1, 8);
    for (const auto& coord : pending.ordered(focus, false)) {
        if (jobs.size() >= capacity ||
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(2))
            break;
        if (std::any_of(jobs.begin(), jobs.end(),
                        [&](const Job& job) { return job.coord == coord; }))
            continue;  // rescheduled after the running job finishes
        auto it = states.find(coord);
        if (it == states.end() || !it->second.inputs) {
            pending.erase(coord);
            continue;
        }
        bool sources = entityLightsNear(coord);
        for (int dz = -1; dz <= 1 && !sources; ++dz)
            for (int dy = -1; dy <= 1 && !sources; ++dy)
                for (int dx = -1; dx <= 1 && !sources; ++dx) {
                    ChunkCoord n;
                    sources = offset(coord, dx, dy, dz, n) && hasEmitters(n);
                }
        pending.erase(coord);
        if (!sources) {
            // Nothing can light it: clear stale light without a job.
            if (it->second.lit || chromas.contains(coord))
                publish(coord, {}, nullptr);
            continue;
        }
        auto input = snapshot(coord);
        // Below meshing and sunlight: block light refines a drawn chunk.
        auto future = pool.submit_task(
            [input = std::move(input)]() mutable {
                return compute(std::move(input));
            },
            BS::pr::normal);
        jobs.push_back({coord, std::move(future)});
    }
}
