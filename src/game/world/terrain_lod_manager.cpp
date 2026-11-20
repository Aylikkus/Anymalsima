#include "terrain_lod_manager.hpp"

#include <bx/math.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string_view>

#include "light_volume_manager.hpp"

namespace {
std::array<int, 3> errorKey(const TerrainLod::Patch& p) {
    return {p.x, p.z, p.spacing};
}
// Offset (0 or a multiple of periodX) that moves a canonical patch to its
// copy nearest refX.
float patchShift(const TerrainLod::Patch& p, float refX, int periodX) {
    if (periodX <= 0) return 0;
    const double center = p.x + p.size() * .5;
    const double shift =
        WorldWrap::deltaf(center, refX, periodX) + refX - center;
    return float(std::round(shift / periodX) * periodX);
}
float horizontalDistance(const TerrainLod::Patch& p, Vec3 eye,
                         int periodX = 0) {
    const float x = p.x + patchShift(p, eye.x, periodX);
    return std::hypot(
        std::max({x - eye.x, 0.f, eye.x - x - p.size()}),
        std::max({float(p.z) - eye.z, 0.f, eye.z - p.z - p.size()}));
}
}  // namespace

TerrainLodManager::Entry::~Entry() {
    if (bgfx::isValid(materialMap)) bgfx::destroy(materialMap);
    if (bgfx::isValid(coverMap)) bgfx::destroy(coverMap);
    if (bgfx::isValid(waterMap)) bgfx::destroy(waterMap);
}
TerrainLodManager::TerrainLodManager(const World& world, IAssets& assets,
                                     ThreadPool& pool,
                                     const TerrainLod::Quality& quality,
                                     TerrainEdits* edits)
    : world(world),
      periodX(TerrainLod::wrapsExactly(world.wrap().tiles, quality.viewDistance)
                  ? world.wrap().tiles
                  : 0),
      assets(assets),
      pool(pool),
      quality(quality),
      edits(edits),
      inputs(std::make_shared<SurfaceInputCache>()),
      detailSampler(world, assets, inputs, edits ? edits->snapshot() : nullptr),
      detailCoverage(quality.chunkRadius),
      waterCoverage(quality.chunkRadius),
      rangeLimit(quality.viewDistance),
      selectionRange(quality.viewDistance) {
    if (world.reservoirs)
        waterConnection = world.reservoirs->changed.connect(
            [this](const ReservoirChange& change) {
                reservoirsChanged(change);
            });
    if (edits) {
        // Structures get their own cap next to the patch budget.
        structures = std::make_unique<StructureLod>(world, assets, pool, inputs,
                                                    quality.gpuBudget / 8,
                                                    quality.viewDistance);
        editConnection = edits->changed.connect(
            [this](uint64_t column) { editChanged(column); });
    }
    maxJobs = std::clamp<size_t>(pool.get_thread_count() / 2, 2, 6);
    textureArray.initTexture("u_texture_array", 0,
                             assets.getMaterialAlbedoArray());
    detailMap = bgfx::createTexture2D(
        DetailSize, DetailSize, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(detailMap))
        throw std::runtime_error(
            "Cannot create terrain detail coverage texture");
    detailUniform.initTexture("u_detail_map", 2, detailMap);
    shadowDetail.initTexture("u_detail_map", 0, detailMap);
    detailOrigin.initVec4("u_detail_origin", {0, 0, DetailSize * 16, 0});
    fogUniform.initVec4("u_world_eye", {0, 0, 0, quality.viewDistance});
    const char* debug = std::getenv("ANYMALSIMA_LOD_DEBUG_EDGES");
    debugEdges = debug && std::string_view(debug) == "1";
    water.initVec4("u_world_water", {0, 0, 0, 0});
    patchNormals.initTexture("u_normal_array", 3,
                             assets.getMaterialNormalArray());
    joinNormals.initTexture("u_normal_array", 1,
                            assets.getMaterialNormalArray());
    const uint8_t fullSky[4] = {255, 0, 0, 255};
    joinLightTexture = bgfx::createTexture3D(
        1, 1, 1, false, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        bgfx::copy(fullSky, sizeof(fullSky)));
    if (!bgfx::isValid(joinLightTexture))
        throw std::runtime_error("Cannot create LOD join light volume");
    joinLightVolume.initTexture("u_light_volume", 2, joinLightTexture);
    const uint8_t noCover = 0;
    joinCoverTexture = bgfx::createTexture3D(
        1, 1, 1, false, bgfx::TextureFormat::R8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        bgfx::copy(&noCover, 1));
    if (!bgfx::isValid(joinCoverTexture))
        throw std::runtime_error("Cannot create LOD join cover volume");
    joinCoverVolume.initTexture("u_cover_volume", 5, joinCoverTexture);
    joinLightChunk.initVec4("u_light_chunk", {0, 0, 0, 0});
    joinLightSlot.initVec4("u_light_slot", {0, 0, 0, 0});
}
TerrainLodManager::~TerrainLodManager() {
    if (world.reservoirs) world.reservoirs->changed.disconnect(waterConnection);
    if (edits) edits->changed.disconnect(editConnection);
    for (auto& job : jobs) job.cancel->store(true, std::memory_order_relaxed);
    for (auto& job : jobs) {
        try {
            job.future.get();
        } catch (const std::exception& e) {
            std::cerr << "LOD shutdown: " << e.what() << '\n';
        }
    }
    if (bgfx::isValid(detailMap)) bgfx::destroy(detailMap);
    if (bgfx::isValid(joinLightTexture)) bgfx::destroy(joinLightTexture);
    if (bgfx::isValid(joinCoverTexture)) bgfx::destroy(joinCoverTexture);
}

bool TerrainLodManager::wanted(const TerrainLod::Patch& p) const {
    return desiredSet.contains(p) || splitSet.contains(p) || roots.contains(p);
}
bool TerrainLodManager::isDrawable(const TerrainLod::Patch& p) const {
    const auto it = entries.find(p);
    return it != entries.end() && it->second->drawable;
}
bool TerrainLodManager::inView(const TerrainLod::Patch& p) const {
    // Same visibility rule as TerrainLod::select, at the selection eye, so
    // patches select() dropped never block their parent's replacement.
    return p.x < world.width * 128 && p.z < world.height * 128 &&
           horizontalDistance(p, selectionEye, periodX) < selectionRange;
}

bool TerrainLodManager::fallbackSatisfied(const TerrainLod::Patch& p) const {
    if (!splitSet.contains(p) || !drawableBelow.contains(p)) return false;
    const TerrainLod::Cover cover(
        splitSet, [&](const auto& q) { return inView(q); },
        [&](const auto& q) { return isDrawable(q); },
        [&](const auto& q) { return drawableBelow.contains(q); });
    return cover.coveredByDescendants(p);
}

