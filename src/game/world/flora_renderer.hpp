#pragma once

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"
#include "flora_mesh_builder.hpp"
#include "light_renderer.hpp"
#include "render/frustrum.hpp"
#include "render/render_target.hpp"

// Main-thread GPU side of plants (docs/handoff/flora.md, sections 5 and 6).
//
// Meshes come from FloraMeshBuilder, one per (species, variant, stage), built
// on demand within a per-frame budget and shared by every instance. Each
// chunk with plants keeps a cache of its instances (base position, yaw,
// scale, seed, growth stage, light at the base tile, ground colour), rebuilt
// when its plants, light or tiles change. Every frame, plants within
// their fade distance are grouped by mesh and drawn with one instanced draw
// per mesh in DrawLayer::GAME (the foliage program). Each plant fades out
// with a dither over the last fifth of a distance that grows with its
// height (fadeDistance): tufts and flowers end near 45-55 m, bushes at
// FadeEnd, inside the detailed-chunk radius. Small plants are the most
// numerous, so this bounds per-frame instance work.
class FloraRenderer {
   public:
    static constexpr float FadeEnd = 100;
    // Fade distance of a plant `height` metres tall.
    static float fadeDistance(float height) {
        return std::clamp(35.f + 35.f * height, 45.f, FadeEnd);
    }
    static constexpr int InstanceStride = 64;  // four vec4, foliage.vert

    FloraRenderer(ChunkManager& chunks, const IAssets& assets,
                  const World& world, IRenderer& renderer);
    ~FloraRenderer();
    FloraRenderer(const FloraRenderer&) = delete;
    FloraRenderer& operator=(const FloraRenderer&) = delete;

    // now: seconds (wind). today: game day for growth stages (day 0 until
    // the world clock exists).
    void render(RenderTarget& target, const Camera& camera,
                const Frustum& frustum, LightRenderer& lights, double now,
                double today = 0);

    size_t drawnPlants() const { return drawn; }
    size_t drawCalls() const { return calls; }
    size_t meshCount() const { return meshes.size(); }
    // Main-thread time of the last render() (cache upkeep, grouping,
    // instance upload), in milliseconds.
    double cpuMilliseconds() const { return cpuMs; }

   private:
    struct MeshEntry {
        FloraMesh cpu;  // kept alive: the GPU mesh references its arrays
        MeshHandle gpu;
    };
    struct Plant {
        uint32_t mesh;
        double x, y, z;  // base, world (canonical X)
        float data[12];  // i_data0.w .. i_data3 except the position
        float fade;      // fade distance (also data[11])
    };
    struct ChunkPlants {
        std::vector<Plant> plants;
        bool valid = false;
    };

    static uint32_t meshKey(uint8_t species, int variant, FloraStage stage) {
        return uint32_t(species) << 16 | uint32_t(variant) << 8 |
               uint32_t(stage);
    }
    const MeshEntry* mesh(uint32_t key, int& budget);
    void rebuild(const ChunkCoord& coord, ChunkPlants& cache, double today);
    void invalidate(const ChunkCoord& coord);

    ChunkManager& chunks;
    const IAssets& assets;
    const World& world;
    IRenderer& renderer;
    FloraMeshBuilder builder;
    std::unordered_map<uint32_t, MeshEntry> meshes;
    std::unordered_map<ChunkCoord, ChunkPlants> caches;
    std::unordered_set<ChunkCoord> withPlants;
    std::deque<bgfx::InstanceDataBuffer> instances;  // stable addresses
    ShaderProperty albedo, normals, eyeUniform, params;
    std::string shader = "foliage";
    Signal<const ChunkCoord&>::Connection readyConnection, floraConnection,
        lightConnection, removedConnection;
    std::vector<float> speciesFade;  // by Flora id - 1
    double lastToday = 0;
    size_t drawn = 0, calls = 0;
    double cpuMs = 0;
};
