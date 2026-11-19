#pragma once

#include <deque>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "block_light_propagator.hpp"
#include "chunk_manager.hpp"
#include "light_renderer.hpp"
#include "render/frustrum.hpp"
#include "render/render_target.hpp"
#include "structure_lod.hpp"
#include "tree_foliage_builder.hpp"

class SkyHeightMap;

// Main-thread GPU side of tree crowns (docs/handoff/trees.md, "Rendering").
// Wood blocks are ordinary chunk geometry; this draws the rest: foliage and
// branches thinner than a block, with the foliage program in tree mode
// (u_foliage.z = 1: wind by the vertex weight, no root darkening).
//
// Trees come from the built region tree sets (ChunkManager::forEachTree)
// and the landmark cache, refreshed periodically. Trees anchored in the
// detailed chunks around the camera target draw their crowns (near or far
// mesh) next to their wood blocks; others are drawn whole as
// distant meshes (crown and bark prisms for the wood), ordinary trees up
// to DistantEnd and landmarks up to the view distance. Ordinary trees share
// meshes per (species, variant, growth bucket, detail), drawn instanced
// with the variant's quarter-turn yaw; landmarks and trees of their own get
// their own mesh. Meshes are built on demand within a per-frame budget and
// dropped when unused.
class TreeRenderer {
   public:
    static constexpr float FadeEnd = 128;
    // Ordinary trees beyond the detailed chunks are drawn (distant meshes,
    // wood as bark prisms) up to here; landmarks up to the view distance.
    static constexpr float DistantEnd = 600;
    // Beyond the distant meshes, forests are billboards (one instanced draw
    // per species) up to here or the view distance, cross-fading with the
    // meshes between 0.8 and 1 DistantEnd. Farther regions draw a thinner,
    // scaled-up share of their trees.
    static constexpr float BillboardEnd = 2048;
    // Within this distance (plus half the crown) the near mesh is drawn.
    static constexpr float NearDistance = 40;
    static constexpr int InstanceStride = 64;  // foliage.vert

    // pool: crown meshes are built on it (a massive crown takes tens of
    // milliseconds); the previous mesh of a tree is drawn meanwhile.
    TreeRenderer(ChunkManager& chunks, const IAssets& assets,
                 const World& world, IRenderer& renderer, ThreadPool& pool);
    ~TreeRenderer();
    TreeRenderer(const TreeRenderer&) = delete;
    TreeRenderer& operator=(const TreeRenderer&) = delete;

    // now: seconds (wind). today: game day (growth and death).
    void render(RenderTarget& target, const Camera& camera,
                const Frustum& frustum, LightRenderer& lights, double now,
                double today, ShaderProperty& water);

    size_t drawnTrees() const { return drawn; }
    size_t drawCalls() const { return calls; }
    size_t meshCount() const { return meshes.size(); }
    size_t knownTrees() const { return trees.size(); }
    size_t drawnBillboards() const { return billboardsDrawn; }
    // Landmarks whose block wood is drawn as distant voxels
    // (TerrainLodManager::setStructures): they get crowns without bark
    // prisms. landmarkVoxels lists their wood as structure voxels, its age
    // quantized to growth buckets.
    void setVoxelLandmarks(bool on) { voxelLandmarks = on; }
    // Whether the caller draws this chunk's blocks this frame (detail
    // coverage, radius and an uploaded mesh, possibly empty). A tree with
    // voxel wood drops its bark prisms only when every chunk of its wood is
    // drawn, so the hand-over never shows a gap. Default: an uploaded mesh.
    using ChunkDrawn = std::function<bool(const ChunkCoord&)>;
    void setChunkDrawn(ChunkDrawn drawn) { chunkDrawn = std::move(drawn); }
    // The map must outlive the renderer.
    void setSkyLight(const SkyHeightMap& map) { skyMap = &map; }
    // Optional block light chroma (coloured lamps tint crowns).
    // The propagator must outlive the renderer.
    void setBlockLight(const BlockLightPropagator* light) {
        blockLight = light;
    }
    // Pass the voxel wood day and disable age quantization for sky tops.
    std::vector<structure_voxels::SourcePtr> landmarkVoxels(
        double today, bool quantizeAge = true) const;
    double cpuMilliseconds() const { return cpuMs; }

