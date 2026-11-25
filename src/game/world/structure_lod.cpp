#include "structure_lod.hpp"

#include <bx/math.h>

#include <cfloat>
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
    for (auto& [key, structure] : structures) {
        if (structure.job) structure.job->future.wait();
        if (structure.occupancyJob) structure.occupancyJob->wait();
    }
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

void StructureLod::planStructures(Vec3 eye, const TileBox& fullDetail) {
    using namespace std::chrono_literals;
    const std::array<int, 3> cell{int(eye.x) / 32, int(eye.y) / 32,
                                  int(eye.z) / 32};
    // Occupancy of new sources arrives from the pool.
    for (auto& [key, structure] : structures)
        if (structure.occupancyJob &&
            structure.occupancyJob->wait_for(0s) == std::future_status::ready) {
            try {
                structure.occupancy = structure.occupancyJob->get();
            } catch (const std::exception& e) {
                std::cerr << "Structure occupancy failed: " << e.what() << '\n';
                structure.occupancy = nullptr;
            }
            structure.occupancyJob.reset();
            replan = true;
        }
    if (!replan && cell == plannedCell && fullDetail == plannedDetail) return;
    replan = false;
    plannedCell = cell;
    plannedDetail = fullDetail;
    for (auto& [key, structure] : structures) {
        if (!structure.wanted || structure.job) continue;
        // Its occupancy first (once per source object, on the pool); the
        // built slabs stay drawn meanwhile.
        if (structure.occupancyFor != structure.wanted) {
            if (structure.occupancyJob) continue;  // for an older source
            structure.occupancyFor = structure.wanted;
            structure.occupancy = nullptr;
            structure.occupancyJob = pool.submit_task(
                [source = structure.wanted] { return source->occupancy(); },
                BS::pr::low);
            continue;
        }
        if (structure.occupancyJob) continue;
        // Same eye cell, source and built slabs as the last plan: the same
        // plan (its pending slabs, if any, are still listed).
        if (structure.plannedCell == cell &&
            structure.plannedDetail == fullDetail &&
            structure.plannedWanted == structure.wanted &&
            structure.plannedBuilt == structure.built)
            continue;
        structure.plannedCell = cell;
        structure.plannedDetail = fullDetail;
        structure.plannedWanted = structure.wanted;
        structure.plannedBuilt = structure.built;
        structure.pending.clear();
        structure.pendingPlan.clear();
        const auto& source = *structure.wanted;
        auto planned = structure_voxels::plan(
            source.bounds, eye.x, eye.y, eye.z, world.wrap(), structure.covered,
            structure.occupancy.get(), fullDetail, shadowDetail);
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
        auto plan = std::make_shared<const std::unordered_set<ChunkCoord>>(
            std::move(structure.pendingPlan));
        // The worker rasterizes the missing slabs and merges them with the
        // built ones still planned (a massive tree has thousands: copying
        // and re-marking them used to take milliseconds of the main thread).
        auto future = pool.submit_task(
            [source = structure.wanted, slabs = std::move(structure.pending),
             plan, keep = structure.pendingKeep, old = structure.slabs,
             wrap = world.wrap()] {
                Built out;
                auto next = std::make_shared<VoxelSlabs>();
                std::unordered_set<ChunkCoord> rebuilt;
                for (const auto& p : slabs) rebuilt.insert(p.key);
                if (keep && old) {
                    // Slabs still in the plan stay; rebuilt ones go.
                    for (const auto& [coord, slab] : *old)
                        if (plan->contains(coord) && !rebuilt.contains(coord))
                            next->emplace(coord, slab);
                        else
                            out.changed.push_back(coord);
                } else if (old) {
                    for (const auto& [coord, slab] : *old)
                        out.changed.push_back(coord);
                }
                for (const auto& p : slabs)
                    if (auto slab = source->rasterize(levelOf(p.key), p.sx,
                                                      p.sy, p.sz)) {
                        out.changed.push_back(p.key);
                        (*next)[p.key] = std::move(*slab);
                    }
                // Enclosure depends on the neighbours: slabs whose flag
                // flips change too.
                std::unordered_map<ChunkCoord, bool> before;
                for (const auto& [coord, slab] : *next)
                    before.emplace(coord, slab.enclosed);
                structure_voxels::markEnclosed(*next, wrap);
                for (const auto& [coord, slab] : *next)
                    if (before.at(coord) != slab.enclosed)
                        out.changed.push_back(coord);
                out.slabs = std::move(next);
                return out;
            },
            BS::pr::low);
        structure.job = Structure::Build{structure.wanted, std::move(plan),
                                         std::move(future)};
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
            std::optional<Built> built;
            try {
                built = job.future.get();
            } catch (const std::exception& e) {
                std::cerr << "Structure voxels failed: " << e.what() << '\n';
            }
            if (built) {
                changed.insert(changed.end(), built->changed.begin(),
                               built->changed.end());
                structure.slabs = std::move(built->slabs);
                structure.built = std::move(job.source);
                structure.covered = *job.plan;
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
    // One structure in view (the usual case): share its slabs as they are.
    std::vector<const Structure*> holding;
    for (const auto& [key, structure] : structures)
        if (structure.slabs && structure.wanted && !structure.slabs->empty())
            holding.push_back(&structure);
    if (holding.size() == 1) {
        voxels = holding.front()->slabs;
    } else {
        // Match the material precedence of the detailed chunk.
        std::sort(holding.begin(), holding.end(),
                  [](const auto* a, const auto* b) {
                      return a->wanted->key < b->wanted->key;
                  });
        auto merged = std::make_shared<VoxelSlabs>();
        bool hasShapes = false;
        for (const auto* structure : holding)
            for (const auto& [coord, slab] : *structure->slabs) {
                hasShapes |= !slab.shapes.empty() || !slab.pieces.empty();
                auto [it, inserted] = merged->try_emplace(coord, slab);
                if (!inserted) it->second.merge(slab);
            }
        if (hasShapes) structure_voxels::markEnclosed(*merged, world.wrap());
        voxels = merged->empty() ? nullptr : std::move(merged);
    }
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

void StructureLod::update(Vec3 eye, const TileBox& fullDetail,
                          std::shared_ptr<const EditMap> edits,
                          std::span<const TileBox> casterDetail) {
    if (!std::ranges::equal(shadowDetail, casterDetail)) {
        shadowDetail.assign(casterDetail.begin(), casterDetail.end());
        for (auto& [key, structure] : structures)
            structure.plannedWanted.reset();
        replan = true;
    }
    snapshot = std::move(edits);
    collectStructures();
    planStructures(eye, fullDetail);
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
        // Wanted again before its replacements were ready: back in use.
        if (const auto it = retired.find(c.coord);
            it != retired.end() && !proxies.contains(c.coord)) {
            retiredBytes -= it->second->bytes;
            residentOf(*it->second) += it->second->bytes;
            proxies.emplace(c.coord, std::move(it->second));
            retired.erase(it);
        }
    }
    // Removed structures and slabs out of view release their meshes. A
    // drawn structure slab replaced by another level (the eye moved) is
    // retired instead: drawn until its replacements are (see draw).
    for (auto it = proxies.begin(); it != proxies.end();)
        if (!keep.contains(it->first)) {
            auto& proxy = *it->second;
            residentOf(proxy) -= proxy.bytes;
            bool replaced = false;
            if (proxy.voxels && proxy.drawable && proxy.hasMesh && voxels)
                forEachOverlap(it->first, [&](const ChunkCoord& c) {
                    replaced = replaced || keep.contains(c);
                });
            if (replaced) {
                retiredBytes += proxy.bytes;
                retired[it->first] = std::move(it->second);
            }
            it = proxies.erase(it);
        } else
            ++it;
    // Retired proxies whose area left the view entirely go too.
    for (auto it = retired.begin(); it != retired.end();) {
        bool replaced = false;
        forEachOverlap(it->first, [&](const ChunkCoord& c) {
            replaced = replaced || keep.contains(c);
        });
        if (replaced) {
            ++it;
            continue;
        }
        retiredBytes -= it->second->bytes;
        it = retired.erase(it);
    }
}

bool StructureLod::coveredByChunks(const ChunkCoord& c) const {
    if (!chunkDrawn || levelOf(c) != 0) return false;
    const int inner = drawnRadius - DrawnMargin;
    const int64_t dx = world.wrap().enabled()
                           ? world.wrap().deltaChunk(c.x, drawnCentre.x)
                           : int64_t(c.x) - int64_t(drawnCentre.x);
    if (std::abs(dx) > inner ||
        std::abs(int(c.y) - int(drawnCentre.y)) > inner ||
        std::abs(int(c.z) - int(drawnCentre.z)) > inner || !chunkDrawn(c))
        return false;
    // Have an exact proxy ready before the loaded chunk stops casting.
    const TileBox box =
        structure_voxels::boxOf({c, int(c.x), int(c.y), int(c.z), 0});
    return std::none_of(shadowDetail.begin(), shadowDetail.end(),
                        [&](const TileBox& detail) {
                            return structure_voxels::intersectsWrapped(
                                box, detail, world.wrap());
                        });
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
        // Its real chunk is drawn, well inside the detail box: no proxy
        // needed until the player walks away (DrawnMargin chunks).
        if (!proxy.drawable && coveredByChunks(coord)) continue;
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
                            structure_voxels::stamp(*it->second, *w);
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
                auto mesh = mesher.build(
                    *self, baseOf(coord),
                    [&](int x, int y, int z) -> ChunkMesher::Neighbour {
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
                            const int lx = x % Chunk::SIZE_X,
                                      ly = y % Chunk::SIZE_Y,
                                      lz = z % Chunk::SIZE_Z;
                            const auto& tile = chunk->tiles[lz][ly][lx];
                            if (tile.isSolid()) return {*chunk, lx, ly, lz};
                        }
                        // Faces resting on the (possibly edited) ground hide
                        // (a cell by the ground at its centre). Carry the
                        // ground profile too: a thin wall over a partially
                        // occupied substrate cell exposes only the remainder.
                        const int groundX = x * s + s / 2;
                        const int groundZ = z * s + s / 2;
                        if (y * s + s / 2 <= ground.height(groundX, groundZ))
                            return {&solid,
                                    ground.groundShape(groundX, groundZ)};
                        return &air;
                    });
                const auto base = baseOf(coord);
                for (auto& v : mesh.vertices) {
                    // Probe just outside the face, in the rendered X image.
                    const int x = int(std::floor(
                        (base.x * Chunk::SIZE_X + v.x + .01f * v.nx) * s));
                    const int z = int(std::floor(
                        (base.z * Chunk::SIZE_Z + v.z + .01f * v.nz) * s));
                    const auto water = ground.waterAt(x, z);
                    v.u = -2.f - water.surfaceY;
                }
                return mesh;
            },
            BS::pr::low);
        jobs.push_back({coord, revision, std::move(future)});
    }
}