void TerrainLodManager::choose(const Camera& camera, Vec2 viewport) {
    const Vec3 eye = camera.eye();
    const std::array<int, 4> focus{int(eye.x) / 32, int(eye.z) / 32,
                                   int(eye.y) / 32, int(viewport.y)};
    const bool moved = focus != lastFocus;
    // Texture and structure backlogs do not hold up terrain's next
    // refinement. Cached meshes outside the drawn layout are reclaimable.
    const bool roomToRefine =
        (errorScale > 1 || rangeLimit < quality.viewDistance) && [&] {
            size_t visible = 0;
            for (const auto& p : drawn) visible += entries.at(p)->bytes;
            return visible < budget() * 3 / 4;
        }();
    const bool terrainIdle =
        std::none_of(jobs.begin(), jobs.end(),
                     [](const Job& job) { return !job.refiningWater; });
    const bool refine = errorsChanged && terrainIdle && terrainBacklog() == 0;
    const bool sameSelection =
        errorScale == selectedScale && rangeLimit == selectedRangeLimit;
    if (!moved && !refine && sameSelection) return;
    if ((moved || refine) && roomToRefine && sameSelection) {
        if (rangeLimit < quality.viewDistance)
            rangeLimit = std::min(quality.viewDistance, rangeLimit / .8f);
        else
            errorScale = std::max(1.f, errorScale / 1.5f);
    }
    lastFocus = focus;
    errorsChanged = false;
    selectedScale = errorScale;
    selectionRange = rangeLimit;
    selectionEye = eye;
    const auto estimate = [&](const TerrainLod::Patch& p) {
        auto it = estimates.find(errorKey(p));
        // Until measured, assume relief at the sampling scale. The mesh stores
        // its midpoint error for subsequent decisions.
        return it == estimates.end() ? p.spacing * .4f
                                     : std::max(.5f, it->second.error);
    };
    float threshold = quality.pixelError * errorScale;
    std::vector<TerrainLod::Patch> layout;
    for (;;) {
        layout = TerrainLod::select(world.width * 128, world.height * 128, eye,
                                    viewport.y, estimate, false, threshold,
                                    quality.exactRadius, camera.position,
                                    selectionRange, periodX);
        const auto bytes = [&](const TerrainLod::Patch& p) -> size_t {
            if (const auto it = entries.find(p); it != entries.end()) {
                if (it->second->drawable) return it->second->bytes;
                if (it->second->cpu) return it->second->cpu->bytes();
            }
            const auto hint = estimates.find(errorKey(p));
            if (hint != estimates.end()) return hint->second.bytes;
            return p.spacing == 1 ? 320 * 1024 : 180 * 1024;
        };
        size_t expected = 0;
        std::set<TerrainLod::Patch> fallbackRoots;
        for (const auto& p : layout) {
            expected += bytes(p);
            if (p.spacing == TerrainLod::RootSpacing) continue;
            auto root = TerrainLod::parent(p);
            while (root.spacing < TerrainLod::RootSpacing)
                root = TerrainLod::parent(root);
            fallbackRoots.insert(root);
        }
        // Roots must coexist with their replacements until coverage is
        // complete.
        for (const auto& root : fallbackRoots)
            if (!fallbackSatisfied(root)) expected += bytes(root);
        if (expected <= budget() * 7 / 8) break;
        if (threshold < 512) {
            threshold *= 2;
        } else {
            if (selectionRange <=
                TerrainLod::RootSpacing * TerrainLod::Cells / 2)
                break;
            selectionRange =
                std::max(float(TerrainLod::RootSpacing * TerrainLod::Cells / 2),
                         selectionRange * .8f);
        }
    }
    rangeLimit = selectionRange;
    selectedRangeLimit = rangeLimit;
    selectionDirty = true;
    desired = std::move(layout);
    desiredSet = {desired.begin(), desired.end()};
    splitSet.clear();
    roots.clear();
    for (const auto& p : desired) {
        if (p.spacing >= TerrainLod::RootSpacing) {
            roots.insert(p);
            continue;
        }
        for (auto a = TerrainLod::parent(p); splitSet.insert(a).second;
             a = TerrainLod::parent(a))
            if (a.spacing >= TerrainLod::RootSpacing) {
                roots.insert(a);
                break;
            }
    }
    const auto near = [&](const auto& a, const auto& b) {
        const float ax = a.x + patchShift(a, eye.x, periodX),
                    bx = b.x + patchShift(b, eye.x, periodX);
        return std::hypot(ax + a.size() * .5f - eye.x,
                          a.z + a.size() * .5f - eye.z) <
               std::hypot(bx + b.size() * .5f - eye.x,
                          b.z + b.size() * .5f - eye.z);
    };
    queue.assign(roots.begin(), roots.end());
    for (const auto& p : desired)
        if (!roots.contains(p)) queue.push_back(p);
    for (const auto& p : splitSet)
        if (p.spacing < TerrainLod::RootSpacing) queue.push_back(p);
    // Finish the camera's fallback subtrees before the horizon consumes the
    // budget. Within each group build coarser owners before their detail.
    constexpr int rootSize = TerrainLod::RootSpacing * TerrainLod::Cells;
    const auto priority = [&](const TerrainLod::Patch& p) {
        const TerrainLod::Patch root{p.x / rootSize * rootSize,
                                     p.z / rootSize * rootSize,
                                     TerrainLod::RootSpacing};
        const bool foreground =
            horizontalDistance(root, camera.position, periodX) <
            quality.exactRadius;
        return std::pair{foreground                             ? 0
                         : p.spacing == TerrainLod::RootSpacing ? 1
                                                                : 2,
                         foreground ? -p.spacing : 0};
    };
    std::stable_sort(queue.begin(), queue.end(),
                     [&](const auto& a, const auto& b) {
                         const auto pa = priority(a), pb = priority(b);
                         return pa != pb ? pa < pb : near(a, b);
                     });
    for (auto& job : jobs)
        if (!wanted(job.patch) &&
            std::find(drawn.begin(), drawn.end(), job.patch) == drawn.end())
            job.cancel->store(true, std::memory_order_relaxed);
    // Unuploaded results of an older layout only cost memory. Drawable ones
    // stay cached until the GPU budget needs their room.
    for (auto it = entries.begin(); it != entries.end();)
        if (!it->second->drawable && !wanted(it->first))
            it = entries.erase(it);
        else
            ++it;
    // Error history is advisory and must not grow forever during travel.
    if (estimates.size() > 8192) estimates.clear();
}

