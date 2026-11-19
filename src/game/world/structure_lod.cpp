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
      voxelBudget(budget * 4),
      viewDistance(viewDistance) {}

StructureLod::~StructureLod() {
    for (auto& job : jobs) {
        try {
            job.future.get();
        } catch (const std::exception& e) {
            std::cerr << "Structure LOD shutdown: " << e.what() << '\n';
        }
    }
    for (auto& [key, structure] : structures)
        if (structure.job) structure.job->future.wait();
}

void StructureLod::setStructures(
    std::vector<structure_voxels::SourcePtr> list) {
    std::unordered_set<uint64_t> seen;
    for (auto& source : list) {
        if (!source) continue;
        seen.insert(source->key);
        auto& entry = structures[source->key];
        // The same voxels: keep the old source (plans compare revisions).
        if (entry.wanted && entry.wanted->revision == source->revision)
            continue;
        entry.wanted = std::move(source);
        replan = true;
    }
    for (auto& [key, structure] : structures)
        if (!seen.contains(key) && structure.wanted) {
            structure.wanted = nullptr;
            replan = true;
        }
}

void StructureLod::planStructures(Vec3 eye) {
    const std::array<int, 3> cell{int(eye.x) / 32, int(eye.y) / 32,
                                  int(eye.z) / 32};
    if (!replan && cell == plannedCell) return;
    replan = false;
    plannedCell = cell;
    for (auto& [key, structure] : structures) {
        structure.pending.clear();
        structure.pendingPlan.clear();
        if (!structure.wanted || structure.job) continue;
        const auto& source = *structure.wanted;
        auto planned =
            structure_voxels::plan(source.bounds, eye.x, eye.y, eye.z,
                                   world.wrap(), structure.covered);
        structure.distance = planned.empty() ? 0 : planned.front().distance;
        const bool same =
            structure.built && structure.built->revision == source.revision;
        // A new revision of the same structure: only slabs touching the
        // changed box are rebuilt when the source can tell where it is.
        std::optional<TileBox> changed;
        if (structure.built && !same)
            changed = source.changedSince(*structure.built);
        const bool keep = same || changed.has_value();
        std::unordered_set<ChunkCoord> plan;
        std::vector<structure_voxels::Planned> missing;
        for (const auto& p : planned) {
            plan.insert(p.key);
            if (!keep || !structure.covered.contains(p.key) ||
                (changed &&
                 !changed->intersect(structure_voxels::boxOf(p)).empty()))
                missing.push_back(p);
        }
        if (missing.empty() && keep && plan == structure.covered) {
            // Nothing planned changed: adopt the new revision as built.
            if (!same) structure.built = structure.wanted;
            continue;
        }
        structure.pending = std::move(missing);
        structure.pendingPlan = std::move(plan);
        structure.pendingKeep = keep;
    }
}

void StructureLod::requestStructures() {
    size_t running =
        std::count_if(structures.begin(), structures.end(),
                      [](const auto& s) { return s.second.job.has_value(); });
    // Nearest structures first.
    std::vector<Structure*> order;
    for (auto& [key, structure] : structures)
        if (!structure.job && structure.wanted &&
            !structure.pendingPlan.empty())
            order.push_back(&structure);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) {
        return a->distance < b->distance;
    });
    for (auto* next : order) {
        if (running >= MaxVoxelJobs) break;
        auto& structure = *next;
        std::unordered_set<ChunkCoord> rebuilt;
        for (const auto& p : structure.pending) rebuilt.insert(p.key);
        auto future = pool.submit_task(
            [source = structure.wanted, slabs = std::move(structure.pending)] {
                VoxelSlabs out;
                for (const auto& p : slabs)
                    if (auto slab =
                            source->rasterize(levelOf(p.key), p.sx, p.sy, p.sz))
                        out[p.key] = std::move(*slab);
                return out;
            },
            BS::pr::low);
        structure.job = Structure::Build{
            structure.wanted, std::move(structure.pendingPlan),
            std::move(future), std::move(rebuilt), structure.pendingKeep};
        structure.pending.clear();
        structure.pendingPlan.clear();
        ++running;
    }
}

