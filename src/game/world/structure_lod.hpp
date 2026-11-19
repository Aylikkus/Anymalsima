#pragma once

#include <bx/math.h>

#include <bitset>
#include <functional>
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
    // Chunks the detail pass draws (level 0 slabs there need no proxy mesh
    // while they stay well inside it). Set every frame by the owner; used by
    // the next update.
    using Drawn = std::function<bool(const ChunkCoord&)>;
    void setChunkDrawn(Drawn drawn, ChunkCoord centre, int radius) {
        chunkDrawn = std::move(drawn);
        drawnCentre = centre;
        drawnRadius = radius;
    }
    // hidden(coord): the real chunk is drawn instead. Retired proxies (a
    // level change) stay drawn until all their replacements are drawable;
    // the replacements wait meanwhile, so a level swap never shows a hole
    // nor both levels at once.
    template <typename Hidden>
    void draw(RenderTarget& target, const Frustum& frustum,
              const Camera& camera, const std::string* shader,
              const std::vector<ShaderProperty*>& uniforms, Hidden&& hidden) {
        releaseRetired(hidden);
        const float eyeX = camera.eye().x;
        forEachDrawn(hidden, [&](const ChunkCoord& coord, Proxy& p) {
            auto* proxy = &p;
            const int level = levelOf(coord);
            const auto base = baseOf(coord);
            const double span = spanOf(level);
            // Drawn at the copy nearest the eye (X wraps).
            const float x = drawTileX(base.x * span, span, eyeX);
            AABB bounds = proxy->bounds;
            bounds.maxX += x - bounds.minX;
            bounds.minX = x;
            if (!frustum.intersects(bounds)) return;
            // Meshes are slab-local (ChunkMesher, in cells); place them by
            // the eye.
            auto transform =
                relativeTransform(camera, x, base.y * span, base.z * span);
            scale(transform, level);
            auto bound = uniforms;
            target.drawVertexBuffer(proxy->mesh.get(), shader, std::move(bound),
                                    false, std::move(transform),
                                    DrawLayer::GAME);
        });
    }
    // Shadow caster pass of one light cube face (LightShadows): slabs inside
    // the light's sphere and the face frustum (light-relative coordinates).
    template <typename Hidden>
    void drawShadow(RenderTarget& target, const Frustum& frustum,
                    const std::array<double, 3>& light, float radius,
                    const std::string* shader,
                    const std::vector<ShaderProperty*>& uniforms, uint16_t view,
                    Hidden&& hidden) {
        forEachDrawn(hidden, [&](const ChunkCoord& coord, Proxy& p) {
            auto* proxy = &p;
            const int level = levelOf(coord);
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
                return;
            if (!frustum.intersects(AABB{float(lx), float(ly), float(lz),
                                         float(hx), float(hy), float(hz)}))
                return;
            auto transform = LightShadowMath::translation(
                x - light[0], y - light[1], z - light[2]);
            scale(transform, level);
            target.drawShadowCaster(proxy->mesh.get(), shader,
                                    std::vector(uniforms), false,
                                    std::move(transform), view);
        });
    }
    size_t residentBytes() const {
        return resident + voxelResident + retiredBytes;
    }
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
    size_t retiredCount() const { return retired.size(); }
    // Debug: structure voxel slabs within `radius` of the eye that show
    // nothing this frame (no drawn proxy, no drawn chunk): visible holes.
    template <typename Hidden>
    size_t gaps(Vec3 eye, float radius, Hidden&& hidden) const {
        if (!voxels) return 0;
        size_t out = 0;
        for (const auto& [coord, slab] : *voxels) {
            if (slab.enclosed) continue;
            const int level = levelOf(coord);
            const auto base = baseOf(coord);
            const float span = float(spanOf(level));
            const float x0 = drawTileX(base.x * span, span, eye.x);
            const float y0 = base.y * span, z0 = base.z * span;
            const float dx = std::max({x0 - eye.x, 0.f, eye.x - x0 - span});
            const float dy = std::max({y0 - eye.y, 0.f, eye.y - y0 - span});
            const float dz = std::max({z0 - eye.z, 0.f, eye.z - z0 - span});
            if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
            if (level == 0 && hidden(coord)) continue;
            // Drawn, or still covered by the retired proxy it replaces.
            if (shadowedByRetired(coord)) continue;
            if (const auto it = proxies.find(coord);
                it != proxies.end() && it->second->drawable)
                continue;
            ++out;
        }
        return out;
    }
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

    // Keys of other levels overlapping `coord`'s box: its coarser parents
    // and its finer children.
    template <typename Visit>
    static void forEachOverlap(const ChunkCoord& coord, Visit&& visit) {
        const int level = levelOf(coord);
        const auto base = baseOf(coord);
        for (int l = level + 1; l <= MaxLevel; ++l) {
            const int shift = l - level;
            visit(withLevel({base.x >> shift, base.y >> shift, base.z >> shift},
                            l));
        }
        for (int l = level - 1; l >= 0; --l) {
            const unsigned n = 1u << (level - l);
            for (unsigned z = 0; z < n; ++z)
                for (unsigned y = 0; y < n; ++y)
                    for (unsigned x = 0; x < n; ++x)
                        visit(withLevel(
                            {base.x * n + x, base.y * n + y, base.z * n + z},
                            l));
        }
    }
    // A wanted proxy whose area a retired proxy still draws.
    bool shadowedByRetired(const ChunkCoord& coord) const {
        if (retired.empty()) return false;
        bool found = false;
        forEachOverlap(coord, [&](const ChunkCoord& c) {
            found = found || retired.contains(c);
        });
        return found;
    }
    // Releases retired proxies whose replacements all draw something (a
    // proxy, their real chunk, or nothing because they hold no visible
    // voxels).
    template <typename Hidden>
    void releaseRetired(Hidden&& hidden) {
        for (auto it = retired.begin(); it != retired.end();) {
            bool ready = true;
            forEachOverlap(it->first, [&](const ChunkCoord& c) {
                if (!ready || !voxels) return;
                const auto slab = voxels->find(c);
                if (slab == voxels->end() || slab->second.enclosed) return;
                if (levelOf(c) == 0 && hidden(c)) return;
                const auto p = proxies.find(c);
                ready = p != proxies.end() && p->second->drawable;
            });
            if (!ready) {
                ++it;
                continue;
            }
            retiredBytes -= it->second->bytes;
            it = retired.erase(it);
        }
    }
    // Proxies to draw this frame: current ones not waiting for a retired
    // proxy over them, then the retired ones.
    template <typename Hidden, typename Fn>
    void forEachDrawn(Hidden&& hidden, Fn&& fn) {
        for (auto& [coord, proxy] : proxies) {
            if (!proxy->drawable || !proxy->hasMesh ||
                (levelOf(coord) == 0 && hidden(coord)) ||
                shadowedByRetired(coord))
                continue;
            fn(coord, *proxy);
        }
        for (auto& [coord, proxy] : retired) {
            if (!proxy->hasMesh || (levelOf(coord) == 0 && hidden(coord)))
                continue;
            fn(coord, *proxy);
        }
    }
    struct Job {
        ChunkCoord slab;
        uint64_t revision;
        std::future<ChunkMesh> future;
    };
    // A structure's rasterized slabs, merged with the ones it keeps on the
    // worker (a massive tree holds thousands).
    struct Built {
        std::shared_ptr<const VoxelSlabs> slabs;
        std::vector<ChunkCoord> changed;  // slabs added, replaced or gone
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
            std::shared_ptr<const std::unordered_set<ChunkCoord>> plan;
            std::future<Built> future;
        };
        std::optional<Build> job;
        // Where the wanted source may hold voxels, computed on the pool.
        structure_voxels::SourcePtr occupancyFor;
        structure_voxels::OccupancyPtr occupancy;
        std::optional<std::future<structure_voxels::OccupancyPtr>> occupancyJob;
        // Inputs of the last plan: nothing to do while they are the same.
        std::array<int, 3> plannedCell{INT32_MIN, 0, 0};
        structure_voxels::SourcePtr plannedWanted, plannedBuilt;
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
    // Proxies that left the plan (a level change), drawn until their
    // replacements are ready. Not counted against the budgets, so they never
    // block their own replacements.
    std::unordered_map<ChunkCoord, std::unique_ptr<Proxy>> retired;
    size_t retiredBytes = 0;
    Drawn chunkDrawn;
    ChunkCoord drawnCentre{};
    int drawnRadius = 0;
    // Level 0 slabs drawn as real chunks this far inside the detail box get
    // no proxy mesh (the player must walk this many chunks first).
    static constexpr int DrawnMargin = 2;
    bool coveredByChunks(const ChunkCoord& c) const;
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
    static constexpr size_t MaxJobs = 4;
    static constexpr size_t MaxVoxelJobs = 2;
};