void TerrainLodManager::markDrawable(const TerrainLod::Patch& p, int delta) {
    for (auto a = p; a.spacing < TerrainLod::RootSpacing;) {
        a = TerrainLod::parent(a);
        auto& count = drawableBelow[a];
        count += delta;
        if (count <= 0) drawableBelow.erase(a);
    }
}
void TerrainLodManager::erase(Entries::iterator it) {
    if (it->second->drawable) {
        if (it->second->lastDrawn == frame) selectionDirty = true;
        resident -= it->second->bytes;
        markDrawable(it->first, -1);
    }
    entries.erase(it);
}
std::optional<std::vector<TerrainLodManager::Entries::iterator>>
TerrainLodManager::roomFor(size_t bytes, const TerrainLod::Patch* incoming) {
    using TerrainLod::Patch;
    if (resident + bytes <= budget()) return std::vector<Entries::iterator>{};
    const auto inside = [](const Patch& inner, const Patch& outer) {
        return inner.spacing < outer.spacing && inner.x >= outer.x &&
               inner.z >= outer.z && inner.x < outer.x + outer.size() &&
               inner.z < outer.z + outer.size();
    };
    const auto replacedByCoarser = [&](const Patch& p) {
        return incoming && !splitSet.contains(*incoming) &&
               inside(p, *incoming);
    };
    const auto wasDrawn = [&](Entries::iterator it) {
        return it->second->lastDrawn != 0 && it->second->lastDrawn + 1 >= frame;
    };
    std::set<Patch> omitted;
    const TerrainLod::Cover cover(
        splitSet, [&](const Patch& p) { return inView(p); },
        [&](const Patch& p) {
            return (incoming && p == *incoming) ||
                   (!omitted.contains(p) && isDrawable(p));
        },
        [&](const Patch& p) {
            return drawableBelow.contains(p) ||
                   (incoming && inside(*incoming, p));
        });

    std::vector<Entries::iterator> candidates;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it->second->drawable) continue;
        const auto& p = it->first;
        if (replacedByCoarser(p) ||
            (splitSet.contains(p) && (incoming || !wasDrawn(it)) &&
             cover.coveredByDescendants(p)) ||
            (!wanted(p) && !wasDrawn(it)))
            candidates.push_back(it);
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](const auto& a, const auto& b) {
                  return std::pair{wasDrawn(a), a->second->lastDrawn} <
                         std::pair{wasDrawn(b), b->second->lastDrawn};
              });

    size_t remaining = resident;
    std::vector<Entries::iterator> victims;
    for (auto it : candidates) {
        omitted.insert(it->first);
        // An omitted child may have been keeping a coarse parent dispensable.
        // Validate the entire prospective transaction before evicting it.
        const auto covered = [&](Entries::iterator victim) {
            return !wasDrawn(victim) || !inView(victim->first) ||
                   replacedByCoarser(victim->first) ||
                   cover.coveredByDescendants(victim->first);
        };
        if (!covered(it) ||
            !std::all_of(victims.begin(), victims.end(), covered)) {
            omitted.erase(it->first);
            continue;
        }
        victims.push_back(it);
        remaining -= it->second->bytes;
        if (remaining + bytes <= budget()) return victims;
    }
    return std::nullopt;
}

bool TerrainLodManager::makeRoom(size_t bytes) {
    auto victims = roomFor(bytes);
    if (!victims) return false;
    for (auto it : *victims) erase(it);
    return true;
}
void TerrainLodManager::pressure() {
    if (errorScale != selectedScale || rangeLimit != selectedRangeLimit) return;
    if (errorScale < 32)
        errorScale = std::min(32.f, errorScale * 1.5f);
    else
        rangeLimit =
            std::max(float(TerrainLod::RootSpacing * TerrainLod::Cells / 2),
                     rangeLimit * .8f);
}

void TerrainLodManager::collect() {
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
            if (job.cancel->load()) continue;
            if (job.refiningWater) {
                const auto it = entries.find(job.patch);
                if (!mesh.waterLevels.empty() && it != entries.end() &&
                    it->second->drawable && it->second->provisionalWater &&
                    !it->second->stale)
                    it->second->waterReplacement = std::move(mesh);
                continue;
            }
            if (mesh.vertices.empty()) continue;
            const auto [hint, inserted] = estimates.try_emplace(
                errorKey(job.patch), Estimate{mesh.error, mesh.bytes()});
            if (inserted || hint->second.error != mesh.error)
                errorsChanged = true;
            hint->second = {mesh.error, mesh.bytes()};
            if (const auto it = entries.find(job.patch); it != entries.end()) {
                // Rebuilt after an edit: replaces the drawn mesh on upload.
                auto& entry = *it->second;
                if (entry.drawable && entry.stale &&
                    job.editRevision >= entry.staleRevision &&
                    (!entry.replacement ||
                     entry.replacementRevision < job.editRevision ||
                     !mesh.provisionalWater)) {
                    entry.stale = true;
                    entry.replacement = std::move(mesh);
                    entry.replacementRevision = job.editRevision;
                }
                continue;
            }
            if (!wanted(job.patch)) continue;
            auto entry = std::make_unique<Entry>();
            entry->cpu = std::move(mesh);
            entries.emplace(job.patch, std::move(entry));
        } catch (const std::exception& e) {
            // No pending flag is retained: a failed patch can be retried.
            std::cerr << "LOD generation failed: " << e.what() << '\n';
        }
    }
}
void TerrainLodManager::request() {
    const auto submit = [&](const TerrainLod::Patch& p, bool provisional,
                            bool refiningWater) {
        auto cancel = std::make_shared<std::atomic_bool>(false);
        auto future = pool.submit_task(
            [source = &world, definitions = &assets, shared = inputs, cancel, p,
             map = quality.materialMap, wrapX = periodX > 0, provisional,
             refiningWater, changes = edits ? edits->snapshot() : nullptr,
             reservoirs =
                 world.reservoirs ? world.reservoirs->snapshot() : nullptr] {
                const auto water = provisional
                                       ? SurfaceSampler::WaterDetail::Preview
                                       : SurfaceSampler::WaterDetail::Exact;
                SurfaceSampler sampler(*source, *definitions, shared, changes,
                                       water, reservoirs);
                return TerrainLod::build(p, sampler, *cancel, map, wrapX,
                                         refiningWater);
            },
            refiningWater ? BS::pr::low : BS::pr::normal);
        jobs.push_back({p, edits ? edits->revision() : 0, refiningWater,
                        std::move(cancel), std::move(future)});
    };
    auto terrainJobs =
        size_t(std::count_if(jobs.begin(), jobs.end(), [](const Job& job) {
            return !job.refiningWater;
        }));
    const auto staged =
        std::count_if(entries.begin(), entries.end(),
                      [](const auto& item) { return bool(item.second->cpu); });
    if (terrainJobs < maxJobs && staged < maxJobs) {
        std::vector<TerrainLod::Patch> order;
        for (const auto& p : drawn)
            if (entries.contains(p) && entries.at(p)->needsBuild())
                order.push_back(p);
        order.insert(order.end(), queue.begin(), queue.end());
        for (const auto& p : order) {
            if (terrainJobs >= maxJobs) break;
            const auto existing = entries.find(p);
            const bool rebuild =
                existing != entries.end() && existing->second->needsBuild();
            if ((existing != entries.end() && !rebuild) ||
                std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) {
                    return !j.refiningWater && j.patch == p &&
                           !j.cancel->load(std::memory_order_relaxed) &&
                           (!rebuild ||
                            j.editRevision >= existing->second->staleRevision);
                }))
                continue;
            if (!rebuild && fallbackSatisfied(p)) continue;
            const bool provisional =
                p.spacing > 1 &&
                (!rebuild || existing->second->provisionalWater);
            submit(p, provisional, false);
            ++terrainJobs;
        }
    }

    // One low-priority slot can refine visible water even while the terrain
    // queue is full. Prefer the camera's half of the view.
    if (std::any_of(jobs.begin(), jobs.end(),
                    [](const Job& job) { return job.refiningWater; }))
        return;
    const TerrainLod::Patch* next = nullptr;
    float best = std::numeric_limits<float>::infinity();
    for (const auto& p : drawn) {
        const auto& entry = *entries.at(p);
        if (entry.stale || !entry.provisionalWater || entry.waterReplacement)
            continue;
        const float dx = p.x + patchShift(p, selectionEye.x, periodX) +
                         p.size() * .5f - selectionEye.x;
        const float dz = p.z + p.size() * .5f - selectionEye.z;
        const float score =
            horizontalDistance(p, selectionEye, periodX) +
            (dx * viewForward.x + dz * viewForward.z < 0 ? quality.viewDistance
                                                         : 0);
        if (score < best) {
            best = score;
            next = &p;
        }
    }
    if (next) submit(*next, false, true);
}

