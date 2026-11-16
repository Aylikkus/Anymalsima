#include "terrain_lod_manager.hpp"

#include <bx/math.h>

#include <iostream>
#include <ranges>

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
      detailSampler(world, assets, inputs,
                    edits ? edits->snapshot() : nullptr) {
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
    detailOrigin.initVec4("u_detail_origin", {0, 0, DetailSize * 16, 0});
    fogUniform.initVec4("u_world_eye", {0, 0, 0, quality.viewDistance});
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
    joinLightChunk.initVec4("u_light_chunk", {0, 0, 0, 0});
    joinLightSlot.initVec4("u_light_slot", {0, 0, 0, 0});
}
TerrainLodManager::~TerrainLodManager() {
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
}

bool TerrainLodManager::wanted(const TerrainLod::Patch& p) const {
    return desiredSet.contains(p) || roots.contains(p);
}
bool TerrainLodManager::isDrawable(const TerrainLod::Patch& p) const {
    const auto it = entries.find(p);
    return it != entries.end() && it->second->drawable;
}
bool TerrainLodManager::inView(const TerrainLod::Patch& p) const {
    // Same visibility rule as TerrainLod::select, at the selection eye, so
    // patches select() dropped never block their parent's replacement.
    return p.x < world.width * 128 && p.z < world.height * 128 &&
           horizontalDistance(p, selectionEye, periodX) < quality.viewDistance;
}

