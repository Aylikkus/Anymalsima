#pragma once

#include <bx/math.h>

#include <bitset>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_mesher.hpp"
#include "light_shadow_math.hpp"
#include "render/frustrum.hpp"
#include "render/render_target.hpp"
#include "render/ui_context.hpp"
#include "structure_voxels.hpp"
#include "surface_sampler.hpp"
#include "terrain_edits.hpp"

// Voxel proxies for built structures at distance: one mesh per structure
// slab (16x16x16) of every ColumnEdit in view. A structure is just its
// voxels, so it may span any number of slabs, chunk columns and regions;
// border faces look into neighbouring slabs, so no seam faces appear.
// Meshes are built on the pool from immutable edit snapshots and uploaded on
// the main thread. A rebuilt proxy replaces its old mesh only once ready.
//
// Generated structures (massive trees today; docs/handoff/structures.md)
// are drawn the same way: they never reach the edit snapshots, so
// setStructures lists their voxel sources (structure_voxels::Source). Each
// is planned per slab around the eye (full tiles near, cells of 2 or 4
// tiles far: structure_voxels::plan), rasterized on the pool and merged
// into the slabs' voxels. Only slabs whose level changes are rebuilt as the
// eye moves. Slabs whose real chunk is drawn hide, as for edits. Coarse
// proxies are keyed with their level in the high bits of y
// (levelOf/baseOf) and their coordinates count slabs of 16 cells.
class StructureLod {
   public:
    StructureLod(const World& world, IAssets& assets, ThreadPool& pool,
                 std::shared_ptr<SurfaceInputCache> inputs, size_t budget,
                 float viewDistance);
    ~StructureLod();
    StructureLod(const StructureLod&) = delete;
    StructureLod& operator=(const StructureLod&) = delete;

    // A column's edit changed: it and its 4 side columns need new meshes.
    void edited(uint64_t column);
    void update(Vec3 eye, std::shared_ptr<const EditMap> snapshot);
    // The generated structures in view (main thread; sources with an
    // unchanged key and revision are kept).
    void setStructures(std::vector<structure_voxels::SourcePtr> sources);
    // Returns the uploaded bytes, at most about `budget` (one oversized mesh
    // may use a whole frame).
    size_t upload(IRenderer& renderer, size_t budget);
    // hidden(coord): the real chunk is drawn instead.
    template <typename Hidden>
    void draw(RenderTarget& target, const Frustum& frustum,
              const Camera& camera, const std::string* shader,
              const std::vector<ShaderProperty*>& uniforms, Hidden&& hidden) {
        const float eyeX = camera.eye().x;
        for (auto& [coord, proxy] : proxies) {
            const int level = levelOf(coord);
            const auto base = baseOf(coord);
            const double span = spanOf(level);
            // Drawn at the copy nearest the eye (X wraps).
            const float x = drawTileX(base.x * span, span, eyeX);
            AABB bounds = proxy->bounds;
            bounds.maxX += x - bounds.minX;
            bounds.minX = x;
            if (!proxy->drawable || !proxy->hasMesh ||
                (level == 0 && hidden(coord)) || !frustum.intersects(bounds))
                continue;
            // Meshes are slab-local (ChunkMesher, in cells); place them by
            // the eye.
            auto transform =
                relativeTransform(camera, x, base.y * span, base.z * span);
            scale(transform, level);
            auto bound = uniforms;
            target.drawVertexBuffer(proxy->mesh.get(), shader, std::move(bound),
                                    false, std::move(transform),
                                    DrawLayer::GAME);
        }
    }
    // Shadow caster pass of one light cube face (LightShadows): slabs inside
    // the light's sphere and the face frustum (light-relative coordinates).
    template <typename Hidden>
    void drawShadow(RenderTarget& target, const Frustum& frustum,
                    const std::array<double, 3>& light, float radius,
                    const std::string* shader,
                    const std::vector<ShaderProperty*>& uniforms, uint16_t view,
                    Hidden&& hidden) {
        for (auto& [coord, proxy] : proxies) {
            const int level = levelOf(coord);
            if (!proxy->drawable || !proxy->hasMesh ||
                (level == 0 && hidden(coord)))
                continue;
            const auto base = baseOf(coord);
            const double span = spanOf(level);
            const double x = drawTileX(base.x * span, span, float(light[0]));
            const double y = base.y * span;
            const double z = base.z * span;
            const auto& b = proxy->bounds;
            const double lx = x - light[0], ly = b.minY - light[1],
                         lz = b.minZ - light[2];
            const double hx = lx + (b.maxX - b.minX), hy = b.maxY - light[1],
                         hz = b.maxZ - light[2];
            if (!LightShadowMath::reaches(lx, ly, lz, hx, hy, hz, radius))
                continue;
            if (!frustum.intersects(AABB{float(lx), float(ly), float(lz),
                                         float(hx), float(hy), float(hz)}))
                continue;
            auto transform = LightShadowMath::translation(
                x - light[0], y - light[1], z - light[2]);
            scale(transform, level);
            target.drawShadowCaster(proxy->mesh.get(), shader,
                                    std::vector(uniforms), false,
                                    std::move(transform), view);
        }
    }
    size_t residentBytes() const { return resident + voxelResident; }
    // World X of chunk column cx at its copy nearest refX.
    float drawX(unsigned cx, float refX) const {
        return drawTileX(double(cx) * Chunk::SIZE_X, Chunk::SIZE_X, refX);
    }
    // World X of a slab starting at tile x, `span` tiles wide, at its copy
    // nearest refX.
    float drawTileX(double x, double span, float refX) const {
        const int period = world.wrap().tiles;
        if (period <= 0) return float(x);
        // A whole number of periods, so neighbouring slabs stay aligned.
        const double image = world.wrap().imageX(x + span / 2, refX) - span / 2;
        return float(x + std::round((image - x) / period) * period);
    }
    // Proxy keys of coarse structure voxels (see the class comment).
    static constexpr int MaxLevel = structure_voxels::MaxLevel;
    static int levelOf(const ChunkCoord& c) {
        return structure_voxels::levelOf(c);
    }
    static ChunkCoord baseOf(ChunkCoord c) {
        return structure_voxels::baseOf(c);
    }
    static ChunkCoord withLevel(ChunkCoord c, int level) {
        return structure_voxels::withLevel(c, level);
    }
    // Tiles per side of a slab of the level.
    static double spanOf(int level) { return double(Chunk::SIZE_X << level); }
    static void scale(std::array<float, 16>& m, int level) {
        const float s = float(1 << level);
        m[0] *= s;
        m[5] *= s;
        m[10] *= s;
    }
    size_t count() const { return proxies.size(); }
    // Slabs holding structure voxels, all levels or one (debug).
    size_t voxelSlabCount(int level = -1) const;
    using VoxelSlab = structure_voxels::Slab;
    using VoxelSlabs = structure_voxels::Slabs;
    // Wanted slabs without a current mesh, and structures whose planned
    // slabs are not rasterized yet.
    size_t backlog() const;

