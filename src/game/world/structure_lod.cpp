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
      treeBudget(budget * 4),
      viewDistance(viewDistance) {}

StructureLod::~StructureLod() {
    for (auto& job : jobs) {
        try {
            job.future.get();
        } catch (const std::exception& e) {
            std::cerr << "Structure LOD shutdown: " << e.what() << '\n';
        }
    }
    for (auto& [key, tree] : trees)
        if (tree.job) tree.job->wait();
}

void StructureLod::chooseLevels(Vec3 eye) {
    // Full voxels near (where the real chunks take over), then cells of 2
    // and 4 tiles; 64 tiles of hysteresis.
    constexpr float Limits[MaxLevel] = {512, 1536};
    constexpr float Margin = 64;
    for (auto& [key, tree] : trees) {
        if (!tree.wanted.tree) continue;
        const auto& box = tree.wanted.tree->wood;
        const double cx = (box.x0 + box.x1) * 0.5;
        const double dx = std::max(
            0.0, std::abs(world.wrap().deltaX(cx, eye.x)) - box.sizeX() * 0.5);
        const double dy = std::max(
            0.0, std::abs((box.y0 + box.y1) * 0.5 - eye.y) - box.sizeY() * 0.5);
        const double dz = std::max(
            0.0, std::abs((box.z0 + box.z1) * 0.5 - eye.z) - box.sizeZ() * 0.5);
        const float d = float(std::sqrt(dx * dx + dy * dy + dz * dz));
        const int current =
            tree.builtLevel < 0 ? tree.wantedLevel : tree.builtLevel;
        int level = 0;
        while (level < MaxLevel &&
               d >= Limits[level] + (current > level ? -Margin : Margin))
            ++level;
        tree.wantedLevel = level;
        tree.distance = d;
    }
}

void StructureLod::setTrees(std::vector<TreeWoodSource> list) {
    std::unordered_set<uint64_t> seen;
    for (auto& source : list) {
        if (!source.tree) continue;
        seen.insert(source.tree->key);
        trees[source.tree->key].wanted = std::move(source);
    }
    for (auto& [key, tree] : trees)
        if (!seen.contains(key)) tree.wanted = {};
}

void StructureLod::collectTrees() {
    using namespace std::chrono_literals;
    std::vector<ChunkCoord> changed;
    const auto touched = [&](const std::shared_ptr<const WoodSlabs>& slabs) {
        if (slabs)
            for (const auto& [coord, slab] : *slabs) changed.push_back(coord);
    };
    for (auto it = trees.begin(); it != trees.end();) {
        auto& tree = it->second;
        if (tree.job && tree.job->wait_for(0s) == std::future_status::ready) {
            std::shared_ptr<const WoodSlabs> slabs;
            try {
                slabs = tree.job->get();
            } catch (const std::exception& e) {
                std::cerr << "Tree wood LOD failed: " << e.what() << '\n';
            }
            tree.job.reset();
            if (slabs) {
                touched(tree.slabs);
                touched(slabs);
                tree.slabs = std::move(slabs);
                tree.built = tree.building;
                tree.builtLevel = tree.buildingLevel;
            }
        }
        // Out of view: its slabs go once no job uses the entry.
        if (!tree.wanted.tree && !tree.job) {
            touched(tree.slabs);
            it = trees.erase(it);
            continue;
        }
        ++it;
    }
    if (!changed.empty()) mergeWood(changed);
}

void StructureLod::requestTrees() {
    size_t running =
        std::count_if(trees.begin(), trees.end(),
                      [](const auto& t) { return t.second.job.has_value(); });
    // Nearest trees first.
    std::vector<TreeWood*> order;
    for (auto& [key, tree] : trees)
        if (!tree.job && tree.wanted.tree &&
            !(tree.wanted == tree.built && tree.wantedLevel == tree.builtLevel))
            order.push_back(&tree);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) {
        return a->distance < b->distance;
    });
    for (auto* pending : order) {
        if (running >= MaxTreeJobs) break;
        auto& tree = *pending;
        tree.building = tree.wanted;
        tree.buildingLevel = tree.wantedLevel;
        tree.job = pool.submit_task(
            [source = tree.wanted, level = tree.wantedLevel,
             period = world.wrap().tiles] {
                return tree_wood_lod::rasterize(source, level, period);
            },
            BS::pr::low);
        ++running;
    }
}

void StructureLod::mergeWood(const std::vector<ChunkCoord>& changed) {
    auto merged = std::make_shared<WoodSlabs>();
    for (const auto& [key, tree] : trees)
        if (tree.slabs && tree.wanted.tree)
            for (const auto& [coord, slab] : *tree.slabs) {
                auto [it, inserted] = merged->try_emplace(coord, slab);
                if (!inserted) {
                    it->second.bits |= slab.bits;
                    it->second.enclosed |= slab.enclosed;
                }
            }
    wood = merged->empty() ? nullptr : std::move(merged);
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
                    const int64_t x =
                        world.wrap().enabled()
                            ? WorldWrap::mod(int64_t(base.x) + dx,
                                             world.wrap().chunks() >> level)
                            : int64_t(base.x) + dx;
                    stale.insert(withLevel(
                        ChunkCoord{unsigned(x), unsigned(int(base.y) + dy),
                                   unsigned(int(base.z) + dz)},
                        level));
                }
    ++revision;
    for (auto& [coord, proxy] : proxies)
        if (stale.contains(coord)) proxy->staleRevision = revision;
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
    collectTrees();
    chooseLevels(eye);
    requestTrees();
    collect();
    choose(eye);
    request();
}

void StructureLod::choose(Vec3 eye) {
    const std::array<int, 3> cell{int(eye.x) / 32, int(eye.y) / 32,
                                  int(eye.z) / 32};
    if (snapshot == chosenFor && wood == woodChosenFor && cell == chosenCell)
        return;
    chosenFor = snapshot;
    woodChosenFor = wood;
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
    if (wood)
        for (const auto& [coord, slab] : *wood)
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
            slot->tree = wood && wood->contains(coord);
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
            [coord, level = levelOf(coord), edits = snapshot, trees = wood,
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
                        // Tree wood fills the air of the slab.
                        const WoodSlab* w = nullptr;
                        if (trees)
                            if (const auto found = trees->find(c);
                                found != trees->end())
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
                                            tile.block_id = w->block;
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
    // Trees whose wood is not rasterized yet count too.
    const size_t pendingTrees =
        std::count_if(trees.begin(), trees.end(), [](const auto& t) {
            const auto& tree = t.second;
            return tree.wanted.tree && (!(tree.wanted == tree.built) ||
                                        tree.wantedLevel != tree.builtLevel);
        });
    return pendingTrees +
           std::count_if(wanted.begin(), wanted.end(), [&](const auto& c) {
               const auto it = proxies.find(c);
               return it == proxies.end() || !it->second->drawable ||
                      it->second->stale();
           });
}
