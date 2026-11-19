#include "flora_renderer.hpp"

#include <chrono>
#include <cmath>

#include "flora_generator.hpp"
#include "foliage_light.hpp"
#include "ground_cover_climate.hpp"
#include "items/ground_cover.hpp"

namespace {
// Plant meshes built per frame, and chunk instance caches rebuilt per frame.
constexpr int MeshBuildsPerFrame = 48;
constexpr int ChunkRebuildsPerFrame = 24;
// Bare soil under plants without ground cover.
constexpr Color3 BareGround{0.40f, 0.32f, 0.22f};
}  // namespace

FloraRenderer::FloraRenderer(ChunkManager& chunks, const IAssets& assets,
                             const World& world, IRenderer& renderer)
    : chunks(chunks),
      assets(assets),
      world(world),
      renderer(renderer),
      builder(assets) {
    albedo.initTexture("u_texture_array", 0, assets.getFoliageAlbedoArray());
    normals.initTexture("u_normal_array", 1, assets.getFoliageNormalArray());
    eyeUniform.initVec4("u_world_eye", {0, 0, 0, 4096});
    params.initVec4("u_foliage", {0, 1, 0, 0});
    if (assets.hasStorage<Flora>())
        for (const auto& f : assets.getStorage<Flora>().items)
            speciesFade.push_back(fadeDistance(floraHeightEstimate(f)));
    const auto changed = [this](const ChunkCoord& c) { invalidate(c); };
    readyConnection = chunks.becameReady.connect(changed);
    floraConnection = chunks.floraChanged.connect(changed);
    // Light at the base tile, and tiles (cover under a plant) both arrive
    // through lightChanged: markModified emits it too.
    lightConnection = chunks.lightChanged.connect([this](const ChunkCoord& c) {
        invalidate(c);
        if (c.y > 0) invalidate({c.x, c.y - 1, c.z});
    });
    removedConnection = chunks.removed.connect([this](const ChunkCoord& c) {
        caches.erase(c);
        withPlants.erase(c);
    });
}

void FloraRenderer::setSkyLight(SkyHeightMap& map) {
    if (skyMap) skyMap->changed.disconnect(skyConnection);
    skyMap = &map;
    skyConnection = map.changed.connect([this](uint64_t key) {
        for (unsigned y = 0; y < Y_CHUNKS; ++y)
            if (const auto it =
                    caches.find({unsigned(key >> 32), y, unsigned(key)});
                it != caches.end())
                it->second.valid = false;
    });
}

FloraRenderer::~FloraRenderer() {
    if (skyMap) skyMap->changed.disconnect(skyConnection);
    chunks.becameReady.disconnect(readyConnection);
    chunks.floraChanged.disconnect(floraConnection);
    chunks.lightChanged.disconnect(lightConnection);
    chunks.removed.disconnect(removedConnection);
}

void FloraRenderer::invalidate(const ChunkCoord& coord) {
    const auto layer = chunks.flora(coord);
    if (!layer || layer->empty()) {
        caches.erase(coord);
        withPlants.erase(coord);
        return;
    }
    withPlants.insert(coord);
    caches[coord].valid = false;
}

const FloraRenderer::MeshEntry* FloraRenderer::mesh(uint32_t key, int& budget) {
    if (auto it = meshes.find(key); it != meshes.end())
        return it->second.cpu.empty() ? nullptr : &it->second;
    if (budget <= 0) return nullptr;
    --budget;
    auto& entry = meshes[key];
    entry.cpu = builder.build(uint8_t(key >> 16), int((key >> 8) & 255),
                              FloraStage(key & 255));
    if (entry.cpu.empty()) return nullptr;
    entry.gpu.initStatic(&renderer, entry.cpu.vertices.data(),
                         entry.cpu.vertices.size(), entry.cpu.indices.data(),
                         entry.cpu.indices.size());
    return &entry;
}