size_t StructureLod::upload(IRenderer& renderer, size_t limit,
                            const Hidden& hidden) {
    size_t uploaded = 0;
    for (const auto& coord : wanted) {
        auto it = proxies.find(coord);
        if (it == proxies.end() || !it->second->cpu) continue;
        auto& proxy = *it->second;
        const auto& cpu = *proxy.cpu;
        const size_t bytes = cpu.vertices.size() * sizeof(BlockVertex) +
                             cpu.indices.size() * sizeof(uint32_t);
        const bool coarse = levelOf(coord) > 0;
        // Coarse meshes reach the GPU merged (rebuildGroups), within the
        // same per-frame limit.
        if (!coarse && uploaded && uploaded + bytes > limit) break;
        if (!proxy.drawable && residentOf(proxy) + bytes > budgetOf(proxy))
            continue;
        if (coarse) {
            proxy.merged = std::move(proxy.cpu);
            proxy.mesh = MeshHandle{};
        } else {
            MeshHandle mesh;
            if (!cpu.vertices.empty()) {
                mesh.initDynamic<BlockVertex>(&renderer, cpu.vertices.size(),
                                              cpu.indices.size());
                const auto& gpu = std::get<DynamicMesh>(mesh.get());
                if (!bgfx::isValid(gpu.vbh) || !bgfx::isValid(gpu.ibh))
                    continue;
                mesh.update(cpu.vertices.data(), cpu.vertices.size(),
                            cpu.indices.data(), cpu.indices.size(), true);
            }
            // The previous mesh is released here, after its last draw.
            proxy.mesh = std::move(mesh);
            proxy.hasMesh = !cpu.vertices.empty();
            uploaded += bytes;
        }
        if (coarse) proxy.hasMesh = !proxy.merged->vertices.empty();
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
        if (!coarse && uploaded >= limit) break;
    }
    // Replacements ready: retired proxies go, and the groups holding either
    // are rebuilt in this same frame (no hole, no overlap).
    const bool released = releaseRetired(hidden);
    uploaded += rebuildGroups(renderer, limit > uploaded ? limit - uploaded : 0,
                              released);
    return uploaded;
}

