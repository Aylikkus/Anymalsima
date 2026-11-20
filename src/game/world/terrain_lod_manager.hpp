#pragma once

#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>

#include "chunk_manager.hpp"
#include "detail_coverage.hpp"
#include "light_shadows.hpp"
#include "lod_cover.hpp"
#include "render/render_target.hpp"
#include "structure_lod.hpp"
#include "terrain_edits.hpp"

// Distant terrain in three tiers: voxel chunks near the camera (owned by
// DetailCoverage), stepped spacing-1 patches inside the exact radius, and
// smooth quadtree patches beyond it. Patches are independent cached meshes:
// each frame draws the finest available patch for every part of the desired
// layout and falls back to an ancestor or to finer leftovers, so a layout
// change never waits for, or discards, unrelated patches.
class TerrainLodManager {
   public:
    // edits: gameplay terrain changes shown at distance; optional.
    TerrainLodManager(
        const World& world, IAssets& assets, ThreadPool& pool,
        const TerrainLod::Quality& quality = TerrainLod::Qualities[0],
        TerrainEdits* edits = nullptr);
    ~TerrainLodManager();
    void update(const Camera& camera, Vec2 viewport,
                std::span<const TileBox> casterDetail = {});
    void render(RenderTarget& target, IRenderer& renderer, ChunkManager& chunks,
                const Camera& camera, Vec2 viewport);
    ShaderProperty& waterUniform() { return water; }
    bool eyeUnderwater() const { return underwater; }
    // Uniforms appended to every distant terrain draw (lights): patches use
    // the terrain_lod program, joins and structures the block program. They
    // must outlive the manager's draws.
    void setExtraUniforms(std::vector<ShaderProperty*> patchUniforms,
                          std::vector<ShaderProperty*> blockUniforms) {
        extraUniforms = std::move(patchUniforms);
        extraBlockUniforms = std::move(blockUniforms);
    }
    // Increments whenever the drawn distant geometry changes (patch
    // selection, joins, structure meshes); shadow maps refresh on it.
    uint64_t drawnVersion() const { return drawnRevision; }
    // Shadow caster pass of one light cube face: the patches, joins and
    // structure slabs drawn last frame that reach into the face.
    void submitShadowCasters(RenderTarget& target,
                             const LightShadows::Face& face,
                             ChunkManager& chunks);
    bool drawsColumn(unsigned x, unsigned z) const;
    // Inside this frame's chunk radius, whether blocks draw instead of distant
    // terrain and structure voxels: a detailed column, or any chunk above the
    // column's surface band (distant terrain has nothing there).
    bool drawsChunk(const ChunkCoord& c) const;
    // Generated structures drawn as voxels at distance (massive trees today;
    // StructureLod::setStructures); ignored without edit tracking (no
    // structure proxies).
    void setStructures(std::vector<structure_voxels::SourcePtr> sources) {
        if (structures) structures->setStructures(std::move(sources));
    }
    bool drawsStructureVoxels() const { return structures != nullptr; }
    // Slabs of structure voxels, all levels or one.
    size_t voxelSlabs(int level = -1) const {
        return structures ? structures->voxelSlabCount(level) : 0;
    }
    size_t residentBytes() const {
        return resident + (joinsInitialised ? JoinBudget : 0) +
               (structures ? structures->residentBytes() : 0);
    }
    size_t structureCount() const {
        return structures ? structures->count() : 0;
    }
    // Merged meshes of coarse structure slabs.
    size_t structureGroups() const {
        return structures ? structures->groupCount() : 0;
    }
    // Debug: structure voxel slabs within `radius` of the eye that draw
    // nothing this frame (neither a proxy nor their real chunk).
    size_t structureGaps(const ChunkManager& chunks, const Camera& camera,
                         float radius) const;
    size_t patchCount() const { return drawn.size(); }
    size_t jobCount() const { return jobs.size(); }
    size_t backlog() const;
    size_t uploadedBytes() const { return uploaded; }
    const TerrainLod::Quality& settings() const { return quality; }

    // Sampler stage of u_cover_map in terrain_lod.frag.
    static constexpr uint8_t CoverStage = 6;

