#include "structure_lod.hpp"

#include <bx/math.h>

#include <iostream>
#include <unordered_set>

#include "save/chunk_codec.hpp"

StructureLod::StructureLod(const World& world, IAssets& assets,
                           ThreadPool& pool,
                           std::shared_ptr<SurfaceInputCache> inputs,
                           size_t budget, float viewDistance)
    : world(world),
      assets(assets),
      pool(pool),
      inputs(std::move(inputs)),
      mesher(assets),
      budget(budget),
      viewDistance(viewDistance) {}

StructureLod::~StructureLod() {
    for (auto& job : jobs) {
        try {
            job.future.get();
        } catch (const std::exception& e) {
            std::cerr << "Structure LOD shutdown: " << e.what() << '\n';
        }
    }
}

void StructureLod::edited(uint64_t column) {
    ++revision;
    const int cx = int(column >> 32), cz = int(uint32_t(column));
    for (auto& [coord, proxy] : proxies)
        if (std::abs(world.wrap().deltaChunk(coord.x, cx)) +
                std::abs(int(coord.z) - cz) <=
            1)
            proxy->staleRevision = revision;
    // Force a new selection: the slab set of this column may have changed.
    chosenFor = nullptr;
}

void StructureLod::update(Vec3 eye, std::shared_ptr<const EditMap> edits) {
    snapshot = std::move(edits);
    collect();
    choose(eye);
    request();
}

void StructureLod::choose(Vec3 eye) {
    const std::array<int, 3> cell{int(eye.x) / 32, int(eye.y) / 32,
                                  int(eye.z) / 32};
    if (snapshot == chosenFor && cell == chosenCell) return;
    chosenFor = snapshot;
    chosenCell = cell;
    struct Candidate {
        ChunkCoord coord;
        float distance;
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, edit] : *snapshot) {
        // The column copy nearest the eye (X wraps).
        const float x0 = drawX(unsigned(key >> 32), eye.x),
                    z0 = float(uint32_t(key)) * Chunk::SIZE_Z;
        const float dx = std::max({x0 - eye.x, 0.f, eye.x - x0 - 16});
        const float dz = std::max({z0 - eye.z, 0.f, eye.z - z0 - 16});
        if (std::hypot(dx, dz) >= viewDistance) continue;
        for (const auto& [slabY, blob] : edit->structure) {
            const ChunkCoord coord{unsigned(key >> 32), slabY,
                                   unsigned(uint32_t(key))};
            const float cy = slabY * 16.f + 8;
            candidates.push_back({coord, std::hypot(x0 + 8 - eye.x, cy - eye.y,
                                                    z0 + 8 - eye.z)});
        }
    }
    std::sort(
        candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.distance < b.distance; });
    wanted.clear();
    std::unordered_set<ChunkCoord> keep;
    for (const auto& c : candidates) {
        wanted.push_back(c.coord);
        keep.insert(c.coord);
    }
    // Removed structures and slabs out of view release their meshes.
    for (auto it = proxies.begin(); it != proxies.end();)
        if (!keep.contains(it->first)) {
            resident -= it->second->bytes;
            it = proxies.erase(it);
        } else
            ++it;
}