void TerrainLodManager::update(const Camera& camera, Vec2 viewport,
                               std::span<const TileBox> casterDetail) {
    collect();
    const auto eye = camera.eye();
    viewForward = {camera.position.x - eye.x, 0, camera.position.z - eye.z};
    choose(camera, viewport);
    request();
    if (structures) {
        // Keep full voxels through the real chunk draw box and for two
        // slabs beyond it, so an approaching camera has time to refine.
        const int radius = quality.chunkRadius + 2;
        const int x = int(camera.position.x) / Chunk::SIZE_X;
        const int y = int(camera.position.y) / Chunk::SIZE_Y;
        const int z = int(camera.position.z) / Chunk::SIZE_Z;
        const TileBox detail{
            (x - radius) * Chunk::SIZE_X,     (y - radius) * Chunk::SIZE_Y,
            (z - radius) * Chunk::SIZE_Z,     (x + radius + 1) * Chunk::SIZE_X,
            (y + radius + 1) * Chunk::SIZE_Y, (z + radius + 1) * Chunk::SIZE_Z};
        structures->update(camera.eye(), detail, edits->snapshot(),
                           casterDetail);
    }
}
void TerrainLodManager::upload(IRenderer& renderer) {
    for (const auto& p : queue) {
        if (uploaded >= quality.uploadBudget) break;
        auto it = entries.find(p);
        if (it == entries.end() || it->second->drawable || !it->second->cpu)
            continue;
        auto& entry = *it->second;
        const auto& cpu = *entry.cpu;
        if (fallbackSatisfied(p)) {
            entries.erase(it);
            continue;
        }
        const auto bytes = cpu.bytes();
        // One oversized mesh may use a whole frame rather than never fit.
        if (uploaded && uploaded + bytes > quality.uploadBudget) continue;
        auto victims = roomFor(bytes, &p);
        if (!victims) {
            pressure();
            continue;
        }
        auto fresh = std::make_unique<Entry>();
        if (!createGpu(*fresh, p, cpu, renderer)) {
            pressure();
            continue;
        }
        for (auto victim : *victims) erase(victim);
        fresh->bounds = cpu.bounds;
        fresh->waterBounds = cpu.water.bounds;
        fresh->bands = cpu.bands;
        fresh->bytes = bytes;
        fresh->drawable = true;
        fresh->provisionalWater = cpu.provisionalWater;
        it->second = std::move(fresh);
        markDrawable(p, 1);
        uploaded += bytes;
        resident += bytes;
    }
}

bool TerrainLodManager::createGpu(Entry& entry, const TerrainLod::Patch& p,
                                  const TerrainLod::Mesh& cpu,
                                  IRenderer& renderer) {
    entry.mesh.initDynamic<BlockVertex>(&renderer, cpu.vertices.size(),
                                        cpu.indices.size());
    const auto& gpu = std::get<DynamicMesh>(entry.mesh.get());
    if (!bgfx::isValid(gpu.vbh) || !bgfx::isValid(gpu.ibh)) {
        entry.mesh.release();
        return false;
    }
    entry.mesh.update(cpu.vertices.data(), cpu.vertices.size(),
                      cpu.indices.data(), cpu.indices.size(), true);
    entry.materialMap = bgfx::createTexture2D(
        cpu.mapSize, cpu.mapSize, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(cpu.materials.data(), cpu.materials.size()));
    if (!bgfx::isValid(entry.materialMap)) {
        entry.mesh.release();
        return false;
    }
    entry.coverMap = bgfx::createTexture2D(
        uint16_t(cpu.coverSize), uint16_t(cpu.coverSize), false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(cpu.covers.data(), uint32_t(cpu.covers.size())));
    if (!bgfx::isValid(entry.coverMap)) {
        entry.mesh.release();
        bgfx::destroy(entry.materialMap);
        entry.materialMap = BGFX_INVALID_HANDLE;
        return false;
    }
    if (!createWater(entry, cpu, renderer)) return false;
    entry.mapUniform.initTexture("u_surface_map", 1, entry.materialMap);
    entry.coverUniform.initTexture("u_cover_map", CoverStage, entry.coverMap);
    entry.coverInfo.initVec4("u_lod_cover", {float(cpu.coverSize), 0, 0, 0});
    entry.patchUniform.initVec4(
        "u_lod_patch",
        {float(p.x), float(p.z), float(p.size()), float(p.spacing)});
    entry.shiftUniform.initVec4("u_lod_shift", {0, 0, 0, 0});
    return true;
}

bool TerrainLodManager::createWater(Entry& entry, const TerrainLod::Mesh& cpu,
                                    IRenderer& renderer) {
    MeshHandle mesh;
    if (!cpu.water.vertices.empty()) {
        mesh.initDynamic<BlockVertex>(&renderer, cpu.water.vertices.size(),
                                      cpu.water.indices.size());
        if (!mesh.drawable()) return false;
        mesh.update(cpu.water.vertices.data(), cpu.water.vertices.size(),
                    cpu.water.indices.data(), cpu.water.indices.size(), true);
    }
    const auto texture = bgfx::createTexture2D(
        cpu.waterMapSize, cpu.waterMapSize, false, 1, bgfx::TextureFormat::RG8,
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(cpu.waterLevels.data(), cpu.waterLevels.size()));
    if (!bgfx::isValid(texture)) return false;
    if (bgfx::isValid(entry.waterMap)) bgfx::destroy(entry.waterMap);
    entry.waterMap = texture;
    entry.waterMapUniform.initTexture("u_patch_water", 8, texture);
    entry.waterMesh = std::move(mesh);
    entry.waterBounds = cpu.water.bounds;
    entry.waterBytes = cpu.water.bytes() + cpu.waterLevels.size();
    return true;
}

