#pragma once

#include <future>
#include <map>
#include <memory>
#include <optional>

#include "chunk_manager.hpp"
#include "detail_coverage.hpp"
#include "lod_frontier.hpp"
#include "render/render_target.hpp"

class TerrainLodManager {
   public:
    TerrainLodManager(const World& world, IAssets& assets, ThreadPool& pool);
    ~TerrainLodManager();
    void update(const Camera& camera, Vec2 viewport);
    void render(RenderTarget& target, IRenderer& renderer, ChunkManager& chunks,
                const Camera& camera, Vec2 viewport);
    bool drawsColumn(unsigned x, unsigned z) const;
    size_t residentBytes() const {
        return resident + (joinsInitialised ? JoinBudget : 0);
    }
    size_t patchCount() const { return frontier.current().size(); }
    size_t jobCount() const { return jobs.size(); }
    size_t backlog() const;
    size_t uploadedBytes() const { return uploaded; }

   private:
    struct Entry {
        MeshHandle mesh;
        bgfx::TextureHandle materialMap = BGFX_INVALID_HANDLE;
        ShaderProperty mapUniform;
        ShaderProperty patchUniform;
        std::optional<TerrainLod::Mesh> cpu;
        std::vector<TerrainLod::SurfaceBand> bands;
        AABB bounds{};
        size_t bytes = 0;
        bool drawable = false;
        ~Entry();
    };
    struct Job {
        TerrainLod::Patch patch;
        uint64_t revision;
        std::shared_ptr<std::atomic_bool> cancel;
        std::future<TerrainLod::Mesh> future;
    };
    void choose(const Camera& camera, Vec2 viewport);
    void collect();
    void trim();
    void upload(IRenderer& renderer);
    void updateCoverage(ChunkManager& chunks, const Camera& camera);
    void uploadCoverage();
    bool uploadJoins(std::span<const TerrainLod::Patch> layout,
                     IRenderer& renderer);
    const World& world;
    IAssets& assets;
    ThreadPool& pool;
    SurfaceSampler detailSampler;
    TerrainLod::DetailCoverage detailCoverage;
    TerrainLod::DetailCoverage::Columns surfaceBands;
    std::vector<TerrainLod::Patch> joinedLayout;
    uint64_t joinedCoverageVersion = UINT64_MAX;
    static constexpr size_t JoinBudget = TerrainLod::UploadBudget;
    MeshHandle detailJoins;
    bool joinsInitialised = false, joinsDrawable = false;
    std::string joinShader = "block";
    TerrainLod::Frontier frontier;
    std::map<TerrainLod::Patch, std::unique_ptr<Entry>> entries;
    std::map<std::array<int, 3>, float> errors;
    std::vector<Job> jobs;
    std::array<int, 4> lastFocus{-1, -1, -1, -1};
    bool coarse = true;
    bool refined = false;
    bool budgetPressure = false;
    float exactRadius = 128;
    size_t resident = 0, uploaded = 0;
    int detailX = 0, detailZ = 0;
    static constexpr int DetailSize = 32;
    std::array<uint8_t, DetailSize * DetailSize * 4> coverage{};
    bgfx::TextureHandle detailMap = BGFX_INVALID_HANDLE;
    ShaderProperty detailUniform, detailOrigin, textureArray, fogUniform;
    // Normal maps: stage 3 in terrain_lod, stage 1 in the block program.
    ShaderProperty patchNormals, joinNormals;
    // Joins use the block program but have no voxel light volume. A 1^3
    // dummy volume plus u_light_chunk.w = 0 selects full sky light.
    bgfx::TextureHandle joinLightTexture = BGFX_INVALID_HANDLE;
    ShaderProperty joinLightVolume, joinLightChunk, joinLightSlot;
    std::string shader = "terrain_lod";
};