void FloraRenderer::rebuild(const ChunkCoord& coord, ChunkPlants& cache,
                            double today) {
    cache.plants.clear();
    cache.valid = true;
    const auto layer = chunks.flora(coord);
    if (!layer || !assets.hasStorage<Flora>()) return;
    const auto& species = assets.getStorage<Flora>();
    const auto* covers = assets.hasStorage<GroundCover>()
                             ? &assets.getStorage<GroundCover>()
                             : nullptr;
    cache.plants.reserve(layer->instances.size());
    for (const auto& p : layer->instances) {
        if (!p.species || p.species > species.items.size()) continue;
        const auto& flora = species.get(p.species);
        const auto state = flora_growth::at(flora, p.seed, today - p.birthDay);
        if (!state.alive) continue;
        // Harvested flowers and fruit are gone until the plant regrows.
        auto stage = state.stage;
        if ((p.flags & FloraInstance::Harvested) &&
            (stage == FloraStage::Flowering || stage == FloraStage::Fruiting))
            stage = FloraStage::Mature;
        Plant plant;
        plant.mesh =
            meshKey(p.species, p.seed % FloraMeshBuilder::Variants, stage);
        plant.x = double(coord.x) * Chunk::SIZE_X + p.localX();
        plant.z = double(coord.z) * Chunk::SIZE_Z + p.localZ();
        const int anchorY = int(coord.y) * Chunk::SIZE_Y + p.y;
        plant.y = anchorY + 1;
        const int tx = int(std::floor(plant.x)), tz = int(std::floor(plant.z));
        // Light of the air tile the plant stands in.
        const float sun =
            foliage_light::sky(chunks, skyMap, tx, anchorY + 1, tz);
        float block = foliage_light::pack(0, 0, foliage_light::White);
        if (const Tile* above = chunks.tileAt(tx, anchorY + 1, tz)) {
            const int level = above->lightLevel.artificial_light;
            block = foliage_light::pack(
                level, level,
                level
                    ? foliage_light::at(chunks, blockLight, tx, anchorY + 1, tz)
                    : foliage_light::White);
        }
        // Ground colour: the cover's climate tint, faded to bare soil by
        // its density.
        Color3 ground = BareGround;
        if (const Tile* anchor = chunks.tileAt(tx, anchorY, tz);
            anchor && anchor->cover && covers &&
            anchor->cover <= covers->items.size()) {
            const auto [h, t] =
                ground_cover_climate::at(world, plant.x, plant.z);
            const auto tint = covers->get(anchor->cover).tint_gradient.at(h, t);
            const float d = anchor->coverDensity() / 15.f;
            ground = {BareGround.r + (tint.r - BareGround.r) * d,
                      BareGround.g + (tint.g - BareGround.g) * d,
                      BareGround.b + (tint.b - BareGround.b) * d};
        }
        const float scale = p.scale();
        const float stretch = 0.92f + 0.16f * float(p.seed & 0xF) / 15.f;
        const float seed = float(p.seed) / 65536.f;
        const float wither =
            state.stage == FloraStage::Withering ? state.progress : 0.f;
        const float data[12] = {p.yaw(),  scale,    scale * stretch,
                                scale,    seed,     ground.r,
                                ground.g, ground.b, wither,
                                sun,      block,    speciesFade[p.species - 1]};
        std::copy(std::begin(data), std::end(data), plant.data);
        plant.fade = data[11];
        cache.plants.push_back(plant);
    }
}

