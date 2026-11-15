#include "terrain_lod_manager.hpp"

#include <bx/math.h>

#include <iostream>

namespace {
std::array<int, 3> errorKey(const TerrainLod::Patch& p) {
    return {p.x, p.z, p.spacing};
}
}  // namespace

TerrainLodManager::Entry::~Entry() {
    if (bgfx::isValid(materialMap)) bgfx::destroy(materialMap);
}
TerrainLodManager::TerrainLodManager(const World& world, IAssets& assets,
                                     ThreadPool& pool)
    : world(world), assets(assets), pool(pool), detailSampler(world, assets) {
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
    fogUniform.initVec4("u_world_eye", {0, 0, 0, TerrainLod::ViewDistance});
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

void TerrainLodManager::choose(const Camera& camera, Vec2 viewport) {
    const Vec3 eye = camera.eye();
    const std::array<int, 4> focus{int(eye.x) / 32, int(eye.z) / 32,
                                   int(eye.y) / 32, int(viewport.y)};
    const bool complete =
        !frontier.current().empty() && frontier.current() == frontier.pending();
    if (!budgetPressure && focus == lastFocus && (!complete || refined)) return;
    const bool pressure = budgetPressure;
    budgetPressure = false;
    const bool hasDetail = std::any_of(
        frontier.current().begin(), frontier.current().end(),
        [](const auto& p) { return p.spacing < TerrainLod::RootSpacing; });
    if (pressure) {
        if (!hasDetail) exactRadius = std::max(32.f, exactRadius - 16);
        refined = false;
    }
    const bool moved = focus != lastFocus;
    const bool teleported =
        lastFocus[0] >= 0 && std::hypot(float(focus[0] - lastFocus[0]),
                                        float(focus[1] - lastFocus[1])) >
                                 TerrainLod::ViewDistance / 64;
    lastFocus = focus;
    if (moved) refined = false;
    // Extending the far edge of the view does not invalidate the existing
    // layout. Use coarse coverage for a large teleport, an uncovered
    // destination, or actual pressure requiring release of old LOD meshes.
    const bool uncovered =
        std::none_of(frontier.current().begin(), frontier.current().end(),
                     [&](const auto& p) {
                         return camera.position.x >= p.x &&
                                camera.position.x < p.x + p.size() &&
                                camera.position.z >= p.z &&
                                camera.position.z < p.z + p.size();
                     });
    coarse = uncovered || teleported || (pressure && hasDetail);
    const auto estimate = [&](const TerrainLod::Patch& p) {
        auto it = errors.find(errorKey(p));
        // Until measured, assume relief at the sampling scale. The mesh stores
        // its midpoint error for subsequent decisions.
        return it == errors.end() ? p.spacing * .4f : std::max(.5f, it->second);
    };
    float threshold = 2;
    std::vector<TerrainLod::Patch> layout;
    for (;;) {
        layout = TerrainLod::select(world.width * 128, world.height * 128, eye,
                                    viewport.y, estimate, coarse, threshold,
                                    exactRadius, camera.position);
        size_t expected = 0;
        for (const auto& p : layout) {
            const auto it = entries.find(p);
            expected += it != entries.end() && it->second->drawable
                            ? it->second->bytes
                            : (p.spacing == 1 ? 320 * 1024 : 180 * 1024);
        }
        if (expected <= TerrainLod::GpuBudget * 3 / 4 || coarse ||
            threshold >= 64)
            break;
        threshold *= 2;
    }
    const auto token = frontier.stage(std::move(layout));
    for (auto& job : jobs)
        if (job.revision != token)
            job.cancel->store(true, std::memory_order_relaxed);
    trim();
    refined = !coarse;
}

void TerrainLodManager::trim() {
    const auto wanted = [&](const TerrainLod::Patch& p) {
        const auto has = [&](const auto& list) {
            return std::find(list.begin(), list.end(), p) != list.end();
        };
        return has(frontier.current()) || has(frontier.pending());
    };
    for (auto it = entries.begin(); it != entries.end();)
        if (!wanted(it->first)) {
            resident -= it->second->bytes;
            it = entries.erase(it);
        } else
            ++it;
    // Error history is advisory and must not grow forever during travel.
    if (errors.size() > 8192) errors.clear();
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
            if (job.revision != frontier.token() || job.cancel->load() ||
                mesh.vertices.empty())
                continue;
            errors[errorKey(job.patch)] = mesh.error;
            auto entry = std::make_unique<Entry>();
            entry->cpu = std::move(mesh);
            entries[job.patch] = std::move(entry);
        } catch (const std::exception& e) {
            // No pending flag is retained: a failed patch can be retried.
            std::cerr << "LOD generation failed: " << e.what() << '\n';
        }
    }
}
void TerrainLodManager::update(const Camera& camera, Vec2 viewport) {
    collect();
    choose(camera, viewport);
    if (frontier.current() == frontier.pending()) return;
    std::vector<TerrainLod::Patch> requests = frontier.pending();
    const auto eye = camera.eye();
    std::stable_sort(requests.begin(), requests.end(),
                     [&](const auto& a, const auto& b) {
                         return std::hypot(a.x + a.size() * .5f - eye.x,
                                           a.z + a.size() * .5f - eye.z) <
                                std::hypot(b.x + b.size() * .5f - eye.x,
                                           b.z + b.size() * .5f - eye.z);
                     });
    for (const auto& p : requests) {
        if (jobs.size() >= 2) break;
        if (entries.contains(p) ||
            std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) {
                return j.patch == p && j.revision == frontier.token();
            }))
            continue;
        const size_t reserve = p.spacing == 1 ? 512 * 1024 : 256 * 1024;
        if (resident + reserve > TerrainLod::GpuBudget - JoinBudget) {
            budgetPressure = true;
            continue;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const World* source = &world;
        IAssets* definitions = &assets;
        auto future = pool.submit_task(
            [source, definitions, cancel, p] {
                SurfaceSampler sampler(*source, *definitions);
                return TerrainLod::build(p, sampler, *cancel);
            },
            BS::pr::low);
        jobs.push_back(
            {p, frontier.token(), std::move(cancel), std::move(future)});
    }
}
void TerrainLodManager::upload(IRenderer& renderer) {
    if (frontier.current() == frontier.pending()) return;
    for (const auto& p : frontier.pending()) {
        auto it = entries.find(p);
        if (it == entries.end()) continue;
        auto& entry = *it->second;
        if (entry.drawable) {
            frontier.publish(frontier.token(), p);
            continue;
        }
        if (!entry.cpu) continue;
        const auto& cpu = *entry.cpu;
        const auto bytes = cpu.bytes();
        if (bytes > TerrainLod::UploadBudget ||
            resident + bytes > TerrainLod::GpuBudget - JoinBudget) {
            // A steep landscape can cost more than the initial estimates.
            // Keep old coverage, then retry with a smaller exact band.
            budgetPressure = true;
            continue;
        }
        if (uploaded + bytes > TerrainLod::UploadBudget) continue;
        entry.mesh.initDynamic<BlockVertex>(&renderer, cpu.vertices.size(),
                                            cpu.indices.size());
        const auto& gpu = std::get<DynamicMesh>(entry.mesh.get());
        if (!bgfx::isValid(gpu.vbh) || !bgfx::isValid(gpu.ibh)) {
            entry.mesh.release();
            budgetPressure = true;
            continue;
        }
        entry.mesh.update(cpu.vertices.data(), cpu.vertices.size(),
                          cpu.indices.data(), cpu.indices.size(), true);
        entry.materialMap = bgfx::createTexture2D(
            cpu.mapSize, cpu.mapSize, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            bgfx::copy(cpu.materials.data(), cpu.materials.size()));
        if (!bgfx::isValid(entry.materialMap)) {
            entry.mesh.release();
            continue;
        }
        entry.mapUniform.initTexture("u_surface_map", 1, entry.materialMap);
        entry.patchUniform.initVec4(
            "u_lod_patch",
            {float(p.x), float(p.z), float(p.size()), float(p.spacing)});
        entry.bounds = cpu.bounds;
        entry.bands = cpu.bands;
        entry.bytes = bytes;
        entry.cpu.reset();
        entry.drawable = true;
        uploaded += bytes;
        resident += bytes;
        frontier.publish(frontier.token(), p);
    }
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
    for (auto it = surfaceBands.begin(); it != surfaceBands.end();)
        if (std::abs(it->first.first - cx) > keep + 1 ||
            std::abs(it->first.second - cz) > keep + 1)
            it = surfaceBands.erase(it);
        else
            ++it;
    // Exact LOD results provide cheap initial summaries, but do not own the
    // summaries or the visibility of chunks after handover.
    for (const auto& p : frontier.current())
        for (const auto& band : entries.at(p)->bands)
            if (std::abs(band.x - cx) <= keep + 1 &&
                std::abs(band.z - cz) <= keep + 1)
                surfaceBands[{band.x, band.z}] = band;
    int newBands = 0;
    chunks.forEachReady([&](const ChunkCoord& coord, Chunk&) {
        const int x = int(coord.x), z = int(coord.z);
        if (newBands >= 4 ||
            std::abs(x - cx) > TerrainLod::DetailCoverage::EnterRadius ||
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
            const int x = int(coord.x) + dx[i], y = int(coord.y) + dy[i],
                      z = int(coord.z) + dz[i];
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
        [&](ChunkCoord coord) { return chunks.hasUploadedMesh(coord); });
    detailX = cx - DetailSize / 2;
    detailZ = cz - DetailSize / 2;
}
void TerrainLodManager::uploadCoverage() {
    coverage.fill(0);
    for (const auto& [key, band] : detailCoverage.columns()) {
        const int ix = band.x - detailX, iz = band.z - detailZ;
        if (ix >= 0 && iz >= 0 && ix < DetailSize && iz < DetailSize)
            coverage[(iz * DetailSize + ix) * 4] = 255;
    }
    bgfx::updateTexture2D(detailMap, 0, 0, 0, 0, DetailSize, DetailSize,
                          bgfx::copy(coverage.data(), coverage.size()));
    detailOrigin.setVec4(
        {float(detailX * 16), float(detailZ * 16), DetailSize * 16, 0});
}
bool TerrainLodManager::uploadJoins(std::span<const TerrainLod::Patch> layout,
                                    IRenderer& renderer) {
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
    if (joinedCoverageVersion == detailCoverage.version() &&
        std::equal(layout.begin(), layout.end(), joinedLayout.begin(),
                   joinedLayout.end()))
        return true;
    std::vector<TerrainLod::SurfaceBand> columns;
    for (const auto& [key, band] : detailCoverage.columns())
        columns.push_back(band);
    const auto mesh = TerrainLod::joinDetails(
        layout, columns, detailSampler,
        [&](int x, int z) { return detailCoverage.contains(x, z); });
    if (mesh.bytes() > JoinBudget)
        throw std::runtime_error("Terrain detail joins exceed bounded window");
    if (uploaded + mesh.bytes() > TerrainLod::UploadBudget) return false;
    joinsDrawable = !mesh.vertices.empty();
    if (joinsDrawable)
        detailJoins.update(mesh.vertices.data(), mesh.vertices.size(),
                           mesh.indices.data(), mesh.indices.size(), true);
    uploaded += mesh.bytes();
    joinedLayout.assign(layout.begin(), layout.end());
    joinedCoverageVersion = detailCoverage.version();
    return true;
}
void TerrainLodManager::render(RenderTarget& target, IRenderer& renderer,
                               ChunkManager& chunks, const Camera& camera,
                               Vec2 viewport) {
    uploaded = 0;
    updateCoverage(chunks, camera);
    const auto& current =
        frontier.current().empty() ? frontier.pending() : frontier.current();
    if (!uploadJoins(current, renderer))
        throw std::runtime_error("Cannot upload current terrain detail joins");
    upload(renderer);
    // Geometry and its joins become visible together. If this frame's upload
    // budget is exhausted, keep the old frontier until the next frame.
    if (frontier.canCommit() && uploadJoins(frontier.pending(), renderer)) {
        frontier.commit();
        trim();
    }
    uploadCoverage();
    const auto eye = camera.eye();
    fogUniform.setVec4({eye.x, eye.y, eye.z, TerrainLod::ViewDistance});
    float view[16], proj[16], vp[16];
    bx::mtxLookAt(view, {eye.x, eye.y, eye.z},
                  {camera.position.x, camera.position.y, camera.position.z},
                  {0, 1, 0});
    bx::mtxProj(proj, 60.f, viewport.x / std::max(1.f, viewport.y), 1.f,
                TerrainLod::ViewDistance + 512,
                bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(vp, view, proj);
    const auto frustum = Frustum::fromViewProj(vp);
    // Before the first complete coarse frontier, show each available root.
    const auto& visible =
        frontier.current().empty() ? frontier.pending() : frontier.current();
    for (const auto& p : visible) {
        auto it = entries.find(p);
        if (it == entries.end() || !it->second->drawable) continue;
        auto& entry = *it->second;
        if (!frustum.intersects(entry.bounds)) continue;
        std::array<float, 16> transform;
        bx::mtxIdentity(transform.data());
        std::vector<ShaderProperty*> uniforms{
            &textureArray,  &entry.mapUniform, &entry.patchUniform,
            &detailUniform, &detailOrigin,     &fogUniform,
            &patchNormals};
        target.drawVertexBuffer(entry.mesh.get(), &shader, std::move(uniforms),
                                false, std::move(transform), DrawLayer::GAME);
    }
    if (joinsDrawable) {
        std::array<float, 16> transform;
        bx::mtxIdentity(transform.data());
        target.drawVertexBuffer(
            detailJoins.get(), &joinShader,
            {&textureArray, &joinNormals, &fogUniform, &joinLightChunk,
             &joinLightSlot, &joinLightVolume},
            false, std::move(transform), DrawLayer::GAME);
    }
}
size_t TerrainLodManager::backlog() const {
    return std::count_if(frontier.pending().begin(), frontier.pending().end(),
                         [&](const auto& p) {
                             const auto it = entries.find(p);
                             return it == entries.end() ||
                                    !it->second->drawable;
                         });
}