   private:
    struct Entry {
        MeshHandle mesh;
        bgfx::TextureHandle materialMap = BGFX_INVALID_HANDLE;
        // Ground cover (TerrainLod::Mesh::covers) at stage CoverStage, and
        // u_lod_cover: x = texels per side.
        bgfx::TextureHandle coverMap = BGFX_INVALID_HANDLE;
        ShaderProperty mapUniform;
        ShaderProperty coverUniform;
        ShaderProperty coverInfo;
        ShaderProperty patchUniform;
        // x: offset (0 or ±period) placing the patch at its copy nearest
        // the eye when the world wraps on X.
        ShaderProperty shiftUniform;
        std::optional<TerrainLod::Mesh> cpu;
        std::vector<TerrainLod::SurfaceBand> bands;
        AABB bounds{};
        size_t bytes = 0;
        bool drawable = false;
        // Frame in which the patch was last selected for drawing.
        uint64_t lastDrawn = 0;
        // Terrain edits changed under a drawable patch: it stays drawn until
        // a rebuilt mesh (built from edits revision >= staleRevision)
        // replaces it.
        bool stale = false;
        uint64_t staleRevision = 0;
        std::optional<TerrainLod::Mesh> replacement;
        uint64_t replacementRevision = 0;
        bool needsBuild() const {
            return stale &&
                   (!replacement || replacementRevision < staleRevision);
        }
        ~Entry();
    };
    using Entries = std::map<TerrainLod::Patch, std::unique_ptr<Entry>>;
    struct Job {
        TerrainLod::Patch patch;
        uint64_t editRevision = 0;
        std::shared_ptr<std::atomic_bool> cancel;
        std::future<TerrainLod::Mesh> future;
    };
    void choose(const Camera& camera, Vec2 viewport);
    void collect();
    void request();
    void upload(IRenderer& renderer);
    void uploadReplacements(IRenderer& renderer);
    // Creates the GPU mesh, material map and uniforms of an entry.
    bool createGpu(Entry& entry, const TerrainLod::Patch& patch,
                   const TerrainLod::Mesh& cpu, IRenderer& renderer);
    void editChanged(uint64_t column);
    void selectDrawn();
    bool wanted(const TerrainLod::Patch& patch) const;
    bool isDrawable(const TerrainLod::Patch& patch) const;
    // Inside the world and the view distance at the last selection.
    bool inView(const TerrainLod::Patch& patch) const;
    // Evicts unwanted cached patches, least recently drawn first, until
    // `bytes` more fit in the GPU budget. What was drawn in the previous
    // frame is kept, except finer leftovers inside `incoming`, a desired leaf
    // uploaded in the same frame that replaces them on screen.
    bool makeRoom(size_t bytes, const TerrainLod::Patch* incoming = nullptr);
    void erase(Entries::iterator it);
    void markDrawable(const TerrainLod::Patch& patch, int delta);
    // Not enough budget: select a coarser layout once per selection.
    void pressure();
    void updateCoverage(ChunkManager& chunks, const Camera& camera);
    void uploadCoverage();
    void uploadJoins(IRenderer& renderer);
    size_t budget() const { return quality.gpuBudget - JoinBudget; }

    const World& world;
    // World width in tiles when distant terrain wraps on X, else 0 (worlds
    // whose width is not a whole number of LOD roots stay bounded at
    // distance).
    int periodX = 0;
    IAssets& assets;
    ThreadPool& pool;
    const TerrainLod::Quality quality;
    TerrainEdits* edits;
    Signal<uint64_t>::Connection editConnection;
    std::shared_ptr<SurfaceInputCache> inputs;
    SurfaceSampler detailSampler;
    // Built structures at distance; present when edits are tracked.
    std::unique_ptr<StructureLod> structures;
    ChunkCoord detailCentre{};
    TerrainLod::DetailCoverage detailCoverage;
    TerrainLod::DetailCoverage::Columns surfaceBands;
    std::vector<TerrainLod::Patch> joinedLayout;
    uint64_t joinedCoverageVersion = UINT64_MAX;
    static constexpr size_t JoinBudget = TerrainLod::UploadBudget;
    MeshHandle detailJoins;
    bool joinsInitialised = false, joinsDrawable = false;
    std::string joinShader = "block";

    // Desired leaves, their strict ancestors, and the root patches (always
    // kept: the coarse fallback for every part of the view).
    std::vector<TerrainLod::Patch> desired;
    std::set<TerrainLod::Patch> desiredSet, splitSet, roots;
    // Request/upload order: roots first, then desired leaves near to far.
    std::vector<TerrainLod::Patch> queue;
    Entries entries;
    // Drawable entries strictly below each patch; bounds the fallback search.
    std::map<TerrainLod::Patch, int> drawableBelow;
    std::map<std::array<int, 3>, float> errors;
    bool errorsChanged = false;
    std::vector<Job> jobs;
    size_t maxJobs = 2;
    std::array<int, 4> lastFocus{-1, -1, -1, -1};
    float errorScale = 1, selectedScale = 0;
    Vec3 selectionEye{0, 0, 0};
    std::vector<TerrainLod::Patch> drawn;
    uint64_t frame = 1;
    size_t resident = 0, uploaded = 0;
    int detailX = 0, detailZ = 0;
    static constexpr int DetailSize = 32;
    std::array<uint8_t, DetailSize * DetailSize * 4> coverage{};
    bgfx::TextureHandle detailMap = BGFX_INVALID_HANDLE;
    ShaderProperty detailUniform, detailOrigin, textureArray, fogUniform;
    ShaderProperty water;
    bool underwater = false;
    bool debugEdges = false;
    // Normal maps: stage 3 in terrain_lod, stage 1 in the block program.
    ShaderProperty patchNormals, joinNormals;
    // Joins use the block program but have no voxel light volume. A 1^3
    // dummy volume plus u_light_chunk.w = 0 selects full sky light.
    bgfx::TextureHandle joinLightTexture = BGFX_INVALID_HANDLE;
    // Zero ground-cover density for the same draws (block.frag samples it).
    bgfx::TextureHandle joinCoverTexture = BGFX_INVALID_HANDLE;
    ShaderProperty joinLightVolume, joinLightChunk, joinLightSlot,
        joinCoverVolume;
    std::string shader = "terrain_lod";
    std::vector<ShaderProperty*> extraUniforms;
    std::vector<ShaderProperty*> extraBlockUniforms;
    uint64_t drawnRevision = 0;
    // u_detail_map at the light_shadow program's stage.
    ShaderProperty shadowDetail;
};