void StructureLod::collect() {
    using namespace std::chrono_literals;
    for (size_t i = 0; i < jobs.size();) {
        if (jobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(jobs[i]);
        jobs.erase(jobs.begin() + i);
        try {
            auto mesh = job.future.get();
            auto it = proxies.find(job.slab);
            if (it == proxies.end()) continue;
            auto& proxy = *it->second;
            if (proxy.cpu && proxy.cpuRevision >= job.revision) continue;
            proxy.cpu = std::move(mesh);
            proxy.cpuRevision = job.revision;
        } catch (const std::exception& e) {
            std::cerr << "Structure LOD build failed: " << e.what() << '\n';
        }
    }
}

void StructureLod::request() {
    for (const auto& coord : wanted) {
        if (jobs.size() >= MaxJobs) break;
        auto& slot = proxies[coord];
        if (!slot) slot = std::make_unique<Proxy>();
        auto& proxy = *slot;
        const bool current = proxy.drawable && !proxy.stale();
        const bool building =
            (proxy.cpu && proxy.cpuRevision >= proxy.staleRevision) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) {
                return j.slab == coord && j.revision >= proxy.staleRevision;
            });
        if (current || building) continue;
        // New proxies only while the budget has room; rebuilds always.
        if (!proxy.drawable && resident >= budget) break;
        auto future = pool.submit_task(
            [coord, edits = snapshot, mesher = mesher, source = &world,
             definitions = &assets, shared = inputs] {
                SurfaceSampler ground(*source, *definitions, shared, edits);
                std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>> decoded;
                const auto structureChunk = [&](ChunkCoord c) -> const Chunk* {
                    auto [it, inserted] = decoded.try_emplace(c);
                    if (inserted) {
                        const auto edit = edits->find(editKey(c.x, c.z));
                        const ChunkBlob* blob =
                            edit == edits->end()
                                ? nullptr
                                : edit->second->slab(uint16_t(c.y));
                        if (blob) {
                            it->second = std::make_unique<Chunk>();
                            chunk_codec::decode(**blob, *it->second);
                        }
                    }
                    return it->second.get();
                };
                static const Tile air{};
                static const Tile solid = [] {
                    Tile tile;
                    tile.block_id = 1;
                    return tile;
                }();
                const int width = ground.width(), depth = ground.depth();
                const auto* self = structureChunk(coord);
                if (!self) return ChunkMesh{};
                return mesher.build(
                    *self, coord, [&](int x, int y, int z) -> const Tile* {
                        x = int(WorldWrap::mod(x, width));
                        if (x < 0 || z < 0 || y < 0 || z >= depth ||
                            y >= WORLD_HEIGHT_Y)
                            return &air;
                        const ChunkCoord c{unsigned(x / Chunk::SIZE_X),
                                           unsigned(y / Chunk::SIZE_Y),
                                           unsigned(z / Chunk::SIZE_Z)};
                        if (const auto* chunk = structureChunk(c)) {
                            const auto& tile = chunk->tiles[z % Chunk::SIZE_Z]
                                                           [y % Chunk::SIZE_Y]
                                                           [x % Chunk::SIZE_X];
                            if (tile.isSolid()) return &tile;
                        }
                        // Faces resting on the (possibly edited) ground hide.
                        return y <= ground.height(x, z) ? &solid : &air;
                    });
            },
            BS::pr::low);
        jobs.push_back({coord, revision, std::move(future)});
    }
}

size_t StructureLod::upload(IRenderer& renderer, size_t limit) {
    size_t uploaded = 0;
    for (const auto& coord : wanted) {
        auto it = proxies.find(coord);
        if (it == proxies.end() || !it->second->cpu) continue;
        auto& proxy = *it->second;
        const auto& cpu = *proxy.cpu;
        const size_t bytes = cpu.vertices.size() * sizeof(BlockVertex) +
                             cpu.indices.size() * sizeof(uint32_t);
        if (uploaded && uploaded + bytes > limit) break;
        if (!proxy.drawable && resident + bytes > budget) continue;
        MeshHandle mesh;
        if (!cpu.vertices.empty()) {
            mesh.initDynamic<BlockVertex>(&renderer, cpu.vertices.size(),
                                          cpu.indices.size());
            const auto& gpu = std::get<DynamicMesh>(mesh.get());
            if (!bgfx::isValid(gpu.vbh) || !bgfx::isValid(gpu.ibh)) continue;
            mesh.update(cpu.vertices.data(), cpu.vertices.size(),
                        cpu.indices.data(), cpu.indices.size(), true);
        }
        // The previous mesh is released here, after its last draw.
        proxy.mesh = std::move(mesh);
        proxy.hasMesh = !cpu.vertices.empty();
        resident = resident - proxy.bytes + bytes;
        proxy.bytes = bytes;
        proxy.bounds = {float(coord.x) * Chunk::SIZE_X,
                        float(coord.y) * Chunk::SIZE_Y,
                        float(coord.z) * Chunk::SIZE_Z,
                        float(coord.x + 1) * Chunk::SIZE_X,
                        float(coord.y + 1) * Chunk::SIZE_Y,
                        float(coord.z + 1) * Chunk::SIZE_Z};
        proxy.builtRevision = proxy.cpuRevision;
        proxy.drawable = true;
        proxy.cpu.reset();
        uploaded += bytes;
        if (uploaded >= limit) break;
    }
    return uploaded;
}

size_t StructureLod::backlog() const {
    return std::count_if(wanted.begin(), wanted.end(), [&](const auto& c) {
        const auto it = proxies.find(c);
        return it == proxies.end() || !it->second->drawable ||
               it->second->stale();
    });
}
