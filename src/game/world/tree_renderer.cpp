#include "tree_renderer.hpp"

#include <chrono>
#include <cmath>

#include "foliage_light.hpp"
#include "ground_cover_climate.hpp"
#include "items/ground_cover.hpp"
#include "tree_wood_lod.hpp"

namespace {
// Crown mesh jobs started per frame, and running at once (a forest needs
// only a few dozen shared meshes; a massive crown takes tens of ms).
constexpr int MeshBuildsPerFrame = 8;
constexpr size_t MaxMeshJobs = 8;
// Tree list refresh, frames (also when the eye changes chunk column).
constexpr uint64_t GatherInterval = 120;
// Meshes unused this many frames are released.
constexpr uint64_t MeshLifetime = 600;
constexpr Color3 BareGround{0.40f, 0.32f, 0.22f};
constexpr float QuarterTurn = 1.57079632679f;
}  // namespace

TreeRenderer::TreeRenderer(ChunkManager& chunks, const IAssets& assets,
                           const World& world, IRenderer& renderer,
                           ThreadPool& pool)
    : chunks(chunks),
      assets(assets),
      world(world),
      renderer(renderer),
      pool(pool) {
    albedo.initTexture("u_texture_array", 0, assets.getFoliageAlbedoArray());
    normals.initTexture("u_normal_array", 1, assets.getFoliageNormalArray());
    eyeUniform.initVec4("u_world_eye", {0, 0, 0, 4096});
    params.initVec4("u_foliage", {0, 1, 1, 0});
}

TreeRenderer::TreeDraw TreeRenderer::describe(const TreeInstancePtr& tree) {
    TreeDraw draw{tree, 1, 0, 0, BareGround};
    const auto& k = *tree->skeleton;
    const auto& a = tree->anchor;
    // Light of the tree: the brightest air tiles among rings of samples
    // around the trunk. Sky light from the crown and above it; block light
    // separately near the ground (by the trunk) and through the crown. New
    // chunks start with a sky-light estimate (ChunkManager::
    // estimateSkyLight), so open air reads full sky even before its
    // sunlight sweep. No air sampled at all means "unknown": full sky until
    // a later gather finds light.
    const int height = std::max(2, int(std::ceil(k.boundsMax.y)));
    const int crownLow = a.y + 1 + std::max(2, int(k.height * 0.4f));
    const int crownHigh = a.y + 1 + height;
    const int ring = int(k.crownRadius * 0.6f + k.trunkRadius) + 1;
    const int near = int(k.trunkRadius) + 1;
    float sun = 0, blockBase = 0, blockCrown = 0;
    bool air = false;
    // Colour of the brightest block light sampled (one per tree).
    int brightest = 0;
    std::array<int, 3> brightestAt{};
    const auto sample = [&](int y, int r, float& block, bool sky) {
        for (const auto [dx, dz] :
             {std::pair{r, 0}, std::pair{-r, 0}, std::pair{0, r},
              std::pair{0, -r}, std::pair{r, r}, std::pair{-r, r},
              std::pair{r, -r}, std::pair{-r, -r}}) {
            const Tile* t = chunks.tileAt(a.x + dx, y, a.z + dz);
            if (!t || !t->isAir()) continue;
            air = true;
            const int level = t->lightLevel.artificial_light;
            block = std::max(block, level / 15.f);
            if (level > brightest) {
                brightest = level;
                brightestAt = {a.x + dx, y, a.z + dz};
            }
            if (sky) sun = std::max(sun, t->lightLevel.sun_exposure / 15.f);
        }
    };
    for (int y = a.y + 1; y <= a.y + 3; ++y) sample(y, near, blockBase, false);
    for (const int y : {crownLow, (crownLow + crownHigh) / 2, crownHigh})
        sample(y, ring, blockCrown, true);
    sample(crownHigh + 1, ring, blockCrown, true);
    if (air) {
        draw.sun = sun;
        draw.blockBase = blockBase;
        draw.blockCrown = blockCrown;
        if (brightest > 0)
            draw.blockChroma =
                foliage_light::at(chunks, blockLight, brightestAt[0],
                                  brightestAt[1], brightestAt[2]);
    }
    // Ground colour under the tree: its cover's climate tint.
    if (const Tile* ground = chunks.tileAt(a.x, a.y, a.z);
        ground && ground->cover && assets.hasStorage<GroundCover>()) {
        const auto& covers = assets.getStorage<GroundCover>();
        if (ground->cover <= covers.items.size()) {
            const auto [h, t] =
                ground_cover_climate::at(world, a.x + 0.5, a.z + 0.5);
            const auto tint = covers.get(ground->cover).tint_gradient.at(h, t);
            const float d = ground->coverDensity() / 15.f;
            draw.ground = {BareGround.r + (tint.r - BareGround.r) * d,
                           BareGround.g + (tint.g - BareGround.g) * d,
                           BareGround.b + (tint.b - BareGround.b) * d};
        }
    }
    return draw;
}