void StructureLod::collectStructures() {
    using namespace std::chrono_literals;
    std::vector<ChunkCoord> changed;
    const auto touched = [&](const std::shared_ptr<const VoxelSlabs>& slabs) {
        if (slabs)
            for (const auto& [coord, slab] : *slabs) changed.push_back(coord);
    };
    for (auto it = structures.begin(); it != structures.end();) {
        auto& structure = it->second;
        if (structure.job &&
            structure.job->future.wait_for(0s) == std::future_status::ready) {
            auto job = std::move(*structure.job);
            structure.job.reset();
            replan = true;
            std::optional<VoxelSlabs> built;
            try {
                built = job.future.get();
            } catch (const std::exception& e) {
                std::cerr << "Structure voxels failed: " << e.what() << '\n';
            }
            if (built) {
                auto next = std::make_shared<VoxelSlabs>();
                if (job.keep && structure.slabs) {
                    // Slabs still in the plan stay; rebuilt ones go.
                    for (const auto& [coord, slab] : *structure.slabs)
                        if (job.plan.contains(coord) &&
                            !job.rebuilt.contains(coord))
                            next->emplace(coord, slab);
                        else
                            changed.push_back(coord);
                } else {
                    touched(structure.slabs);
                }
                for (auto& [coord, slab] : *built) {
                    changed.push_back(coord);
                    (*next)[coord] = std::move(slab);
                }
                structure_voxels::markEnclosed(*next, world.wrap());
                structure.slabs = std::move(next);
                structure.built = std::move(job.source);
                structure.covered = std::move(job.plan);
            }
        }
        // Out of view: its slabs go once no job uses the entry.
        if (!structure.wanted && !structure.job) {
            touched(structure.slabs);
            it = structures.erase(it);
            continue;
        }
        ++it;
    }
    if (!changed.empty()) mergeVoxels(changed);
}

void StructureLod::mergeVoxels(const std::vector<ChunkCoord>& changed) {
    auto merged = std::make_shared<VoxelSlabs>();
    for (const auto& [key, structure] : structures)
        if (structure.slabs && structure.wanted)
            for (const auto& [coord, slab] : *structure.slabs) {
                auto [it, inserted] = merged->try_emplace(coord, slab);
                if (!inserted) it->second.merge(slab);
            }
    voxels = merged->empty() ? nullptr : std::move(merged);
    // Border faces look into the neighbours: they change too.
    std::unordered_set<ChunkCoord> stale;
    for (const auto& c : changed)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 1)
                        continue;
                    const int level = levelOf(c);
                    const auto base = baseOf(c);
                    if (const auto key = structure_voxels::keyOf(
                            level, int(base.x) + dx, int(base.y) + dy,
                            int(base.z) + dz, world.wrap()))
                        stale.insert(*key);
                }
    ++revision;
    for (auto& [coord, proxy] : proxies)
        if (stale.contains(coord)) proxy->staleRevision = revision;
}

size_t StructureLod::voxelSlabCount(int level) const {
    if (!voxels) return 0;
    if (level < 0) return voxels->size();
    return size_t(std::count_if(
        voxels->begin(), voxels->end(),
        [&](const auto& v) { return levelOf(v.first) == level; }));
}

void StructureLod::edited(uint64_t column) {
    ++revision;
    const int cx = int(column >> 32), cz = int(uint32_t(column));
    for (auto& [coord, proxy] : proxies)
        if (levelOf(coord) == 0 &&
            std::abs(world.wrap().deltaChunk(coord.x, cx)) +
                    std::abs(int(coord.z) - cz) <=
                1)
            proxy->staleRevision = revision;
    // Force a new selection: the slab set of this column may have changed.
    chosenFor = nullptr;
}