   private:
    // A crown: one mesh per part (16-bit indices; massive crowns have
    // several), built on the pool.
    struct MeshEntry {
        // Kept alive: the GPU meshes reference their arrays.
        std::vector<FloraMesh> cpu;
        std::vector<MeshHandle> gpu;
        std::optional<std::future<std::vector<FloraMesh>>> job;
        uint64_t lastUsed = 0;
        // Built, possibly empty (a crown with nothing left draws nothing,
        // rather than the tree's previous mesh).
        bool built = false;
        bool ready() const { return !job && built; }
    };
    // Billboards: one small mesh per species, built in place.
    struct BillboardEntry {
        FloraMesh cpu;
        MeshHandle gpu;
    };
    struct TreeDraw {
        TreeInstancePtr tree;
        float sun = 1;
        // Block (lamp) light by the trunk near the ground and in the crown:
        // the shader blends them by height, so a lamp at a tree's foot
        // lights its lower part, not the whole tree.
        float blockBase = 0, blockCrown = 0;
        Color3 ground;
        // Chroma of the brightest block light sampled (encodeChroma bytes).
        std::array<uint8_t, 2> blockChroma{128, 128};
    };

    // Collects the trees of the built region sets within DistantEnd of the
    // eye, and landmarks within the view distance.
    void gather(double eyeX, double eyeZ, double viewDistance);
    TreeDraw describe(const TreeInstancePtr& tree);
    // Billboard regions within range, nearest first; starts their jobs.
    void gatherBillboards(double eyeX, double eyeZ, double range);
    void drawBillboards(RenderTarget& target, const Camera& camera,
                        const Frustum& frustum,
                        const std::vector<ShaderProperty*>& base, double ex,
                        double ey, double ez, double today);
    const BillboardEntry* billboardMesh(uint8_t species);
    // The crown mesh, or null while it is built (`starts` limits new jobs
    // per frame).
    const MeshEntry* mesh(uint64_t key, const TreeInstance& tree, int bucket,
                          tree_foliage::Detail detail, bool bare, int& starts);

    ChunkManager& chunks;
    const IAssets& assets;
    const World& world;
    IRenderer& renderer;
    std::unordered_map<uint64_t, TreeDraw> trees;  // by TreeInstance::key
    std::unordered_map<uint64_t, MeshEntry> meshes;
    std::unordered_map<uint8_t, BillboardEntry> billboardMeshes;
    // The crown mesh last drawn for each tree (by TreeInstance::key): drawn
    // while its next mesh (cut, growth step, detail change) is built.
    std::unordered_map<uint64_t, uint64_t> drawnMesh;
    ThreadPool& pool;
    // Meshes with a job in flight. Finished jobs are collected every frame
    // whether or not their key is still drawn: a key left behind by a cut,
    // a growth step or a detail change used to keep its job slot forever,
    // and with all slots taken no crown was rebuilt (cut trees kept their
    // old leaves until a stale key happened to be drawn again).
    std::vector<uint64_t> building;
    void collectMeshes();
    struct BillboardRegion {
        Vec2u region;
        ChunkManager::BillboardSet set;
        float low = 0, high = 0;  // anchor heights, for culling
        // Inside the mesh range without a tree set yet: drawn without the
        // near fade, so no ring of the forest is ever empty.
        bool fallback = false;
    };
    std::vector<BillboardRegion> billboardRegions;
    size_t billboardsDrawn = 0;
    uint64_t billboardsGathered = 0, billboardsRevision = 0;
    std::deque<bgfx::InstanceDataBuffer> instances;  // stable addresses
    ShaderProperty albedo, normals, eyeUniform, params;
    std::string shader = "foliage";
    int gatheredX = INT_MIN, gatheredZ = INT_MIN;
    uint64_t frame = 0, gatheredFrame = 0, gatheredRevision = ~0ULL,
             gatheredSets = ~0ULL;
    // True until every chunk of a detailed tree's wood is drawn (see
    // setChunkDrawn): its crown is drawn whole (distant mesh with bark
    // prisms) meanwhile, so leaves never hang over missing wood.
    bool woodPending(const TreeInstance& tree) const;
    ChunkDrawn chunkDrawn;
    const SkyHeightMap* skyMap = nullptr;
    const BlockLightPropagator* blockLight = nullptr;
    size_t drawn = 0, calls = 0;
    double cpuMs = 0;
    bool voxelLandmarks = false;
};