void TreeRenderer::gather(double eyeX, double eyeZ, double viewDistance) {
    const auto wrap = chunks.wrap();
    std::unordered_map<uint64_t, TreeDraw> next;
    const auto add = [&](const TreeInstancePtr& tree, double range) {
        const double dx = wrap.deltaX(tree->anchor.x + 0.5, eyeX);
        const double dz = tree->anchor.z + 0.5 - eyeZ;
        const auto* rule = chunks.treeSpecies().find(tree->species);
        const double reach = range + (rule ? rule->species.extent() : 0);
        if (dx * dx + dz * dz > reach * reach) return;
        auto draw = TreeDraw{tree, 1, 0, 0, BareGround};
        // Light and ground come from loaded tiles: only near trees have
        // them (distant ones keep full sky).
        if (dx * dx + dz * dz < double(FadeEnd + 48) * (FadeEnd + 48))
            draw = describe(tree);
        next.insert_or_assign(tree->key, draw);
    };
    chunks.forEachTree([&](const TreeInstancePtr& t) { add(t, DistantEnd); });
    // Every region whose trees can reach DistantEnd needs its set, built
    // or not by chunk loading.
    const auto w = wrap;
    const int tiles = TerrainTuning::RegionX;
    const double range = DistantEnd + chunks.treeSpecies().maxExtent();
    const int reach = int(std::ceil(range / tiles)) + 1;
    const int64_t ex =
        int64_t(std::floor((w.enabled() ? w.wrapX(eyeX) : eyeX) / tiles));
    const int64_t ez = int64_t(std::floor(eyeZ / tiles));
    std::vector<std::pair<double, Vec2u>> wanted;
    for (int64_t dz = -reach; dz <= reach; ++dz)
        for (int64_t dx = -reach; dx <= reach; ++dx) {
            const int64_t rz = ez + dz;
            int64_t rx = ex + dx;
            if (rz < 0 || rz >= chunks.worldHeightRegions()) continue;
            if (w.enabled())
                rx = w.wrapRegion(rx);
            else if (rx < 0 || rx >= w.regions())
                continue;
            const double cx = w.deltaX((rx + 0.5) * tiles, eyeX);
            const double cz = (rz + 0.5) * tiles - eyeZ;
            const double d = std::sqrt(cx * cx + cz * cz) - tiles * 0.71;
            if (d > range) continue;
            wanted.push_back({d, Vec2u{unsigned(rx), unsigned(rz)}});
        }
    std::sort(wanted.begin(), wanted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Vec2u> regions;
    regions.reserve(wanted.size());
    for (const auto& [d, region] : wanted) regions.push_back(region);
    chunks.wantTreeSets(regions);
    for (const auto& t : chunks.landmarkTrees(eyeX, eyeZ, viewDistance))
        add(t, viewDistance);
    trees = std::move(next);
}

bool TreeRenderer::woodPending(const TreeInstance& tree) const {
    if (tree.wood.empty() || !tree.skeleton || !tree.skeleton->formsWood())
        return false;
    const auto wrap = chunks.wrap();
    const auto& b = tree.wood;
    for (int cy = std::max(0, b.y0 / Chunk::SIZE_Y);
         cy <= (b.y1 - 1) / Chunk::SIZE_Y && cy < Y_CHUNKS; ++cy)
        for (int cz = std::max(0, b.z0 / Chunk::SIZE_Z);
             cz <= (b.z1 - 1) / Chunk::SIZE_Z; ++cz)
            for (int cx = int(std::floor(double(b.x0) / Chunk::SIZE_X));
                 cx <= int(std::floor(double(b.x1 - 1) / Chunk::SIZE_X));
                 ++cx) {
                const int64_t x = wrap.enabled() ? wrap.wrapChunk(cx) : cx;
                if (x < 0) continue;
                const ChunkCoord c{unsigned(x), unsigned(cy), unsigned(cz)};
                if (chunkDrawn ? !chunkDrawn(c) : !chunks.hasUploadedMesh(c))
                    return true;
            }
    return false;
}

std::vector<structure_voxels::SourcePtr> TreeRenderer::landmarkVoxels(
    double today) const {
    std::vector<structure_voxels::SourcePtr> out;
    const auto& table = chunks.treeSpecies();
    for (const auto& [key, draw] : trees) {
        const auto& tree = *draw.tree;
        const auto* rule = table.find(tree.species);
        if (!rule || !rule->landmark || !rule->woodBlock || !tree.skeleton)
            continue;
        const float age = tree.growthAge(today);
        if (age <= 0) continue;
        // Growth steps of the crown meshes: the proxies are rebuilt a few
        // dozen times over the tree's maturity, not every day, and never
        // for a day before the latest cut's regrowth or settling.
        const auto& k = *tree.skeleton;
        const float quantized = tree_foliage::bucketAge(
            tree_foliage::bucket(age, k.matureAge(), k.growthPhaseStart()),
            k.matureAge(), k.growthPhaseStart());
        if (quantized <= 0) continue;
        out.push_back(
            tree_wood_lod::voxels({draw.tree, quantized, rule->woodBlock}));
    }
    return out;
}

void TreeRenderer::gatherBillboards(double eyeX, double eyeZ, double range) {
    const auto wrap = chunks.wrap();
    const int tiles = TerrainTuning::RegionX;
    const int reach = int(std::ceil(range / tiles)) + 1;
    const int width = wrap.regions(), height = int(chunks.worldHeightRegions());
    const int64_t ex =
        int64_t(std::floor((wrap.enabled() ? wrap.wrapX(eyeX) : eyeX) / tiles));
    const int64_t ez = int64_t(std::floor(eyeZ / tiles));
    std::vector<std::pair<double, Vec2u>> order;
    for (int64_t dz = -reach; dz <= reach; ++dz)
        for (int64_t dx = -reach; dx <= reach; ++dx) {
            const int64_t rz = ez + dz;
            int64_t rx = ex + dx;
            if (rz < 0 || rz >= height) continue;
            if (wrap.enabled())
                rx = wrap.wrapRegion(rx);
            else if (rx < 0 || rx >= width)
                continue;
            const double cx = wrap.deltaX((rx + 0.5) * tiles, eyeX);
            const double cz = (rz + 0.5) * tiles - eyeZ;
            const double d = std::sqrt(cx * cx + cz * cz);
            // Inside 0.8 DistantEnd only meshes show (once the region's
            // tree set is built); beyond the range none.
            if (d - tiles * 0.71 > range) continue;
            if (d + tiles * 0.71 < DistantEnd * 0.8 &&
                chunks.hasTreeSet({unsigned(rx), unsigned(rz)}))
                continue;
            order.push_back({d, Vec2u{unsigned(rx), unsigned(rz)}});
        }
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    billboardRegions.clear();
    std::vector<Vec2u> wishes;
    for (const auto& [d, region] : order) {
        const auto set = chunks.billboards(region);
        if (!set) wishes.push_back(region);
        if (!set || set->empty()) continue;
        BillboardRegion r{region, set, INFINITY, -INFINITY,
                          !chunks.hasTreeSet(region)};
        for (const auto& b : *set) {
            r.low = std::min(r.low, b.y);
            r.high = std::max(r.high, b.y + b.height * 2);
        }
        billboardRegions.push_back(std::move(r));
    }
    chunks.wantBillboards(std::move(wishes));
    chunks.releaseBillboards(eyeX, eyeZ, range + 2 * tiles);
}

TreeRenderer::~TreeRenderer() {
    for (auto& [key, entry] : meshes)
        if (entry.job) entry.job->wait();
}

const TreeRenderer::BillboardEntry* TreeRenderer::billboardMesh(
    uint8_t species) {
    if (const auto it = billboardMeshes.find(species);
        it != billboardMeshes.end())
        return it->second.cpu.empty() ? nullptr : &it->second;
    auto& entry = billboardMeshes[species];
    const auto& table = chunks.treeSpecies();
    const auto* rule = table.find(species);
    const auto shape = table.variant(species, 0, 0);
    if (!rule || !shape || !assets.hasStorage<FoliageMaterial>())
        return nullptr;
    entry.cpu = tree_foliage::buildBillboard(
        *shape, rule->species, assets.getStorage<FoliageMaterial>());
    if (entry.cpu.empty()) return nullptr;
    entry.gpu.initStatic(&renderer, entry.cpu.vertices.data(),
                         entry.cpu.vertices.size(), entry.cpu.indices.data(),
                         entry.cpu.indices.size());
    return &entry;
}

void TreeRenderer::drawBillboards(RenderTarget& target, const Camera& camera,
                                  const Frustum& frustum,
                                  const std::vector<ShaderProperty*>& base,
                                  double ex, double ey, double ez,
                                  double today) {
    billboardsDrawn = 0;
    const auto wrap = chunks.wrap();
    const double end = std::min<double>(BillboardEnd, camera.viewDistance);
    const double tiles = TerrainTuning::RegionX;
    std::unordered_map<uint8_t, std::vector<float>> groups;
    for (const auto& r : billboardRegions) {
        const double x0 = ex + wrap.deltaX(r.region.x * tiles, ex);
        const double z0 = r.region.y * tiles;
        const double cx = x0 + tiles * 0.5 - ex, cz = z0 + tiles * 0.5 - ez;
        const double d = std::sqrt(cx * cx + cz * cz);
        if (d - tiles * 0.71 > end) continue;
        if (!frustum.intersects(AABB{float(x0 - 32), r.low, float(z0 - 32),
                                     float(x0 + tiles + 32), r.high,
                                     float(z0 + tiles + 32)}))
            continue;
        // Farther regions keep an even share of their trees (the set is
        // sorted by hash), scaled up so the forest keeps its cover.
        const double share = std::clamp(
            std::pow(DistantEnd / std::max(d, double(DistantEnd)), 2), 0.1,
            1.0);
        const size_t count = size_t(std::ceil(share * double(r.set->size())));
        const float boost = float(std::min(1.8, 1 / std::sqrt(share)));
        const double shift = x0 - r.region.x * tiles;
        for (size_t i = 0; i < count; ++i) {
            const auto& b = (*r.set)[i];
            const double age = today - b.birthDay;
            if (age <= 0 || age >= b.lifespan) continue;
            const double x = std::clamp(age / b.maturity, 0.0, 1.0);
            const float g = float(1 - std::pow(1 - x, double(b.curve)));
            if (g < 0.2f) continue;
            auto& out = groups[b.species];
            const float crown = b.crown * g * boost;
            const float h = b.height * g * std::sqrt(boost);
            const float data[16] = {
                float(b.x + shift - ex),
                float(b.y - ey),
                float(b.z - ez),
                float(b.hash & 0xFFFF) / 65536.f * 6.2831853f,
                crown,
                h,
                crown,
                float(b.hash >> 16) / 65536.f,
                BareGround.r,
                BareGround.g,
                BareGround.b,
                0,
                1,
                foliage_light::pack(0, 0, foliage_light::White),
                float(end),
                r.fallback ? 0.f : DistantEnd};
            out.insert(out.end(), std::begin(data), std::end(data));
        }
    }
    for (const auto& [species, data] : groups) {
        const auto* entry = billboardMesh(species);
        if (!entry) continue;
        const uint32_t wanted = uint32_t(data.size() / 16);
        const auto count =
            bgfx::getAvailInstanceDataBuffer(wanted, InstanceStride);
        if (!count) break;
        auto& buffer = instances.emplace_back();
        bgfx::allocInstanceDataBuffer(&buffer, count, InstanceStride);
        std::copy(data.begin(), data.begin() + size_t(count) * 16,
                  reinterpret_cast<float*>(buffer.data));
        target.drawInstanced(entry->gpu.get(), &shader, std::vector(base), true,
                             relativeTransform(camera, ex, ey, ez),
                             DrawLayer::GAME, &buffer);
        billboardsDrawn += count;
        ++calls;
    }
}

const TreeRenderer::MeshEntry* TreeRenderer::mesh(uint64_t key,
                                                  const TreeInstance& tree,
                                                  int bucket,
                                                  tree_foliage::Detail detail,
                                                  bool bare, int& starts) {
    using namespace std::chrono_literals;
    if (auto it = meshes.find(key); it != meshes.end()) {
        auto& entry = it->second;
        entry.lastUsed = frame;
        if (entry.job && entry.job->wait_for(0s) == std::future_status::ready) {
            --meshJobs;
            try {
                entry.cpu = entry.job->get();
            } catch (const std::exception&) {
                entry.cpu.clear();
            }
            entry.job.reset();
            for (const auto& part : entry.cpu) {
                if (part.empty()) continue;
                entry.gpu.emplace_back().initStatic(
                    &renderer, part.vertices.data(), part.vertices.size(),
                    part.indices.data(), part.indices.size());
            }
        }
        return entry.ready() ? &entry : nullptr;
    }
    if (starts <= 0 || meshJobs >= MaxMeshJobs) return nullptr;
    const auto& table = chunks.treeSpecies();
    const auto* rule = table.find(tree.species);
    auto& entry = meshes[key];
    entry.lastUsed = frame;
    if (!rule || !assets.hasStorage<FoliageMaterial>()) return nullptr;
    // Shared shapes are meshed unturned; the instance yaw turns them.
    const auto skeleton = tree.variant >= 0
                              ? table.variant(tree.species, tree.variant, 0)
                              : tree.skeleton;
    if (!skeleton) return nullptr;
    --starts;
    ++meshJobs;
    // The species table and materials outlive the renderer, which waits
    // for its jobs.
    entry.job = pool.submit_task(
        [skeleton, species = &rule->species,
         materials = &assets.getStorage<FoliageMaterial>(), bucket, detail,
         bare] {
            return tree_foliage::buildParts(
                *skeleton, *species, *materials,
                tree_foliage::bucketAge(bucket, skeleton->matureAge(),
                                        skeleton->growthPhaseStart()),
                detail, bare);
        },
        BS::pr::high);
    return nullptr;
}

void TreeRenderer::render(RenderTarget& target, const Camera& camera,
                          const Frustum& frustum, LightRenderer& lights,
                          double now, double today, ShaderProperty& water) {
    const auto started = std::chrono::steady_clock::now();
    ++frame;
    drawn = calls = 0;
    instances.clear();
    if (chunks.treeSpecies().empty() ||
        !bgfx::isValid(assets.getFoliageAlbedoArray())) {
        cpuMs = 0;
        return;
    }
    const double pitch = camera.pitch, yaw = camera.yaw, d = camera.distance;
    const double ex = camera.position.x - std::cos(pitch) * std::sin(yaw) * d;
    const double ey = camera.position.y - std::sin(pitch) * d;
    const double ez = camera.position.z - std::cos(pitch) * std::cos(yaw) * d;
    const WorldWrap wrap = chunks.wrap();
    const int ecx = int(std::floor((wrap.enabled() ? wrap.wrapX(ex) : ex) /
                                   Chunk::SIZE_X)),
              ecz = int(std::floor(ez / Chunk::SIZE_Z));
    if (ecx != gatheredX || ecz != gatheredZ ||
        frame - gatheredFrame >= GatherInterval ||
        chunks.treeRevision() != gatheredRevision ||
        (chunks.treeSetsRevision() != gatheredSets &&
         frame - gatheredFrame >= 10)) {
        gatheredRevision = chunks.treeRevision();
        gatheredSets = chunks.treeSetsRevision();
        gather(ex, ez, camera.viewDistance);
        billboardsGathered = 0;  // with the trees, below
        gatheredX = ecx;
        gatheredZ = ecz;
        gatheredFrame = frame;
    }
    // Billboard sets arrive in the background: pick them up (at most every
    // 10 frames).
    if (billboardsGathered == 0 ||
        (chunks.billboardRevision() != billboardsRevision &&
         frame - billboardsGathered >= 10)) {
        billboardsRevision = chunks.billboardRevision();
        billboardsGathered = frame;
        gatherBillboards(ex, ez,
                         std::min<double>(BillboardEnd, camera.viewDistance));
    }
    // Wood blocks are drawn for chunks around the camera target (the
    // detailed chunks); trees anchored outside them are drawn whole as
    // distant meshes.
    const int tcx = int(std::floor(camera.position.x / Chunk::SIZE_X)),
              tcz = int(std::floor(camera.position.z / Chunk::SIZE_Z));
    const auto detailed = [&](const TreeAnchor& a) {
        const int64_t dx = wrap.enabled()
                               ? wrap.deltaChunk(a.x / Chunk::SIZE_X, tcx)
                               : int64_t(a.x / Chunk::SIZE_X) - tcx;
        const int64_t dz = int64_t(a.z / Chunk::SIZE_Z) - tcz;
        return std::abs(dx) <= DETAIL_CHUNK_RADIUS &&
               std::abs(dz) <= DETAIL_CHUNK_RADIUS;
    };

    struct Instance {
        const TreeDraw* draw;
        double x, y, z;  // anchor point, nearest copy
        float yaw, wither, fade;
    };
    // Instances by the mesh they draw: their own when built, else the one
    // last drawn for the tree (a cut, a growth step or a detail change
    // never blinks a crown out while its next mesh builds).
    std::unordered_map<uint64_t, std::vector<Instance>> groups;
    std::unordered_map<uint64_t, const MeshEntry*> resolved;
    int starts = MeshBuildsPerFrame;
    for (const auto& [key, draw] : trees) {
        const auto& tree = *draw.tree;
        const auto& k = *tree.skeleton;
        const float age = tree.age(today);
        if (age <= 0) continue;
        const double x = ex + wrap.deltaX(tree.anchor.x + 0.5, ex);
        const double shift = x - (tree.anchor.x + 0.5);
        const auto& b = tree.bounds;
        const double nx = std::clamp(ex, b.x0 + shift, b.x1 + shift);
        const double ny = std::clamp(ey, double(b.y0), double(b.y1));
        const double nz = std::clamp(ez, double(b.z0), double(b.z1));
        const double near2 = (nx - ex) * (nx - ex) + (ny - ey) * (ny - ey) +
                             (nz - ez) * (nz - ez);
        const bool near = detailed(tree.anchor);
        const auto* rule = chunks.treeSpecies().find(tree.species);
        // Detailed trees keep their crowns as long as their wood blocks
        // show (the camera target may be far from the eye).
        const float fade =
            rule && rule->landmark ? camera.viewDistance : DistantEnd;
        if (near2 > double(fade) * fade) continue;
        if (!frustum.intersects(AABB{float(b.x0 + shift), float(b.y0),
                                     float(b.z0), float(b.x1 + shift),
                                     float(b.y1), float(b.z1)}))
            continue;
        const double ax = x - ex, az = tree.anchor.z + 0.5 - ez;
        // Landmarks with distant voxel wood never miss wood: their structure
        // slabs hide only where the chunk is drawn, so they need not wait.
        const bool voxelWood = voxelLandmarks && rule && rule->landmark;
        const auto detail =
            !near || (!voxelWood && woodPending(tree))
                ? (voxelWood && !near ? tree_foliage::Detail::DistantCrown
                                      : tree_foliage::Detail::Distant)
            : std::sqrt(ax * ax + az * az) < NearDistance + k.crownRadius * 0.5
                ? tree_foliage::Detail::Near
                : tree_foliage::Detail::Far;
        const bool bare = age >= k.lifespan || k.dead;
        const float wither = std::clamp(
            (age - 0.97f * k.lifespan) / (0.03f * k.lifespan), 0.f, 1.f);
        const int bucket = tree_foliage::bucket(
            tree.growthAge(today), k.matureAge(), k.growthPhaseStart());
        const uint64_t tail =
            uint64_t(bucket) << 8 | uint64_t(detail) << 1 | uint64_t(bare);
        const uint64_t meshKey =
            tree.variant >= 0
                ? (1ULL << 63) | uint64_t(tree.species) << 40 |
                      uint64_t(tree.variant) << 32 | tail
                // Include the cut revision: allocators can reuse a previous
                // skeleton's address while its crown mesh is still cached.
                : ((tree.key * 0x9e3779b97f4a7c15ULL) ^
                   (uint64_t(tree.cuts.size()) * 0x94d049bb133111ebULL) ^
                   (uint64_t(reinterpret_cast<uintptr_t>(k.segments.data())) *
                    0xbf58476d1ce4e5b9ULL) ^
                   tail) &
                      ~(1ULL << 63);
        auto [found, first] = resolved.try_emplace(meshKey, nullptr);
        if (first)
            found->second = mesh(meshKey, tree, bucket, detail, bare, starts);
        uint64_t drawKey = meshKey;
        if (found->second) {
            drawnMesh[key] = meshKey;
        } else {
            const auto last = drawnMesh.find(key);
            const auto old = last == drawnMesh.end()
                                 ? meshes.end()
                                 : meshes.find(last->second);
            if (old == meshes.end() || !old->second.ready()) continue;
            old->second.lastUsed = frame;
            drawKey = last->second;
        }
        groups[drawKey].push_back(
            {&draw, x, double(tree.anchor.y + 1), tree.anchor.z + 0.5,
             tree.variant >= 0 ? float(tree.rotation) * QuarterTurn : 0.f,
             wither, fade});
    }

    std::vector<ShaderProperty*> base{&albedo, &normals, &eyeUniform, &params,
                                      &water};
    lights.appendUniforms(base, LightRenderer::Program::Block);
    eyeUniform.setVec4({float(ex), float(ey), float(ez), camera.viewDistance});
    // z = 1: tree mode (wind by vertex weight, no root darkening).
    params.setVec4({float(std::fmod(now, 3600.0)), 1, 1, 0});
    for (const auto& [key, list] : groups) {
        const auto& entry = meshes.at(key);
        const auto count = bgfx::getAvailInstanceDataBuffer(
            uint32_t(list.size()), InstanceStride);
        if (!count) break;
        auto& buffer = instances.emplace_back();
        bgfx::allocInstanceDataBuffer(&buffer, count, InstanceStride);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& in = list[i];
            auto* v = reinterpret_cast<float*>(buffer.data +
                                               size_t(i) * InstanceStride);
            const float seed = float(in.draw->tree->seed & 0xFFFF) / 65536.f;
            const float data[16] = {
                float(in.x - ex), float(in.y - ey), float(in.z - ez), in.yaw, 1,
                1, 1, seed, in.draw->ground.r, in.draw->ground.g,
                in.draw->ground.b, in.wither, in.draw->sun,
                // Base and crown block light and its colour
                // (foliage.vert, tree mode).
                foliage_light::pack(int(in.draw->blockBase * 15 + .5f),
                                    int(in.draw->blockCrown * 15 + .5f),
                                    in.draw->blockChroma),
                in.fade, 0};
            std::copy(std::begin(data), std::end(data), v);
        }
        // Every part of the crown shares the instances.
        for (const auto& part : entry.gpu) {
            target.drawInstanced(part.get(), &shader, std::vector(base), true,
                                 relativeTransform(camera, ex, ey, ez),
                                 DrawLayer::GAME, &buffer);
            ++calls;
        }
        drawn += count;
    }
    drawBillboards(target, camera, frustum, base, ex, ey, ez, today);
    if (frame % 300 == 0) {
        std::erase_if(meshes, [&](const auto& e) {
            return !e.second.job && frame - e.second.lastUsed > MeshLifetime;
        });
        std::erase_if(drawnMesh, [&](const auto& e) {
            return !trees.contains(e.first) || !meshes.contains(e.second);
        });
    }
    cpuMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
}