void TerrainLodManager::choose(const Camera& camera, Vec2 viewport) {
    const Vec3 eye = camera.eye();
    const std::array<int, 4> focus{int(eye.x) / 32, int(eye.z) / 32,
                                   int(eye.y) / 32, int(viewport.y)};
    const bool moved = focus != lastFocus;
    // Measured errors refine the layout once the current one has arrived.
    const bool refine = errorsChanged && backlog() == 0;
    if (!moved && !refine && errorScale == selectedScale) return;
    if (moved && errorScale > 1 && resident < budget() / 2)
        errorScale = std::max(1.f, errorScale / 1.5f);
    lastFocus = focus;
    errorsChanged = false;
    selectedScale = errorScale;
    selectionEye = eye;
    const auto estimate = [&](const TerrainLod::Patch& p) {
        auto it = errors.find(errorKey(p));
        // Until measured, assume relief at the sampling scale. The mesh stores
        // its midpoint error for subsequent decisions.
        return it == errors.end() ? p.spacing * .4f : std::max(.5f, it->second);
    };
    float threshold = quality.pixelError * errorScale;
    std::vector<TerrainLod::Patch> layout;
    for (;;) {
        layout = TerrainLod::select(world.width * 128, world.height * 128, eye,
                                    viewport.y, estimate, false, threshold,
                                    quality.exactRadius, camera.position,
                                    quality.viewDistance, periodX);
        size_t expected = 0;
        for (const auto& p : layout) {
            const auto it = entries.find(p);
            expected += it != entries.end() && it->second->drawable
                            ? it->second->bytes
                            : (p.spacing == 1 ? 320 * 1024 : 180 * 1024);
        }
        if (expected <= quality.gpuBudget * 3 / 4 || threshold >= 64) break;
        threshold *= 2;
    }
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
    std::stable_sort(queue.begin(), queue.end(), near);
    const size_t rootCount = queue.size();
    for (const auto& p : desired)
        if (!roots.contains(p)) queue.push_back(p);
    std::stable_sort(queue.begin() + rootCount, queue.end(), near);
    for (auto& job : jobs)
        if (!wanted(job.patch))
            job.cancel->store(true, std::memory_order_relaxed);
    // Unuploaded results of an older layout only cost memory. Drawable ones
    // stay cached until the GPU budget needs their room.
    for (auto it = entries.begin(); it != entries.end();)
        if (!it->second->drawable && !wanted(it->first))
            it = entries.erase(it);
        else
            ++it;
    // Error history is advisory and must not grow forever during travel.
    if (errors.size() > 8192) errors.clear();
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
        resident -= it->second->bytes;
        markDrawable(it->first, -1);
    }
    entries.erase(it);
}
bool TerrainLodManager::makeRoom(size_t bytes,
                                 const TerrainLod::Patch* incoming) {
    if (resident + bytes <= budget()) return true;
    // A desired leaf replaces every finer leftover inside it in the same
    // frame it uploads, so those may go even if they were just drawn.
    const bool replaces = incoming && !splitSet.contains(*incoming);
    const auto inside = [&](const TerrainLod::Patch& q) {
        const auto& p = *incoming;
        return replaces && q.spacing < p.spacing && q.x >= p.x && q.z >= p.z &&
               q.x < p.x + p.size() && q.z < p.z + p.size();
    };
    std::vector<Entries::iterator> candidates;
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->second->drawable && !wanted(it->first) &&
            (it->second->lastDrawn + 1 < frame || inside(it->first)))
            candidates.push_back(it);
    // Invisible entries first, least recently drawn first.
    std::sort(candidates.begin(), candidates.end(),
              [&](const auto& a, const auto& b) {
                  const bool aSeen = a->second->lastDrawn + 1 >= frame,
                             bSeen = b->second->lastDrawn + 1 >= frame;
                  return std::tie(aSeen, a->second->lastDrawn) <
                         std::tie(bSeen, b->second->lastDrawn);
              });
    for (const auto& it : candidates) {
        if (resident + bytes <= budget()) break;
        erase(it);
    }
    return resident + bytes <= budget();
}
void TerrainLodManager::pressure() {
    if (errorScale == selectedScale && errorScale < 16)
        errorScale = std::min(16.f, errorScale * 1.5f);
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
            if (job.cancel->load() || mesh.vertices.empty()) continue;
            const auto [error, inserted] =
                errors.try_emplace(errorKey(job.patch), mesh.error);
            if (inserted || error->second != mesh.error) errorsChanged = true;
            error->second = mesh.error;
            if (const auto it = entries.find(job.patch); it != entries.end()) {
                // Rebuilt after an edit: replaces the drawn mesh on upload.
                auto& entry = *it->second;
                if (entry.drawable && entry.stale &&
                    (!entry.replacement ||
                     entry.replacementRevision < job.editRevision)) {
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
    for (const auto& p : queue) {
        if (jobs.size() >= maxJobs) break;
        const auto existing = entries.find(p);
        const bool rebuild =
            existing != entries.end() && existing->second->needsBuild();
        if ((existing != entries.end() && !rebuild) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) {
                return j.patch == p &&
                       (!rebuild ||
                        j.editRevision >= existing->second->staleRevision);
            }))
            continue;
        const size_t reserve = p.spacing == 1 ? 512 * 1024 : 256 * 1024;
        // A leaf with cached finer leftovers frees their room on upload.
        if (!rebuild && !makeRoom(reserve) &&
            (splitSet.contains(p) || !drawableBelow.contains(p))) {
            pressure();
            continue;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        auto future = pool.submit_task(
            [source = &world, definitions = &assets, shared = inputs, cancel, p,
             map = quality.materialMap, wrapX = periodX > 0,
             changes = edits ? edits->snapshot() : nullptr] {
                SurfaceSampler sampler(*source, *definitions, shared, changes);
                return TerrainLod::build(p, sampler, *cancel, map, wrapX);
            },
            // Roots are the fallback for everything else in view; edited
            // terrain should update promptly.
            roots.contains(p) || rebuild ? BS::pr::normal : BS::pr::low);
        jobs.push_back({p, edits ? edits->revision() : 0, std::move(cancel),
                        std::move(future)});
    }
}
void TerrainLodManager::update(const Camera& camera, Vec2 viewport) {
    collect();
    choose(camera, viewport);
    request();
    if (structures) structures->update(camera.eye(), edits->snapshot());
}
void TerrainLodManager::upload(IRenderer& renderer) {
    for (const auto& p : queue) {
        if (uploaded >= quality.uploadBudget) break;
        auto it = entries.find(p);
        if (it == entries.end() || it->second->drawable || !it->second->cpu)
            continue;
        auto& entry = *it->second;
        const auto& cpu = *entry.cpu;
        const auto bytes = cpu.bytes();
        // One oversized mesh may use a whole frame rather than never fit.
        if (uploaded && uploaded + bytes > quality.uploadBudget) continue;
        if (!makeRoom(bytes, &p)) {
            // A steep landscape can cost more than the initial estimates.
            pressure();
            continue;
        }
        if (!createGpu(entry, p, cpu, renderer)) {
            pressure();
            continue;
        }
        entry.bounds = cpu.bounds;
        entry.bands = cpu.bands;
        entry.bytes = bytes;
        entry.cpu.reset();
        entry.drawable = true;
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
    entry.mapUniform.initTexture("u_surface_map", 1, entry.materialMap);
    entry.patchUniform.initVec4(
        "u_lod_patch",
        {float(p.x), float(p.z), float(p.size()), float(p.spacing)});
    entry.shiftUniform.initVec4("u_lod_shift", {0, 0, 0, 0});
    return true;
}

void TerrainLodManager::uploadReplacements(IRenderer& renderer) {
    std::vector<TerrainLod::Patch> ready;
    for (const auto& [p, entry] : entries)
        if (entry->drawable && entry->replacement) ready.push_back(p);
    for (const auto& p : ready) {
        if (uploaded >= quality.uploadBudget) break;
        auto it = entries.find(p);
        if (it == entries.end()) continue;
        const auto bytes = it->second->replacement->bytes();
        const auto oldBytes = it->second->bytes;
        if (bytes > oldBytes && !makeRoom(bytes - oldBytes)) {
            pressure();
            continue;
        }
        // makeRoom may have evicted this entry.
        it = entries.find(p);
        if (it == entries.end()) continue;
        auto& old = *it->second;
        auto fresh = std::make_unique<Entry>();
        if (!createGpu(*fresh, p, *old.replacement, renderer)) continue;
        fresh->bounds = old.replacement->bounds;
        fresh->bands = old.replacement->bands;
        fresh->bytes = bytes;
        fresh->drawable = true;
        fresh->lastDrawn = old.lastDrawn;
        fresh->staleRevision = old.staleRevision;
        fresh->stale = old.replacementRevision < old.staleRevision;
        uploaded += bytes;
        resident = resident - old.bytes + bytes;
        // Drawable either way: the per-ancestor counts are unchanged.
        it->second = std::move(fresh);
    }
}

void TerrainLodManager::editChanged(uint64_t column) {
    const int x0 = int(column >> 32) * Chunk::SIZE_X - 1,
              z0 = int(uint32_t(column)) * Chunk::SIZE_Z - 1;
    const int x1 = x0 + Chunk::SIZE_X + 2, z1 = z0 + Chunk::SIZE_Z + 2;
    const auto revision = edits->revision();
    for (auto it = entries.begin(); it != entries.end();) {
        const auto& p = it->first;
        // Normals and skirts sample up to one spacing outside the patch,
        // which may reach across the X seam.
        const auto touchesAt = [&](int shift) {
            return p.x - p.spacing < x1 + shift &&
                   p.x + p.size() + p.spacing > x0 + shift &&
                   p.z - p.spacing < z1 && p.z + p.size() + p.spacing > z0;
        };
        const bool touches =
            touchesAt(0) ||
            (periodX > 0 && (touchesAt(periodX) || touchesAt(-periodX)));
        if (!touches) {
            ++it;
            continue;
        }
        if (!it->second->drawable) {
            // A result waiting for upload is outdated: build it again.
            it = entries.erase(it);
            continue;
        }
        it->second->stale = true;
        it->second->staleRevision = revision;
        ++it;
    }
    detailSampler.setEdits(edits->snapshot());
    if (structures) structures->edited(column);
    surfaceBands.erase({int(column >> 32), int(uint32_t(column))});
    joinedCoverageVersion = UINT64_MAX;
}

void TerrainLodManager::selectDrawn() {
    drawn.clear();
    const TerrainLod::Cover cover(
        splitSet, [&](const auto& p) { return inView(p); },
        [&](const auto& p) { return isDrawable(p); },
        [&](const auto& p) { return drawableBelow.contains(p); });
    for (const auto& root : roots) cover.select(root, drawn);
    for (const auto& p : drawn) entries.at(p)->lastDrawn = frame;
}

bool TerrainLodManager::drawsColumn(unsigned x, unsigned z) const {
    return detailCoverage.contains(int(x), int(z));
}
void TerrainLodManager::updateCoverage(ChunkManager& chunks,
                                       const Camera& camera) {
    const int cx = int(camera.position.x) / 16,
              cy = int(camera.position.y) / 16,
              cz = int(camera.position.z) / 16;
    constexpr int keep = TerrainLod::DetailCoverage::KeepRadius;
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
        if (p.spacing != 1 || it == entries.end()) continue;
        for (const auto& band : it->second->bands)
            if (distX(band.x) <= keep + 1 && std::abs(band.z - cz) <= keep + 1)
                surfaceBands[{band.x, band.z}] = band;
    }
    int newBands = 0;
    chunks.forEachMeshed([&](const ChunkCoord& coord) {
        const int x = int(coord.x), z = int(coord.z);
        if (newBands >= 4 ||
            distX(x) > TerrainLod::DetailCoverage::EnterRadius ||
            std::abs(z - cz) > TerrainLod::DetailCoverage::EnterRadius ||
            surfaceBands.contains({x, z}) || !chunks.isMeshUploaded(coord))
            return;
        int low = Y_CHUNKS * Chunk::SIZE_Y, high = 0;
        for (int tz = z * 16 - 1; tz <= z * 16 + 16; ++tz)
            for (int tx = x * 16 - 1; tx <= x * 16 + 16; ++tx) {
                const int h = detailSampler.height(tx, tz);
                low = std::min(low, h);
                high = std::max(high, std::max(h, SEA_LEVEL_Y));
            }
        surfaceBands[{x, z}] = {x, z, low / 16, high / 16};
        ++newBands;
    });
    std::vector<TerrainLod::SurfaceBand> candidates;
    for (const auto& [key, band] : surfaceBands) candidates.push_back(band);
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
        return true;
    };
    detailCoverage.update(
        {unsigned(cx), unsigned(cy), unsigned(cz)}, candidates, current,
        [&](ChunkCoord coord) { return chunks.hasUploadedMesh(coord); },
        wrap.chunks());
    detailX = cx - DetailSize / 2;
    detailZ = cz - DetailSize / 2;
}
void TerrainLodManager::uploadCoverage() {
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
    bgfx::updateTexture2D(detailMap, 0, 0, 0, 0, DetailSize, DetailSize,
                          bgfx::copy(coverage.data(), coverage.size()));
    detailOrigin.setVec4(
        {float(detailX * 16), float(detailZ * 16), DetailSize * 16, 0});
}
void TerrainLodManager::uploadJoins(IRenderer& renderer) {
    // With at most a 15x15 column window, even a checkerboard of visible
    // columns has fewer than 2 MiB of indexed one-tile boundary quads.
    constexpr size_t side = TerrainLod::DetailCoverage::KeepRadius * 2 + 1;
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
    joinedLayout = std::move(layout);
    joinedCoverageVersion = detailCoverage.version();
}
void TerrainLodManager::render(RenderTarget& target, IRenderer& renderer,
                               ChunkManager& chunks, const Camera& camera,
                               Vec2 viewport) {
    uploaded = 0;
    updateCoverage(chunks, camera);
    upload(renderer);
    uploadReplacements(renderer);
    if (structures)
        uploaded +=
            structures->upload(renderer, quality.uploadBudget > uploaded
                                             ? quality.uploadBudget - uploaded
                                             : 0);
    selectDrawn();
    uploadJoins(renderer);
    uploadCoverage();
    const auto eye = camera.eye();
    fogUniform.setVec4({eye.x, eye.y, eye.z, quality.viewDistance});
    float view[16], proj[16], vp[16];
    bx::mtxLookAt(view, {eye.x, eye.y, eye.z},
                  {camera.position.x, camera.position.y, camera.position.z},
                  {0, 1, 0});
    bx::mtxProj(proj, 60.f, viewport.x / std::max(1.f, viewport.y), 1.f,
                quality.viewDistance + 512, bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(vp, view, proj);
    const auto frustum = Frustum::fromViewProj(vp);
    for (const auto& p : drawn) {
        auto& entry = *entries.at(p);
        // Each patch is drawn at its copy nearest the eye (X wraps).
        const float shift = patchShift(p, eye.x, periodX);
        AABB bounds = entry.bounds;
        bounds.minX += shift;
        bounds.maxX += shift;
        if (!frustum.intersects(bounds)) continue;
        // World-space vertices, placed relative to the eye.
        auto transform = relativeTransform(camera, shift, 0, 0);
        entry.patchUniform.setVec4({float(p.x) + shift, float(p.z),
                                    float(p.size()), float(p.spacing)});
        entry.shiftUniform.setVec4({shift, 0, 0, 0});
        std::vector<ShaderProperty*> uniforms{
            &textureArray,  &entry.mapUniform,  &entry.patchUniform,
            &detailUniform, &detailOrigin,      &fogUniform,
            &patchNormals,  &entry.shiftUniform};
        target.drawVertexBuffer(entry.mesh.get(), &shader, std::move(uniforms),
                                false, std::move(transform), DrawLayer::GAME);
    }
    if (joinsDrawable) {
        // World-space vertices, placed relative to the eye.
        auto transform = relativeTransform(camera, 0, 0, 0);
        target.drawVertexBuffer(
            detailJoins.get(), &joinShader,
            {&textureArray, &joinNormals, &fogUniform, &joinLightChunk,
             &joinLightSlot, &joinLightVolume},
            false, std::move(transform), DrawLayer::GAME);
    }
    if (structures) {
        // A structure slab hands over to its real chunk exactly when
        // GameplayState draws that chunk: detailed column, uploaded mesh,
        // inside the detail chunk box.
        const int cx = int(camera.position.x) / Chunk::SIZE_X,
                  cy = int(camera.position.y) / Chunk::SIZE_Y,
                  cz = int(camera.position.z) / Chunk::SIZE_Z;
        structures->draw(
            target, frustum, camera, &joinShader,
            {&textureArray, &joinNormals, &fogUniform, &joinLightChunk,
             &joinLightSlot, &joinLightVolume},
            [&](const ChunkCoord& c) {
                return std::abs(world.wrap().deltaChunk(c.x, cx)) <=
                           DETAIL_CHUNK_RADIUS &&
                       std::abs(int(c.y) - cy) <= DETAIL_CHUNK_RADIUS &&
                       std::abs(int(c.z) - cz) <= DETAIL_CHUNK_RADIUS &&
                       detailCoverage.contains(int(c.x), int(c.z)) &&
                       chunks.hasUploadedMesh(c);
            });
    }
    ++frame;
}
size_t TerrainLodManager::backlog() const {
    return (structures ? structures->backlog() : 0) +
           std::count_if(queue.begin(), queue.end(), [&](const auto& p) {
               const auto it = entries.find(p);
               return it == entries.end() || !it->second->drawable ||
                      it->second->stale;
           });
}