void TerrainLodManager::uploadWater(IRenderer& renderer) {
    for (const auto& p : drawn) {
        auto& entry = *entries.at(p);
        if (entry.stale || !entry.waterReplacement) continue;
        const auto& cpu = *entry.waterReplacement;
        const size_t bytes = cpu.water.bytes() + cpu.waterLevels.size();
        if (uploaded && uploaded + bytes > quality.uploadBudget) continue;
        const size_t previous = entry.waterBytes;
        if (bytes > previous && !makeRoom(bytes - previous)) continue;
        if (!createWater(entry, cpu, renderer)) continue;
        entry.bytes = entry.bytes - previous + bytes;
        resident = resident - previous + bytes;
        uploaded += bytes;
        entry.provisionalWater = false;
        entry.waterReplacement.reset();
        break;
    }
}

void TerrainLodManager::uploadReplacements(IRenderer& renderer) {
    using TerrainLod::Patch;
    std::set<Patch> visible;
    for (const auto& p : drawn)
        if (entries.at(p)->stale) visible.insert(p);
    const auto publish = [&](const std::vector<Patch>& group) {
        uint64_t revision = 0;
        for (const auto& p : group)
            revision = std::max(revision, entries.at(p)->staleRevision);
        for (const auto& p : group) entries.at(p)->staleRevision = revision;
        size_t bytes = 0, previousBytes = 0;
        for (const auto& p : group) {
            const auto& entry = *entries.at(p);
            if (entry.needsBuild()) return;
            bytes += entry.replacement->bytes();
            previousBytes += entry.bytes;
        }
        // A visible edge switches in one frame. Like a single oversized
        // patch, a group may use the whole frame if it exceeds the budget.
        if (uploaded && uploaded + bytes > quality.uploadBudget) return;
        if (bytes > previousBytes && !makeRoom(bytes - previousBytes)) {
            pressure();
            return;
        }
        std::vector<std::unique_ptr<Entry>> ready;
        for (const auto& p : group) {
            const auto it = entries.find(p);
            if (it == entries.end()) return;
            const auto& old = *it->second;
            const auto& cpu = *old.replacement;
            auto fresh = std::make_unique<Entry>();
            if (!createGpu(*fresh, p, cpu, renderer)) return;
            fresh->bounds = cpu.bounds;
            fresh->waterBounds = cpu.water.bounds;
            fresh->bands = cpu.bands;
            fresh->bytes = cpu.bytes();
            fresh->drawable = true;
            fresh->provisionalWater = cpu.provisionalWater;
            fresh->lastDrawn = old.lastDrawn;
            ready.push_back(std::move(fresh));
        }
        // Every buffer in the group now exists. Both sides of each visible
        // edge (including an X seam) can adopt the new snapshot together.
        for (size_t i = 0; i < group.size(); ++i)
            entries.at(group[i]) = std::move(ready[i]);
        uploaded += bytes;
        resident = resident - previousBytes + bytes;
    };
    while (!visible.empty() && uploaded < quality.uploadBudget) {
        std::vector<Patch> group{*visible.begin()};
        visible.erase(visible.begin());
        for (size_t i = 0; i < group.size(); ++i)
            for (auto it = visible.begin(); it != visible.end();) {
                if (TerrainLod::shareEdge(group[i], *it, periodX)) {
                    group.push_back(*it);
                    it = visible.erase(it);
                } else {
                    ++it;
                }
            }
        publish(group);
    }
    // Cached, invisible meshes cannot block an edge in the presentation.
    for (const auto& p : queue) {
        if (uploaded >= quality.uploadBudget) break;
        const auto it = entries.find(p);
        if (it != entries.end() && it->second->drawable && it->second->stale &&
            it->second->lastDrawn != frame)
            publish({p});
    }
}

void TerrainLodManager::editChanged(uint64_t column) {
    const int x0 = int(column >> 32) * Chunk::SIZE_X - 1,
              z0 = int(uint32_t(column)) * Chunk::SIZE_Z - 1;
    const int x1 = x0 + Chunk::SIZE_X + 2, z1 = z0 + Chunk::SIZE_Z + 2;
    const auto touches = [&](const TerrainLod::Patch& p) {
        // Normals and skirts sample up to one spacing outside the patch.
        const auto at = [&](int shift) {
            return p.x - p.spacing < x1 + shift &&
                   p.x + p.size() + p.spacing > x0 + shift &&
                   p.z - p.spacing < z1 && p.z + p.size() + p.spacing > z0;
        };
        return at(0) || (periodX > 0 && (at(periodX) || at(-periodX)));
    };
    invalidatePatches(touches);
    detailSampler.setEdits(edits->snapshot());
    if (structures) structures->edited(column);
    const TerrainLod::DetailCoverage::Key key{int(column >> 32),
                                              int(uint32_t(column))};
    surfaceBands.erase(key);
    waterCoverage.remove(key);
    joinedCoverageVersion = UINT64_MAX;
}

void TerrainLodManager::invalidatePatches(
    const std::function<bool(const TerrainLod::Patch&)>& touches) {
    const auto revision = edits ? edits->revision() : 0;
    for (auto& job : jobs)
        if (touches(job.patch))
            job.cancel->store(true, std::memory_order_relaxed);
    for (auto it = entries.begin(); it != entries.end();) {
        if (!touches(it->first)) {
            ++it;
            continue;
        }
        if (!it->second->drawable) {
            // A result waiting for upload is outdated: build it again.
            it = entries.erase(it);
            continue;
        }
        it->second->stale = true;
        it->second->waterReplacement.reset();
        it->second->replacement.reset();
        it->second->staleRevision = revision;
        ++it;
    }
}

void TerrainLodManager::reservoirsChanged(const ReservoirChange& change) {
    detailSampler.setReservoirs(world.reservoirs->snapshot());
    invalidatePatches([&](const TerrainLod::Patch& p) {
        return change.affects(world.hydrology.get(), p.x - p.spacing,
                              p.z - p.spacing, p.x + p.size() + p.spacing,
                              p.z + p.size() + p.spacing, periodX);
    });
    for (auto it = surfaceBands.begin(); it != surfaceBands.end();) {
        const auto key = it->first;
        const int x = key.first * Chunk::SIZE_X, z = key.second * Chunk::SIZE_Z;
        if (!change.affects(world.hydrology.get(), x - 1, z - 1,
                            x + Chunk::SIZE_X + 1, z + Chunk::SIZE_Z + 1,
                            periodX)) {
            ++it;
            continue;
        }
        waterCoverage.remove(key);
        it = surfaceBands.erase(it);
    }
}