size_t StructureLod::rebuildGroups(IRenderer& renderer, size_t limit,
                                   bool force) {
    struct Wanted {
        std::vector<std::pair<ChunkCoord, const Proxy*>> members;
    };
    std::unordered_map<ChunkCoord, Wanted> next;
    std::vector<ChunkCoord> order;  // nearest group first
    const auto add = [&](const ChunkCoord& coord, const Proxy& proxy) {
        const auto key = groupOf(coord);
        auto [it, inserted] = next.try_emplace(key);
        if (inserted) order.push_back(key);
        it->second.members.emplace_back(coord, &proxy);
    };
    for (const auto& coord : wanted) {
        if (levelOf(coord) == 0) continue;
        const auto it = proxies.find(coord);
        if (it == proxies.end()) continue;
        const auto& proxy = *it->second;
        if (proxy.drawable && proxy.hasMesh && proxy.merged &&
            !shadowedByRetired(coord))
            add(coord, proxy);
    }
    for (const auto& [coord, proxy] : retired)
        if (levelOf(coord) > 0 && proxy->hasMesh && proxy->merged)
            add(coord, *proxy);
    // Groups left with no member.
    for (auto it = groups.begin(); it != groups.end();)
        if (!next.contains(it->first))
            it = groups.erase(it);
        else
            ++it;
    size_t uploaded = 0;
    for (const auto& key : order) {
        auto& members = next.at(key).members;
        std::sort(members.begin(), members.end(),
                  [](const auto& a, const auto& b) {
                      return lessCoord(a.first, b.first);
                  });
        std::vector<ChunkCoord> coords;
        std::vector<uint64_t> revisions;
        size_t vertices = 0, indices = 0;
        for (const auto& [coord, proxy] : members) {
            coords.push_back(coord);
            revisions.push_back(proxy->builtRevision);
            vertices += proxy->merged->vertices.size();
            indices += proxy->merged->indices.size();
        }
        auto& group = groups[key];
        if (group.members == coords && group.revisions == revisions) continue;
        const size_t bytes =
            vertices * sizeof(BlockVertex) + indices * sizeof(uint32_t);
        if (!force && uploaded && uploaded + bytes > limit) continue;
        // Members in group-local cells: offset each slab's mesh.
        const auto groupBase = baseOf(key);
        ChunkMesh mesh;
        mesh.vertices.reserve(vertices);
        mesh.indices.reserve(indices);
        const int level = levelOf(key);
        const float span = float(spanOf(level));
        AABB bounds{FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto& [coord, proxy] : members) {
            const auto base = baseOf(coord);
            const float ox = float((base.x - groupBase.x * GroupSide) *
                                   Chunk::SIZE_X),
                        oy = float((base.y - groupBase.y * GroupSide) *
                                   Chunk::SIZE_Y),
                        oz = float((base.z - groupBase.z * GroupSide) *
                                   Chunk::SIZE_Z);
            const auto first = uint32_t(mesh.vertices.size());
            for (auto v : proxy->merged->vertices) {
                v.x += ox;
                v.y += oy;
                v.z += oz;
                mesh.vertices.push_back(v);
            }
            for (const auto i : proxy->merged->indices)
                mesh.indices.push_back(first + i);
            bounds.minX = std::min(bounds.minX, float(base.x) * span);
            bounds.minY = std::min(bounds.minY, float(base.y) * span);
            bounds.minZ = std::min(bounds.minZ, float(base.z) * span);
            bounds.maxX = std::max(bounds.maxX, float(base.x + 1) * span);
            bounds.maxY = std::max(bounds.maxY, float(base.y + 1) * span);
            bounds.maxZ = std::max(bounds.maxZ, float(base.z + 1) * span);
        }
        MeshHandle gpu;
        if (!mesh.vertices.empty()) {
            gpu.initDynamic<BlockVertex>(&renderer, mesh.vertices.size(),
                                         mesh.indices.size());
            const auto& handles = std::get<DynamicMesh>(gpu.get());
            // Out of handles: keep the old mesh, retry next frame.
            if (!bgfx::isValid(handles.vbh) || !bgfx::isValid(handles.ibh))
                continue;
            gpu.update(mesh.vertices.data(), mesh.vertices.size(),
                       mesh.indices.data(), mesh.indices.size(), true);
        }
        // The previous mesh is released here, after its last draw.
        group.mesh = std::move(gpu);
        group.hasMesh = !mesh.vertices.empty();
        group.members = std::move(coords);
        group.revisions = std::move(revisions);
        group.bounds = bounds;
        uploaded += bytes;
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
               if (it == proxies.end() || !it->second->drawable)
                   return !coveredByChunks(c);
               if (it->second->stale()) return true;
               // Coarse: drawn once in its group's mesh.
               return levelOf(c) > 0 && it->second->hasMesh && !inGroupMesh(c);
           });
}
