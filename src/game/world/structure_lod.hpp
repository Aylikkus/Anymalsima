#pragma once

#include <bx/math.h>

#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "chunk_mesher.hpp"
#include "render/frustrum.hpp"
#include "render/render_target.hpp"
#include "render/ui_context.hpp"
#include "surface_sampler.hpp"
#include "terrain_edits.hpp"

// Voxel proxies for built structures at distance: one mesh per structure
// slab (16x16x16) of every ColumnEdit in view. A structure is just its
// voxels, so it may span any number of slabs, chunk columns and regions;
// border faces look into neighbouring slabs, so no seam faces appear.
// Meshes are built on the pool from immutable edit snapshots and uploaded on
// the main thread. A rebuilt proxy replaces its old mesh only once ready.
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
    // Returns the uploaded bytes, at most about `budget` (one oversized mesh
    // may use a whole frame).
    size_t upload(IRenderer& renderer, size_t budget);
    // hidden(coord): the real chunk is drawn instead.
    template <typename Hidden>
    void draw(RenderTarget& target, const Frustum& frustum,
              const Camera& camera, const std::string* shader,
              const std::vector<ShaderProperty*>& uniforms, Hidden&& hidden) {
        for (auto& [coord, proxy] : proxies) {
            if (!proxy->drawable || !proxy->hasMesh || hidden(coord) ||
                !frustum.intersects(proxy->bounds))
                continue;
            // Meshes are slab-local (ChunkMesher); place them by the eye.
            auto transform =
                relativeTransform(camera, double(coord.x) * Chunk::SIZE_X,
                                  double(coord.y) * Chunk::SIZE_Y,
                                  double(coord.z) * Chunk::SIZE_Z);
            auto bound = uniforms;
            target.drawVertexBuffer(proxy->mesh.get(), shader, std::move(bound),
                                    false, std::move(transform),
                                    DrawLayer::GAME);
        }
    }
    size_t residentBytes() const { return resident; }
    size_t count() const { return proxies.size(); }
    // Wanted slabs without a current mesh.
    size_t backlog() const;

   private:
    struct Proxy {
        MeshHandle mesh;
        AABB bounds{};
        size_t bytes = 0;
        bool drawable = false;
        bool hasMesh = false;
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
    void collect();
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
    std::array<int, 3> chosenCell{INT32_MIN, 0, 0};
    // Wanted slabs, nearest first.
    std::vector<ChunkCoord> wanted;
    std::unordered_map<ChunkCoord, std::unique_ptr<Proxy>> proxies;
    std::vector<Job> jobs;
    uint64_t revision = 1;
    size_t resident = 0;
    static constexpr size_t MaxJobs = 2;
};