void TerrainLodManager::selectDrawn() {
    const auto previous = drawn;
    drawn.clear();
    const TerrainLod::Cover cover(
        splitSet, [&](const auto& p) { return inView(p); },
        [&](const auto& p) { return isDrawable(p); },
        [&](const auto& p) { return drawableBelow.contains(p); });
    for (const auto& root : roots) cover.select(root, drawn);
    for (const auto& p : drawn) entries.at(p)->lastDrawn = frame;
    if (drawn != previous) ++drawnRevision;
}

bool TerrainLodManager::drawsColumn(unsigned x, unsigned z) const {
    return detailCoverage.contains(int(x), int(z));
}
bool TerrainLodManager::drawsChunk(const ChunkCoord& c) const {
    if (std::abs(world.wrap().deltaChunk(c.x, detailCentre.x)) >
            quality.chunkRadius ||
        std::abs(int(c.y) - int(detailCentre.y)) > quality.chunkRadius ||
        std::abs(int(c.z) - int(detailCentre.z)) > quality.chunkRadius)
        return false;
    if (detailCoverage.contains(int(c.x), int(c.z))) return true;
    // Above the column's whole surface band distant terrain draws nothing
    // (structure voxels hand over per chunk), so the chunk can be drawn
    // while its ground is out of the detail range: the wood of a massive
    // tree seen from high above.
    const auto band = surfaceBands.find({int(c.x), int(c.z)});
    return band != surfaceBands.end() && int(c.y) > band->second.ground.maxY;
}