void FloraRenderer::render(RenderTarget& target, const Camera& camera,
                           const Frustum& frustum, LightRenderer& lights,
                           double now, double today, ShaderProperty& water) {
    const auto started = std::chrono::steady_clock::now();
    drawn = calls = 0;
    instances.clear();
    // Stages are evaluated per whole day; a new day refreshes every chunk.
    today = std::floor(today);
    if (today != lastToday) {
        lastToday = today;
        for (auto& [coord, cache] : caches) cache.valid = false;
    }
    if (withPlants.empty() || !bgfx::isValid(assets.getFoliageAlbedoArray())) {
        cpuMs = 0;
        return;
    }
    // Eye in double, as relativeTransform computes it.
    const double pitch = camera.pitch, yaw = camera.yaw, d = camera.distance;
    const double ex = camera.position.x - std::cos(pitch) * std::sin(yaw) * d;
    const double ey = camera.position.y - std::sin(pitch) * d;
    const double ez = camera.position.z - std::cos(pitch) * std::cos(yaw) * d;
    const WorldWrap wrap = chunks.wrap();
    const int tallest = std::max(1, chunks.floraSpecies().maxHeight());
    const double reach = FadeEnd + Chunk::SIZE_X;

    std::unordered_map<uint32_t, std::vector<std::pair<const Plant*, double>>>
        groups;
    int rebuilds = ChunkRebuildsPerFrame;
    for (const auto& coord : withPlants) {
        // The chunk's copy nearest the eye (X wraps).
        const double ox =
            ex + wrap.deltaX(double(coord.x) * Chunk::SIZE_X + 8, ex) - 8;
        const double oy = double(coord.y) * Chunk::SIZE_Y;
        const double oz = double(coord.z) * Chunk::SIZE_Z;
        const double dx = std::max({ox - ex, 0.0, ex - ox - Chunk::SIZE_X});
        const double dz = std::max({oz - ez, 0.0, ez - oz - Chunk::SIZE_Z});
        const double dy =
            std::max({oy - ey, 0.0, ey - oy - Chunk::SIZE_Y - tallest});
        if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
        if (!frustum.intersects(AABB{float(ox), float(oy), float(oz),
                                     float(ox + Chunk::SIZE_X),
                                     float(oy + Chunk::SIZE_Y + tallest),
                                     float(oz + Chunk::SIZE_Z)}))
            continue;
        auto& cache = caches[coord];
        if (!cache.valid) {
            if (rebuilds-- <= 0) continue;
            rebuild(coord, cache, today);
        }
        const double shift = ox - double(coord.x) * Chunk::SIZE_X;
        for (const auto& plant : cache.plants) {
            const double px = plant.x + shift - ex, py = plant.y - ey,
                         pz = plant.z - ez;
            if (px * px + py * py + pz * pz > plant.fade * plant.fade) continue;
            groups[plant.mesh].push_back({&plant, shift});
        }
    }
    std::vector<ShaderProperty*> base{&albedo, &normals, &eyeUniform, &params,
                                      &water};
    lights.appendUniforms(base, LightRenderer::Program::Block);
    eyeUniform.setVec4({float(ex), float(ey), float(ez), camera.viewDistance});
    params.setVec4({float(std::fmod(now, 3600.0)), 1, 0, 0});
    int builds = MeshBuildsPerFrame;
    for (const auto& [key, plants] : groups) {
        const auto* entry = mesh(key, builds);
        if (!entry) continue;
        const auto count = bgfx::getAvailInstanceDataBuffer(
            uint32_t(plants.size()), InstanceStride);
        if (!count) break;
        auto& buffer = instances.emplace_back();
        bgfx::allocInstanceDataBuffer(&buffer, count, InstanceStride);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& [plant, shift] = plants[i];
            auto* v = reinterpret_cast<float*>(buffer.data +
                                               size_t(i) * InstanceStride);
            v[0] = float(plant->x + shift - ex);
            v[1] = float(plant->y - ey);
            v[2] = float(plant->z - ez);
            std::copy(plant->data, plant->data + 12, v + 3);
            v[15] = 0;
        }
        target.drawInstanced(entry->gpu.get(), &shader, std::vector(base), true,
                             relativeTransform(camera, ex, ey, ez),
                             DrawLayer::GAME, &buffer);
        drawn += count;
        ++calls;
    }
    cpuMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
}