void StructureLod::update(Vec3 eye, std::shared_ptr<const EditMap> edits) {
    snapshot = std::move(edits);
    collectStructures();
    planStructures(eye);
    requestStructures();
    collect();
    choose(eye);
    request();
}

void StructureLod::choose(Vec3 eye) {
    const std::array<int, 3> cell{int(eye.x) / 32, int(eye.y) / 32,
                                  int(eye.z) / 32};
    if (snapshot == chosenFor && voxels == voxelsChosenFor &&
        cell == chosenCell)
        return;
    chosenFor = snapshot;
    voxelsChosenFor = voxels;
    chosenCell = cell;
    struct Candidate {
        ChunkCoord coord;
        float distance;
    };
    std::vector<Candidate> candidates;
    std::unordered_set<ChunkCoord> listed;
    const auto consider = [&](ChunkCoord coord) {
        const auto base = baseOf(coord);
        const float span = float(spanOf(levelOf(coord))), half = span / 2;
        // The column copy nearest the eye (X wraps).
        const float x0 = drawTileX(base.x * span, span, eye.x),
                    z0 = float(base.z) * span;
        const float dx = std::max({x0 - eye.x, 0.f, eye.x - x0 - span});
        const float dz = std::max({z0 - eye.z, 0.f, eye.z - z0 - span});
        if (std::hypot(dx, dz) >= viewDistance) return;
        if (!listed.insert(coord).second) return;
        const float cy = base.y * span + half;
        candidates.push_back({coord, std::hypot(x0 + half - eye.x, cy - eye.y,
                                                z0 + half - eye.z)});
    };
    if (voxels)
        for (const auto& [coord, slab] : *voxels)
            if (!slab.enclosed) consider(coord);
    if (snapshot)
        for (const auto& [key, edit] : *snapshot) {
            // The column copy nearest the eye (X wraps).
            const float x0 = drawX(unsigned(key >> 32), eye.x),
                        z0 = float(uint32_t(key)) * Chunk::SIZE_Z;
            const float dx = std::max({x0 - eye.x, 0.f, eye.x - x0 - 16});
            const float dz = std::max({z0 - eye.z, 0.f, eye.z - z0 - 16});
            if (std::hypot(dx, dz) >= viewDistance) continue;
            for (const auto& [slabY, blob] : edit->structure)
                consider(ChunkCoord{unsigned(key >> 32), slabY,
                                    unsigned(uint32_t(key))});
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
            residentOf(*it->second) -= it->second->bytes;
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
        if (!slot) {
            slot = std::make_unique<Proxy>();
            slot->voxels = voxels && voxels->contains(coord);
        }
        auto& proxy = *slot;
        const bool current = proxy.drawable && !proxy.stale();
        const bool building =
            (proxy.cpu && proxy.cpuRevision >= proxy.staleRevision) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) {
                return j.slab == coord && j.revision >= proxy.staleRevision;
            });
        if (current || building) continue;
        // New proxies only while the budget has room; rebuilds always.
        if (!proxy.drawable && residentOf(proxy) >= budgetOf(proxy)) continue;
        auto future = pool.submit_task(
            [coord, level = levelOf(coord), edits = snapshot, merged = voxels,
             mesher = mesher, source = &world, definitions = &assets,
             shared = inputs] {
                SurfaceSampler ground(*source, *definitions, shared, edits);
                std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>> decoded;
                const auto structureChunk = [&](ChunkCoord c) -> const Chunk* {
                    auto [it, inserted] = decoded.try_emplace(c);
                    if (inserted) {
                        const ChunkBlob* blob = nullptr;
                        if (edits && level == 0)
                            if (const auto edit =
                                    edits->find(editKey(c.x, c.z));
                                edit != edits->end())
                                blob = edit->second->slab(uint16_t(c.y));
                        if (blob) {
                            it->second = std::make_unique<Chunk>();
                            chunk_codec::decode(**blob, *it->second);
                        }
                        // Structure voxels fill the air of the slab.
                        const VoxelSlab* w = nullptr;
                        if (merged)
                            if (const auto found = merged->find(c);
                                found != merged->end())
                                w = &found->second;
                        if (w) {
                            if (!it->second)
                                it->second = std::make_unique<Chunk>();
                            auto& chunk = *it->second;
                            for (int z = 0; z < Chunk::SIZE_Z; ++z)
                                for (int y = 0; y < Chunk::SIZE_Y; ++y)
                                    for (int x = 0; x < Chunk::SIZE_X; ++x) {
                                        if (!w->bits.test(
                                                size_t((z * Chunk::SIZE_Y + y) *
                                                           Chunk::SIZE_X +
                                                       x)))
                                            continue;
                                        Tile& tile = chunk.tiles[z][y][x];
                                        if (tile.isAir())
                                            tile.block_id = w->blockAt(
                                                size_t((z * Chunk::SIZE_Y + y) *
                                                           Chunk::SIZE_X +
                                                       x));
                                    }
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
                // Coarse levels: everything below is in cells of s tiles.
                const int s = 1 << level;
                const int width = ground.width() / s,
                          depth = (ground.depth() + s - 1) / s,
                          height = (WORLD_HEIGHT_Y + s - 1) / s;
                const auto* self = structureChunk(coord);
                if (!self) return ChunkMesh{};
                return mesher.build(
                    *self, baseOf(coord),
                    [&](int x, int y, int z) -> const Tile* {
                        if (width > 0) x = int(WorldWrap::mod(x, width));
                        if (x < 0 || z < 0 || y < 0 || z >= depth ||
                            y >= height)
                            return &air;
                        const ChunkCoord c =
                            withLevel({unsigned(x / Chunk::SIZE_X),
                                       unsigned(y / Chunk::SIZE_Y),
                                       unsigned(z / Chunk::SIZE_Z)},
                                      level);
                        if (const auto* chunk = structureChunk(c)) {
                            const auto& tile = chunk->tiles[z % Chunk::SIZE_Z]
                                                           [y % Chunk::SIZE_Y]
                                                           [x % Chunk::SIZE_X];
                            if (tile.isSolid()) return &tile;
                        }
                        // Faces resting on the (possibly edited) ground hide
                        // (a cell by the ground at its centre).
                        return y * s + s / 2 <= ground.height(x * s + s / 2,
                                                              z * s + s / 2)
                                   ? &solid
                                   : &air;
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
        if (!proxy.drawable && residentOf(proxy) + bytes > budgetOf(proxy))
            continue;
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
        residentOf(proxy) = residentOf(proxy) - proxy.bytes + bytes;
        proxy.bytes = bytes;
        const auto base = baseOf(coord);
        const float span = float(spanOf(levelOf(coord)));
        proxy.bounds = {float(base.x) * span,     float(base.y) * span,
                        float(base.z) * span,     float(base.x + 1) * span,
                        float(base.y + 1) * span, float(base.z + 1) * span};
        proxy.builtRevision = proxy.cpuRevision;
        proxy.drawable = true;
        proxy.cpu.reset();
        uploaded += bytes;
        if (uploaded >= limit) break;
    }
    return uploaded;
}

size_t StructureLod::backlog() const {
    // Structures whose planned slabs are not rasterized yet count too.
    const size_t pendingStructures = std::count_if(
        structures.begin(), structures.end(), [](const auto& entry) {
            const auto& s = entry.second;
            return s.wanted && (s.job || !s.pendingPlan.empty() || !s.built ||
                                s.built->revision != s.wanted->revision);
        });
    return pendingStructures +
           std::count_if(wanted.begin(), wanted.end(), [&](const auto& c) {
               const auto it = proxies.find(c);
               return it == proxies.end() || !it->second->drawable ||
                      it->second->stale();
           });
}