bool TerrainLodManager::drawsWater(const ChunkCoord& c) const {
    const auto it = waterCoverage.columns().find({int(c.x), int(c.z)});
    return it != waterCoverage.columns().end() && int(c.y) >= it->second.minY &&
           int(c.y) <= it->second.maxY;
}
void TerrainLodManager::updateCoverage(ChunkManager& chunks,
                                       const Camera& camera,
                                       LightVolumeManager* lightVolumes) {
    const int cx = int(camera.position.x) / 16,
              cy = int(camera.position.y) / 16,
              cz = int(camera.position.z) / 16;
    detailCentre = {unsigned(cx), unsigned(cy), unsigned(cz)};
    const int keep = detailCoverage.keepRadius();
    const WorldWrap wrap{periodX};
    // Chunk-column distance in X, across the seam when the world wraps.
    const auto distX = [&](int x) {
        return int(std::abs(wrap.deltaChunk(x, cx)));
    };
    for (auto it = surfaceBands.begin(); it != surfaceBands.end();)
        if (distX(it->first.first) > keep + 1 ||
            std::abs(it->first.second - cz) > keep + 1)
            it = surfaceBands.erase(it);
        else
            ++it;
    // Exact LOD results provide cheap initial summaries, but do not own the
    // summaries or the visibility of chunks after handover.
    for (const auto& p : drawn) {
        const auto it = entries.find(p);
        if (p.spacing != 1 || it == entries.end() || it->second->stale)
            continue;
        for (const auto& bands : it->second->bands) {
            const auto& band = bands.ground;
            if (distX(band.x) <= keep + 1 && std::abs(band.z - cz) <= keep + 1)
                surfaceBands.try_emplace({band.x, band.z}, bands);
        }
    }
    int newBands = 0;
    chunks.forEachMeshed([&](const ChunkCoord& coord) {
        const int x = int(coord.x), z = int(coord.z);
        if (newBands >= 4 || distX(x) > detailCoverage.enterRadius() ||
            std::abs(z - cz) > detailCoverage.enterRadius() ||
            surfaceBands.contains({x, z}) || !chunks.isMeshUploaded(coord))
            return;
        surfaceBands[{x, z}] = TerrainLod::sampleBands(x, z, detailSampler);
        ++newBands;
    });
    std::vector<TerrainLod::SurfaceBand> candidates, waterCandidates;
    for (const auto& [key, bands] : surfaceBands) {
        candidates.push_back(bands.ground);
        if (bands.water) waterCandidates.push_back(bands.water->slabs);
    }
    const auto lit = [&](ChunkCoord coord) {
        return !lightVolumes ||
               (!chunks.getMesh(coord) && !chunks.getWaterMesh(coord)) ||
               lightVolumes->prepare(coord);
    };
    const auto current = [&](ChunkCoord coord) {
        if (!chunks.isMeshUploaded(coord)) return false;
        const int dx[6] = {-1, 1, 0, 0, 0, 0}, dy[6] = {0, 0, -1, 1, 0, 0},
                  dz[6] = {0, 0, 0, 0, -1, 1};
        for (int i = 0; i < 6; ++i) {
            const int x = int(wrap.wrapChunk(int(coord.x) + dx[i])),
                      y = int(coord.y) + dy[i], z = int(coord.z) + dz[i];
            if (x < 0 || z < 0 || y < 0 || x >= world.width * 8 ||
                z >= world.height * 8 || y >= Y_CHUNKS)
                continue;
            if (!chunks.isReady({unsigned(x), unsigned(y), unsigned(z)}))
                return false;
        }
        return lit(coord);
    };
    detailCoverage.update(
        detailCentre, candidates, current,
        [&](ChunkCoord coord) {
            return chunks.hasUploadedMesh(coord) && lit(coord);
        },
        wrap.chunks());

    const auto waterDrawable = [&](ChunkCoord coord) {
        if (!chunks.hasUploadedMesh(coord)) return false;
        const auto it = surfaceBands.find({int(coord.x), int(coord.z)});
        if (it == surfaceBands.end() || !it->second.water ||
            int(coord.y) < it->second.water->slabs.minY ||
            int(coord.y) > it->second.water->slabs.maxY)
            return false;
        return it->second.water->matches(coord,
                                         chunks.uploadedWaterTops(coord)) &&
               lit(coord);
    };
    const auto waterCurrent = [&](ChunkCoord coord) {
        const int x = int(coord.x) * Chunk::SIZE_X,
                  z = int(coord.z) * Chunk::SIZE_Z;
        // Retain a drawable fallback even when the camera sees the water
        // slab from above the range of the underlying terrain band.
        for (int spacing = 1; spacing <= TerrainLod::RootSpacing;
             spacing *= 2) {
            const int side = TerrainLod::Cells * spacing;
            const auto p =
                entries.find({x / side * side, z / side * side, spacing});
            if (p != entries.end() && p->second->lastDrawn == frame)
                return !p->second->stale && chunks.isMeshUploaded(coord) &&
                       waterDrawable(coord);
        }
        return false;
    };
    waterCoverage.update(detailCentre, waterCandidates, waterCurrent,
                         waterDrawable, wrap.chunks());
    detailX = cx - DetailSize / 2;
    detailZ = cz - DetailSize / 2;
}
void TerrainLodManager::uploadCoverage() {
    if (groundCoverageUploaded == detailCoverage.version() &&
        waterCoverageUploaded == waterCoverage.version() &&
        uploadedDetailX == detailX && uploadedDetailZ == detailZ)
        return;
    groundCoverageUploaded = detailCoverage.version();
    waterCoverageUploaded = waterCoverage.version();
    uploadedDetailX = detailX;
    uploadedDetailZ = detailZ;
    coverage.fill(0);
    // The window is in the camera's frame; columns are canonical.
    const WorldWrap wrap{periodX};
    const int cx = detailX + DetailSize / 2;
    for (const auto& [key, band] : detailCoverage.columns()) {
        const int ix = cx + int(wrap.deltaChunk(band.x, cx)) - detailX,
                  iz = band.z - detailZ;
        if (ix >= 0 && iz >= 0 && ix < DetailSize && iz < DetailSize)
            coverage[(iz * DetailSize + ix) * 4] = 255;
    }
    for (const auto& [key, band] : waterCoverage.columns()) {
        const int ix = cx + int(wrap.deltaChunk(band.x, cx)) - detailX,
                  iz = band.z - detailZ;
        if (ix >= 0 && iz >= 0 && ix < DetailSize && iz < DetailSize)
            coverage[(iz * DetailSize + ix) * 4 + 1] = 255;
    }
    bgfx::updateTexture2D(detailMap, 0, 0, 0, 0, DetailSize, DetailSize,
                          bgfx::copy(coverage.data(), coverage.size()));
    detailOrigin.setVec4(
        {float(detailX * 16), float(detailZ * 16), DetailSize * 16, 0});
}
void TerrainLodManager::uploadJoins(IRenderer& renderer) {
    // With at most a 15x15 column window, even a checkerboard of visible
    // columns has fewer than 2 MiB of indexed one-tile boundary quads.
    constexpr size_t side = TerrainLod::MaxChunkRadius * 2 - 1;
    constexpr size_t quads = 2 * side * (side + 1) * Chunk::SIZE_X;
    static_assert(quads * (4 * sizeof(BlockVertex) + 6 * sizeof(uint32_t)) <=
                  JoinBudget);
    if (!joinsInitialised) {
        detailJoins.initDynamic<BlockVertex>(&renderer, quads * 4, quads * 6);
        const auto& gpu = std::get<DynamicMesh>(detailJoins.get());
        if (!bgfx::isValid(gpu.vbh) || !bgfx::isValid(gpu.ibh))
            throw std::runtime_error("Cannot create terrain detail joins");
        joinsInitialised = true;
    }
    // Only patches inside the detail window can touch a detailed column, so
    // distant uploads do not rebuild the joins.
    const int x0 = detailX * 16, z0 = detailZ * 16, x1 = x0 + DetailSize * 16,
              z1 = z0 + DetailSize * 16;
    const WorldWrap wrap{periodX};
    const int cx = detailX + DetailSize / 2;
    std::vector<TerrainLod::Patch> layout;
    for (const auto& p : drawn) {
        const float px = p.x + patchShift(p, cx * 16.f + 8, periodX);
        if (px < x1 && px + p.size() > x0 && p.z < z1 && p.z + p.size() > z0)
            layout.push_back(p);
    }
    if (joinedCoverageVersion == detailCoverage.version() &&
        layout == joinedLayout)
        return;
    std::vector<TerrainLod::SurfaceBand> columns;
    // Joins are built in the camera's frame, so columns across the seam
    // sit next to the camera.
    for (auto band : detailCoverage.columns() | std::views::values) {
        band.x = cx + int(wrap.deltaChunk(band.x, cx));
        columns.push_back(band);
    }
    const auto mesh = TerrainLod::joinDetails(
        layout, columns, detailSampler,
        [&](int x, int z) {
            return detailCoverage.contains(int(wrap.wrapChunk(x)), z);
        },
        periodX);
    if (mesh.bytes() > JoinBudget)
        throw std::runtime_error("Terrain detail joins exceed bounded window");
    // Joins always follow this frame's patches; their bounded size is not
    // deferred by the per-frame patch budget.
    joinsDrawable = !mesh.vertices.empty();
    if (joinsDrawable)
        detailJoins.update(mesh.vertices.data(), mesh.vertices.size(),
                           mesh.indices.data(), mesh.indices.size(), true);
    uploaded += mesh.bytes();
    ++drawnRevision;
    joinedLayout = std::move(layout);
    joinedCoverageVersion = detailCoverage.version();
}
void TerrainLodManager::render(RenderTarget& target, IRenderer& renderer,
                               ChunkManager& chunks, const Camera& camera,
                               Vec2 viewport,
                               LightVolumeManager* lightVolumes) {
    uploaded = 0;
    if (selectionDirty) {
        selectDrawn();
        selectionDirty = false;
    } else {
        for (const auto& p : drawn) entries.at(p)->lastDrawn = frame;
    }
    uploadReplacements(renderer);
    const size_t replaced = uploaded;
    upload(renderer);
    if (uploaded != replaced) {
        selectDrawn();
        selectionDirty = false;
    }
    // Water-only updates leave the ground shadow casters unchanged.
    if (uploaded) ++drawnRevision;
    uploadWater(renderer);
    updateCoverage(chunks, camera, lightVolumes);
    if (structures) {
        // The same hand-over to real chunks as the draw below.
        const size_t bytes = structures->upload(
            renderer,
            quality.uploadBudget > uploaded ? quality.uploadBudget - uploaded
                                            : 0,
            [&](const ChunkCoord& c) {
                return drawsChunk(c) && chunks.hasUploadedMesh(c);
            });
        uploaded += bytes;
        if (bytes) ++drawnRevision;
    }
    uploadJoins(renderer);
    uploadCoverage();
    const auto eye = camera.eye();
    fogUniform.setVec4({eye.x, eye.y, eye.z, selectionRange});
    const int x = int(std::floor(eye.x)), y = int(std::floor(eye.y)),
              z = int(std::floor(eye.z));
    underwater = false;
    if (const Tile* tile = chunks.tileAt(x, y, z)) {
        underwater = WaterSurface{}.submerged(eye.y, -1, tile);
    } else if (y >= 0 && z >= 0 && z < detailSampler.depth() &&
               (periodX > 0 || (x >= 0 && x < detailSampler.width()))) {
        underwater = detailSampler.submerged(x, z, eye.y);
    }
    static const auto flowStart = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - flowStart)
                               .count();
    water.setVec4(
        {float(elapsed), underwater ? 1.f : 0.f, debugEdges ? 1.f : 0.f, 0});
    float view[16], proj[16], vp[16];
    bx::mtxLookAt(view, {eye.x, eye.y, eye.z},
                  {camera.position.x, camera.position.y, camera.position.z},
                  {0, 1, 0});
    bx::mtxProj(proj, 60.f, viewport.x / std::max(1.f, viewport.y), 1.f,
                selectionRange + 512, bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(vp, view, proj);
    const auto frustum = Frustum::fromViewProj(vp);
    for (const auto& p : drawn) {
        auto& entry = *entries.at(p);
        // Each patch is drawn at its copy nearest the eye (X wraps).
        const float shift = patchShift(p, eye.x, periodX);
        AABB bounds = entry.bounds;
        bounds.minX += shift;
        bounds.maxX += shift;
        AABB waterBounds = entry.waterBounds;
        waterBounds.minX += shift;
        waterBounds.maxX += shift;
        const bool groundVisible = frustum.intersects(bounds);
        const bool waterVisible =
            entry.waterMesh.drawable() && frustum.intersects(waterBounds);
        if (!groundVisible && !waterVisible) continue;
        // World-space vertices, placed relative to the eye.
        auto transform = relativeTransform(camera, shift, 0, 0);
        entry.patchUniform.setVec4({float(p.x) + shift, float(p.z),
                                    float(p.size()), float(p.spacing)});
        entry.shiftUniform.setVec4({shift, 0, 0, 0});
        std::vector<ShaderProperty*> uniforms{
            &textureArray,    &entry.mapUniform,      &entry.patchUniform,
            &detailUniform,   &detailOrigin,          &fogUniform,
            &patchNormals,    &entry.shiftUniform,    &entry.coverUniform,
            &entry.coverInfo, &entry.waterMapUniform, &water};
        uniforms.insert(uniforms.end(), extraUniforms.begin(),
                        extraUniforms.end());
        if (groundVisible)
            target.drawVertexBuffer(entry.mesh.get(), &shader,
                                    std::vector(uniforms), false,
                                    std::array(transform), DrawLayer::GAME);
        if (waterVisible)
            target.drawVertexBuffer(entry.waterMesh.get(), &shader,
                                    std::move(uniforms), false,
                                    std::move(transform), DrawLayer::GAME);
    }
    if (joinsDrawable) {
        // World-space vertices, placed relative to the eye.
        auto transform = relativeTransform(camera, 0, 0, 0);
        std::vector<ShaderProperty*> uniforms{
            &textureArray,  &joinNormals,     &fogUniform,      &joinLightChunk,
            &joinLightSlot, &joinLightVolume, &joinCoverVolume, &water};
        uniforms.insert(uniforms.end(), extraBlockUniforms.begin(),
                        extraBlockUniforms.end());
        target.drawVertexBuffer(detailJoins.get(), &joinShader,
                                std::move(uniforms), false,
                                std::move(transform), DrawLayer::GAME);
    }
    if (structures) {
        // A structure slab hands over to its real chunk exactly when
        // GameplayState draws that chunk: drawsChunk, uploaded mesh,
        // inside the detail chunk box.
        std::vector<ShaderProperty*> uniforms{
            &textureArray,  &joinNormals,     &fogUniform,      &joinLightChunk,
            &joinLightSlot, &joinLightVolume, &joinCoverVolume, &water};
        uniforms.insert(uniforms.end(), extraBlockUniforms.begin(),
                        extraBlockUniforms.end());
        // Level 0 slabs the chunks show get no proxy mesh (next update).
        structures->setChunkDrawn(
            [this, &chunks](const ChunkCoord& c) {
                return drawsChunk(c) && chunks.hasUploadedMesh(c);
            },
            detailCentre, quality.chunkRadius);
        structures->draw(target, frustum, camera, &joinShader,
                         std::move(uniforms), [&](const ChunkCoord& c) {
                             return drawsChunk(c) && chunks.hasUploadedMesh(c);
                         });
    }
    ++frame;
}
void TerrainLodManager::submitShadowCasters(RenderTarget& target,
                                            const LightShadows::Face& face,
                                            ChunkManager& chunks) {
    const auto& l = face.light;
    std::vector<ShaderProperty*> lodUniforms = face.lodUniforms;
    lodUniforms.push_back(&shadowDetail);
    lodUniforms.push_back(&detailOrigin);
    for (const auto& p : drawn) {
        auto& entry = *entries.at(p);
        // Patch vertices are absolute world positions: shift the patch to
        // its copy nearest the light, then place it relative to the light.
        const double shift = patchShift(p, float(l[0]), periodX);
        const auto& b = entry.bounds;
        const double lx = b.minX + shift - l[0], ly = b.minY - l[1],
                     lz = b.minZ - l[2];
        const double hx = b.maxX + shift - l[0], hy = b.maxY - l[1],
                     hz = b.maxZ - l[2];
        if (!LightShadowMath::reaches(lx, ly, lz, hx, hy, hz, face.radius) ||
            !face.frustum.intersects(AABB{float(lx), float(ly), float(lz),
                                          float(hx), float(hy), float(hz)}))
            continue;
        auto uniforms = lodUniforms;
        uniforms.push_back(&entry.patchUniform);  // w = spacing
        target.drawShadowCaster(
            entry.mesh.get(), face.shader, std::move(uniforms), false,
            LightShadowMath::translation(shift - l[0], -l[1], -l[2]),
            face.view);
    }
    if (joinsDrawable)
        // Joins hold world positions in the camera's frame, like the light.
        target.drawShadowCaster(
            detailJoins.get(), face.shader, std::vector(face.chunkUniforms),
            false, LightShadowMath::translation(-l[0], -l[1], -l[2]),
            face.view);
    if (structures) {
        // The same hand-over to real chunks as render().
        structures->drawShadow(
            target, face.frustum, l, face.radius, face.shader,
            face.chunkUniforms, face.view, [&](const ChunkCoord& c) {
                return drawsChunk(c) && chunks.hasUploadedMesh(c);
            });
    }
}

size_t TerrainLodManager::structureGaps(const ChunkManager& chunks,
                                        const Camera& camera,
                                        float radius) const {
    if (!structures) return 0;
    return structures->gaps(camera.eye(), radius, [&](const ChunkCoord& c) {
        return drawsChunk(c) && chunks.hasUploadedMesh(c);
    });
}

int TerrainLodManager::drawnSpacingAt(int x, int z) const {
    x = int(WorldWrap::mod(x, periodX));
    for (const auto& p : drawn)
        if (x >= p.x && x < p.x + p.size() && z >= p.z && z < p.z + p.size())
            return p.spacing;
    return 0;
}

size_t TerrainLodManager::backlog() const {
    return terrainBacklog() + structureBacklog();
}

size_t TerrainLodManager::structureBacklog() const {
    return structures ? structures->backlog() : 0;
}

size_t TerrainLodManager::terrainBacklog() const {
    return std::count_if(queue.begin(), queue.end(), [&](const auto& p) {
        const auto it = entries.find(p);
        if (it != entries.end() && it->second->drawable)
            return it->second->stale;
        return !fallbackSatisfied(p);
    });
}