   private:
    struct Proxy {
        MeshHandle mesh;
        AABB bounds{};
        size_t bytes = 0;
        bool drawable = false;
        bool hasMesh = false;
        // Holds structure voxels: counts toward voxelBudget.
        bool voxels = false;
        // Edits changed after this mesh was built.
        uint64_t staleRevision = 0, builtRevision = 0;
        std::optional<ChunkMesh> cpu;
        uint64_t cpuRevision = 0;
        bool stale() const { return builtRevision < staleRevision; }
    };
    struct Job {
        ChunkCoord slab;
        uint64_t revision;
        std::future<ChunkMesh> future;
    };

    // One generated structure's voxels, planned per slab around the eye.
    struct Structure {
        structure_voxels::SourcePtr wanted;       // null: no longer in view
        structure_voxels::SourcePtr built;        // source of `slabs`
        std::shared_ptr<const VoxelSlabs> slabs;  // non-empty ones
        // Slab keys rasterized for `built` (empty ones too): the plan.
        std::unordered_set<ChunkCoord> covered;
        struct Build {
            structure_voxels::SourcePtr source;
            std::unordered_set<ChunkCoord> plan;
            std::future<VoxelSlabs> future;
            // Keys rasterized by the job (empty results included).
            std::unordered_set<ChunkCoord> rebuilt;
            // The built slabs outside `rebuilt` stay (same voxels there).
            bool keep = false;
        };
        std::optional<Build> job;
        // A plan with slabs to rasterize, waiting for a job slot.
        std::vector<structure_voxels::Planned> pending;
        std::unordered_set<ChunkCoord> pendingPlan;
        bool pendingKeep = false;
        float distance = 0;  // nearest planned slab, at the last plan
    };

    // Plans every structure's slabs for the eye (only after it moved, or
    // a source changed) and lists the slabs to rasterize.
    void planStructures(Vec3 eye);
    void collect();
    void collectStructures();
    void requestStructures();
    // Rebuilds the merged voxel map and marks the proxies of `changed`
    // slabs (and their neighbours, whose border faces they hide) stale.
    void mergeVoxels(const std::vector<ChunkCoord>& changed);
    void choose(Vec3 eye);
    void request();

    const World& world;
    IAssets& assets;
    ThreadPool& pool;
    std::shared_ptr<SurfaceInputCache> inputs;
    ChunkMesher mesher;
    size_t budget;
    float viewDistance;
    std::shared_ptr<const EditMap> snapshot;
    std::shared_ptr<const EditMap> chosenFor;
    std::unordered_map<uint64_t, Structure> structures;  // by Source::key
    std::shared_ptr<const VoxelSlabs> voxels;  // all structures, merged
    std::shared_ptr<const VoxelSlabs> voxelsChosenFor;
    std::array<int, 3> plannedCell{INT32_MIN, 0, 0};
    bool replan = true;  // a source changed or a build finished
    std::array<int, 3> chosenCell{INT32_MIN, 0, 0};
    // Wanted slabs, nearest first.
    std::vector<ChunkCoord> wanted;
    std::unordered_map<ChunkCoord, std::unique_ptr<Proxy>> proxies;
    std::vector<Job> jobs;
    uint64_t revision = 1;
    size_t resident = 0;
    // Structure voxels have their own cap: one massive tree is a few MiB
    // of faces.
    size_t voxelBudget;
    size_t voxelResident = 0;
    size_t& residentOf(const Proxy& proxy) {
        return proxy.voxels ? voxelResident : resident;
    }
    size_t budgetOf(const Proxy& proxy) const {
        return proxy.voxels ? voxelBudget : budget;
    }
    static constexpr size_t MaxJobs = 2;
    static constexpr size_t MaxVoxelJobs = 2;
};
