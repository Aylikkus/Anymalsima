// DEVICE TEST: needs a GPU with Vulkan and a display session (it fails with
// "Authorization required" / no device in headless sandboxes). Not part of
// CTest; agents without a display must not run it (tests/README.md, "Device
// targets").
#include <bx/math.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

#include "lod_capture_renderer.hpp"
#include "lod_scene.hpp"
#include "render/frustrum.hpp"
#include "render/render_views.hpp"
#include "resource_manager.hpp"
#include "world/block_light_propagator.hpp"
#include "world/flora_renderer.hpp"
#include "world/light_renderer.hpp"
#include "world/light_volume_manager.hpp"
#include "world/location_manager.hpp"
#include "world/sky_height_map.hpp"
#include "world/sunlight_propagator.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_lod_manager.hpp"
#include "world/tree_renderer.hpp"
#include "world/tree_simulation.hpp"

int main(int argc, char** argv) {
    const std::string output = argc > 1 ? argv[1] : "build/lod_capture";
    const int maxFrames = argc > 2 ? std::stoi(argv[2]) : 1200;
    bgfx::Init init{};
    init.type = bgfx::RendererType::Vulkan;
    init.resolution.width = 0;
    init.resolution.height = 0;
    if (!bgfx::init(init)) return 2;
    int result = 0;
    const std::string scenario = argc > 3 ? argv[3] : "mountains";
    const bool walking = scenario == "handover_walk";
    // Edit terrain near the camera, then look back at it from 400 tiles away
    // after its regions unloaded: the pit and structure must remain in LOD.
    const bool structure = scenario == "structure";
    // A patterned wall (many greedy-mesh T-junctions) close to the camera
    // far from the world origin, in front of the sky: any background pixel
    // inside it is a seam gap.
    const bool seams = scenario == "seams";
    // Stand next to the X seam of a wrapping world and look across it, then
    // from the other side back: terrain must continue with no gap.
    const bool wrapScene = scenario == "wrap";
    // Day view, then debug lamps at night (block light, far light and
    // glare), then the same lamps from 350 tiles away and high above (far
    // light on distant terrain and glare).
    const bool lights = scenario == "lights";
    const bool handover = scenario == "handover" || walking || structure ||
                          seams || wrapScene || lights;
    const bool travel = scenario == "travel" || walking;
    {
        ResourceManager assets;
        auto definitions = FileIO::readJson("assets/definitions.json");
        definitions.erase("fonts");
        definitions.erase("textures");
        definitions.erase("sounds");
        definitions.erase("music");
        assets.loadFromJson(definitions);
        assets.buildMaterialTextureArrays();
        // Seams need coordinates as large as a full-size world (~64k tiles).
        // Wrap: 128 regions = 16384 tiles, a whole number of LOD roots.
        auto world = lodScene(wrapScene ? 128 : seams ? 480 : 96);
        // SMOKE_FOREST: trees everywhere (docs/handoff/trees.md). Values:
        // taiga (spruces), tropical (palms on plains), mountain (thin
        // pines on the hills), anything else: humid temperate forest
        // (oaks, redwood groves).
        if (const char* forest = std::getenv("SMOKE_FOREST")) {
            const std::string kind = forest;
            for (auto& r : world.regions) {
                r.vegetation = VegetationType::FOREST;
                r.humidity = .75f;
                r.temperature_c = 14;
                if (kind == "taiga") {
                    r.biome = BiomeType::TUNDRA;
                    r.temperature_c = 2;
                    r.humidity = .6f;
                } else if (kind == "tropical") {
                    r.biome = BiomeType::TROPICAL;
                    r.temperature_c = 27;
                    r.humidity = .6f;
                    r.land = LandType::PLAINS;
                } else if (kind == "mountain") {
                    // The scene's own hills and mountains, without forest.
                    r.vegetation = VegetationType::GRASS;
                    r.temperature_c = 6;
                    r.humidity = .4f;
                }
            }
        }
        LodCaptureRenderer renderer;
        LocationGenerator generator;
        ThreadPool pool(4);
        ChunkMesher mesher(assets);
        // SMOKE_FOREST=elder: temperate forest with many elder landmarks
        // (test-only: the shipped density is about one per 200 km²).
        const bool elders = std::getenv("SMOKE_FOREST") &&
                            std::string(std::getenv("SMOKE_FOREST")) == "elder";
        if (elders && assets.hasStorage<Tree>())
            for (auto& tree :
                 const_cast<DataStorage<Tree>&>(assets.getStorage<Tree>())
                     .items)
                if (tree.landmark()) {
                    tree.placement.density_per_100m2 = 0.01f;
                    // SMOKE_ELDER_SPACING: metres between elders (3000:
                    // about one in view, like the shipped density).
                    tree.spacing_m =
                        std::getenv("SMOKE_ELDER_SPACING")
                            ? std::stof(std::getenv("SMOKE_ELDER_SPACING"))
                            : 600.f;
                }
        ChunkManager chunks(generator, pool, &mesher, world, &assets);
        TerrainEdits edits;
        // Sky heightmap (docs/handoff/sky-light.md): seeds sunlight (unless
        // SMOKE_LEGACY_SUN=1, the sweep through every slab up to the column
        // top) and is checked against the voxel sunlight of fully lit chunks
        // at the end of SMOKE_APPROACH.
        SkyHeightMap skyMap(chunks, edits, assets.getStorage<Block>(), pool);
        struct SkyRequests {
            ChunkManager& chunks;
            Signal<const ChunkCoord&>::Connection connection;
            ~SkyRequests() { chunks.becameReady.disconnect(connection); }
        } skyRequests{chunks,
                      chunks.becameReady.connect([&](const ChunkCoord& c) {
                          skyMap.request(c.x, c.z);
                      })};
        SunLightPropagator sunlight(
            chunks, assets.getStorage<Block>(), pool,
            std::getenv("SMOKE_LEGACY_SUN") ? nullptr : &skyMap);
        LocationManager locations(chunks, world);
        TerrainEditTracker tracker(chunks, edits);
        WorldEditor editor(chunks, &sunlight, assets.getStorage<Block>());
        tracker.watch(editor);
        skyMap.watch(editor);
        struct SkyUnwatch {
            SkyHeightMap& map;
            ~SkyUnwatch() { map.unwatch(); }
        } skyUnwatch{skyMap};
        LightSourceRegistry registry;
        BlockLightPropagator blockLight(chunks, assets.getStorage<Block>(),
                                        pool, &registry);
        blockLight.watch(editor);
        LightVolumeManager volumes(chunks, assets.getStorage<Block>(),
                                   &blockLight, &sunlight);
        // SMOKE_QUALITY=0..2 picks the light (shadow) budget preset.
        const int lightQuality = std::getenv("SMOKE_QUALITY")
                                     ? std::atoi(std::getenv("SMOKE_QUALITY"))
                                     : 0;
        LightRenderer lightRenderer(renderer, lightQuality);
        // Plants, in the lights scenario (the only one binding light
        // volumes, like the game).
        FloraRenderer flora(chunks, assets, world, renderer);
        TreeRenderer trees(chunks, assets, world, renderer, pool);
        flora.setBlockLight(&blockLight);
        trees.setBlockLight(&blockLight);
        double treesMaxMs = 0;
        double floraMaxMs = 0;
        std::vector<TilePos> lamps;
        Camera home{};
        TilePos boxLamp{};
        std::vector<uint8_t> boxBaseline, lastPixels;
        LightSourceRegistry::Id beaconId = 0;
        // As in GameplayState: voxel geometry changes re-render shadows.
        auto chunkChanged = [&](const ChunkCoord& c) {
            const std::array<double, 3> min{c.x * 16.0, c.y * 16.0, c.z * 16.0};
            lightRenderer.shadows().geometryChanged(
                min, {min[0] + 16, min[1] + 16, min[2] + 16},
                world.wrap().tiles);
        };
        chunks.meshUploaded.connect(chunkChanged);
        chunks.removed.connect(chunkChanged);
        // An 8 km omnidirectional entity light in the box, independent of
        // test_light.json.
        auto addBeacon = [&] {
            LightSource beacon;
            beacon.owner = LightSource::Owner::Entity;
            beacon.emission.radius = 8000;
            beacon.position = {boxLamp.x + 0.5, boxLamp.y + 0.5,
                               boxLamp.z + 0.5};
            beaconId = registry.add(beacon);
            // Inside an opaque block, like a lamp: its own cube must not
            // shadow it.
            editor.setBlock(boxLamp, 1);
        };
        // Smoke view order: shadow passes, sky, world, glow (view 1), then
        // the readback blit (view 3); see the capture below.
        auto applyOrder = [](uint16_t shadowViews) {
            bgfx::ViewId order[RenderViews::ShadowBase + 16];
            uint16_t n = 0;
            for (uint16_t i = 0; i < shadowViews; ++i)
                order[n++] = bgfx::ViewId(RenderViews::ShadowBase + i);
            for (bgfx::ViewId id : {2, 0, 1, 3}) order[n++] = id;
            for (bgfx::ViewId id = 4; id < RenderViews::ShadowBase; ++id)
                order[n++] = id;
            bgfx::setViewOrder(0, n, order);
        };
        const auto color = bgfx::createTexture2D(
            1280, 720, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
        const auto depth = bgfx::createTexture2D(
            1280, 720, false, 1, bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT);
        const bgfx::TextureHandle attachments[] = {color, depth};
        const auto framebuffer = bgfx::createFrameBuffer(2, attachments, true);
        static const PosVertex skyVertices[] = {
            {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        static const uint16_t skyIndices[] = {0, 1, 2, 0, 2, 3};
        auto sky = renderer.createStaticMesh(skyVertices, 4, skyIndices, 6);
        ShaderProperty skyCamera;
        skyCamera.initVec4("u_sky_camera", {0, 0, 1280.f / 720, 0});
        const auto readback = bgfx::createTexture2D(
            1280, 720, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        // View 1: additive glow after the world (RenderViews::Glow in the
        // game). Captures blit in a frame of their own (view 3).
        applyOrder(0);
        for (bgfx::ViewId view : {0, 1, 2}) {
            bgfx::setViewFrameBuffer(view, framebuffer);
            bgfx::setViewRect(view, 0, 0, 1280, 720);
        }
        // Seams: no sky; a magenta background makes every gap pixel obvious.
        const bool magenta = seams || wrapScene;
        bgfx::setViewClear(2, BGFX_CLEAR_COLOR,
                           magenta ? 0xff00ffff : 0xaec4d6ff);
        bgfx::setViewClear(0, BGFX_CLEAR_DEPTH, uint32_t(0), 1.f);
        Camera camera{{6144, 420, 6144}, 1.1f, -.18f, 40};
        if (argc > 3) {
            const std::string scene = argv[3];
            if (scene == "coast") camera = {{5400, 410, 5800}, -1.2f, -.2f, 40};
            if (scene == "elevated")
                camera = {{6144, 700, 6144}, .7f, -.55f, 40};
        }
        SurfaceSampler surface(world, assets);
        if (seams) {
            // Dry land as far from the origin as the scene allows.
            int x = surface.width() - 1500;
            while (x > 0 && surface.height(x, 6144) < SEA_LEVEL_Y + 8) x -= 32;
            camera = {{float(x), float(surface.height(x, 6144) + 20), 6144},
                      0,
                      -.1f,
                      10};
        }
        if (wrapScene)
            camera = {
                {float(surface.width() - 24), 0, 6144}, bx::kPiHalf, -.3f, 40};
        camera.position.y = std::max(
            camera.position.y, float(surface.height(int(camera.position.x),
                                                    int(camera.position.z)) +
                                     20));
        // SMOKE_CAMERA="x z yaw pitch distance above": look at (x, z) from
        // `above` tiles over its surface (close-ups of plants and cover).
        if (const char* override = std::getenv("SMOKE_CAMERA")) {
            float x = 0, z = 0, above = 2;
            std::sscanf(override, "%f %f %f %f %f %f", &x, &z, &camera.yaw,
                        &camera.pitch, &camera.distance, &above);
            camera.position = {
                x, float(surface.height(int(x), int(z))) + 1 + above, z};
        }
        // Elders: look at the landmark nearest the default view.
        if (elders && !std::getenv("SMOKE_CAMERA") &&
            !chunks.treeLandmarks().empty()) {
            const auto& sites = chunks.treeLandmarks();
            const auto nearest = std::min_element(
                sites.begin(), sites.end(), [](const auto& a, const auto& b) {
                    return std::hypot(a.x - 6144.0, a.z - 6144.0) <
                           std::hypot(b.x - 6144.0, b.z - 6144.0);
                });
            const float x = float(nearest->x), z = float(nearest->z);
            camera = {{x, float(surface.height(int(x), int(z))) + 12, z},
                      bx::kPiHalf,
                      -.1f,
                      95};
            std::cout << "elder at " << nearest->x << ' ' << nearest->z << '\n';
        }
        // SMOKE_APPROACH="start speed stop" (elders, needs a GPU and
        // display like the whole harness): wait until the view at `start`
        // metres west of the elder has settled, then walk toward the trunk
        // at `speed` metres per frame until `stop` metres from it, timing
        // every frame's sections and counting structure slabs that draw
        // nothing (holes) near the eye.
        struct Approach {
            float start = 0, speed = 0, stop = 0;
            double targetX = 0, targetZ = 0;
            int state = 0;  // 0 settling, 1 walking, 2 arrived
            int frames = 0, gapFrames = 0, gapFrames128 = 0;
            size_t maxGaps = 0, maxGaps128 = 0;
            // Loaded columns near the eye showing black ground.
            size_t maxBlack = 0;
            int blackFrames = 0;
            // Ground chunks near the eye: frame first seen ready, and
            // frames until full light (isComplete, not estimated).
            std::unordered_map<ChunkCoord, int> seenAt, litAfter;
            // Frames until full light was first shown (not estimated; with
            // the old sweep the direct lower bound already counts).
            std::unordered_map<ChunkCoord, int> shownAfter;
            std::vector<std::array<double, 6>> times;  // per section
        } approach;
        const bool approaching =
            elders && std::getenv("SMOKE_APPROACH") &&
            !chunks.treeLandmarks().empty() &&
            std::sscanf(std::getenv("SMOKE_APPROACH"), "%f %f %f",
                        &approach.start, &approach.speed, &approach.stop) == 3;
        if (approaching) {
            approach.targetX = camera.position.x;
            approach.targetZ = camera.position.z;
            const float x = float(approach.targetX - approach.start);
            const float z = float(approach.targetZ);
            camera = {{x, float(surface.height(int(x), int(z))) + 3, z},
                      bx::kPiHalf,
                      -.05f,
                      12};
        }
        // SMOKE_LIMB_CUT=1 (elders): stand on the ground under the thickest
        // sideways limb of the elder in view, saw it through near the trunk
        // once the view settled, and log the sunlight under it until it is
        // full sky (the air it shaded must light up).
        struct LimbCut {
            // 0 find, 4 walk under the limb first, 1 settle on the limb,
            // 2 cut, 3 walk under it again
            int state = 0;
            std::vector<TilePos> samples;
            TreeVec cut{};
            TreeAnchor anchor{};
            int dim = 0, reach = 0, frame = 0, quiet = 0;
            size_t visit = 2;  // states 3, 4: the sample walked to
            Vec3 saved{};
        } limbCut;
        const bool cutting = elders && std::getenv("SMOKE_LIMB_CUT");
        std::unique_ptr<TreeSimulation> treeSim;
        if (cutting) treeSim = std::make_unique<TreeSimulation>(chunks, editor);
        if (handover)
            locations.loadRegion({unsigned(camera.position.x) / 128,
                                  unsigned(camera.position.z) / 128},
                                 1);
        ShaderProperty blockTexture, blockEye;
        blockTexture.initTexture("u_texture_array", 0,
                                 assets.getMaterialAlbedoArray());
        blockEye.initVec4("u_world_eye", {0, 0, 0, 4096});
        const std::string blockShader = "block";
        const auto begin = std::chrono::steady_clock::now();
        {
            // Forests track edits like the game: landmark wood is drawn as
            // structure voxels at distance.
            // SMOKE_TERRAIN=0..2: distant terrain preset (Low by default).
            const int terrainQuality =
                std::clamp(std::getenv("SMOKE_TERRAIN")
                               ? std::atoi(std::getenv("SMOKE_TERRAIN"))
                               : 0,
                           0, TerrainLod::QualityCount - 1);
            TerrainLodManager lod(
                world, assets, pool, TerrainLod::Qualities[terrainQuality],
                structure || std::getenv("SMOKE_FOREST") ? &edits : nullptr);
            trees.setVoxelLandmarks(lod.drawsStructureVoxels());
            // As the handover pass below draws chunks.
            trees.setChunkDrawn([&](const ChunkCoord& c) {
                return lod.drawsChunk(c) && chunks.hasUploadedMesh(c);
            });
            if (lights) {
                std::vector<ShaderProperty*> patchLights, blockLights;
                lightRenderer.appendUniforms(patchLights,
                                             LightRenderer::Program::Lod);
                lightRenderer.appendUniforms(blockLights,
                                             LightRenderer::Program::Block);
                lod.setExtraUniforms(std::move(patchLights),
                                     std::move(blockLights));
            }
            size_t maxResident = 0, maxUpload = 0;
            double maxFrame = 0;
            int frame = 0, settled = 0, phase = 0;
            size_t preservedChecks = 0, detailRegressions = 0, seamPixels = 0;
            bool capturedTransition = false;
            std::set<std::pair<int, int>> previousVisible;
            if (scenario == "cancel") {
                lod.update(camera, {1280, 720});
                std::cout << "cancel_pending_jobs=" << lod.jobCount() << '\n';
            }
            for (; frame < maxFrames && scenario != "cancel"; ++frame) {
                const auto frameStart = std::chrono::steady_clock::now();
                using SmokeClock = std::chrono::steady_clock;
                const auto since = [](SmokeClock::time_point t) {
                    return std::chrono::duration<double, std::milli>(
                               SmokeClock::now() - t)
                        .count();
                };
                // chunks, lod update, lod render+upload, trees, frame, flush
                std::array<double, 6> sections{};
                auto mark = SmokeClock::now();
                if (approaching && approach.state == 1) {
                    const double dx = approach.targetX - camera.position.x;
                    const double dz = approach.targetZ - camera.position.z;
                    const double d = std::hypot(dx, dz);
                    if (d <= approach.stop) {
                        approach.state = 2;
                    } else {
                        const double step =
                            std::min<double>(approach.speed, d - approach.stop);
                        camera.position.x += float(dx / d * step);
                        camera.position.z += float(dz / d * step);
                        camera.position.y =
                            float(surface.height(int(camera.position.x),
                                                 int(camera.position.z)) +
                                  3);
                    }
                }
                if (handover) {
                    const int cx = int(camera.position.x) / 16,
                              cy = int(camera.position.y) / 16,
                              cz = int(camera.position.z) / 16;
                    tracker.update();
                    locations.onPlayerRegionChanged(
                        {unsigned(cx) / 8, unsigned(cz) / 8});
                    locations.update();  // gradual unloading
                    chunks.setFocus(camera.position);
                    sunlight.setFocus(camera.position);
                    blockLight.setFocus(camera.position);
                    // SMOKE_RADIUS: chunks requested around the target
                    // (default 3; the game uses DETAIL_CHUNK_RADIUS).
                    static const int reach =
                        std::getenv("SMOKE_RADIUS")
                            ? std::atoi(std::getenv("SMOKE_RADIUS"))
                            : 3;
                    for (int z = cz - reach; z <= cz + reach; ++z)
                        for (int x = cx - reach; x <= cx + reach; ++x)
                            for (int y = cy - reach; y <= cy + reach; ++y)
                                chunks.request(
                                    {unsigned(world.wrap().wrapChunk(x)),
                                     unsigned(y), unsigned(z)});
                    chunks.update();
                    skyMap.setFocus(camera.position);
                    skyMap.update();
                    sunlight.update();
                    blockLight.update();
                    chunks.uploadReadyMeshes(renderer);
                    if (lights) volumes.update(camera.position);
                }
                sections[0] = since(mark);
                mark = SmokeClock::now();
                const double now = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - begin)
                                       .count();
                if (lights)
                    lightRenderer.update(registry, camera, world.wrap(), now);
                lod.setStructures(trees.landmarkVoxels(0));
                lod.update(camera, {1280, 720});
                // Shade under structures on distant surfaces (step 2).
                if (lights && !std::getenv("SMOKE_NO_OCCLUSION"))
                    lightRenderer.setSkyOcclusion(lod.skyOcclusion());
                sections[1] = since(mark);
                mark = SmokeClock::now();
                RenderTarget target;
                lod.render(target, renderer, chunks, camera, {1280, 720});
                sections[2] = since(mark);
                auto& water = lod.waterUniform();
                if (lights) {
                    static uint64_t lodVersion = 0;
                    if (lod.drawnVersion() != lodVersion) {
                        lodVersion = lod.drawnVersion();
                        lightRenderer.shadows().lodChanged();
                    }
                    const int ccx = int(camera.position.x) / 16;
                    lightRenderer.renderShadows(
                        target,
                        [&](RenderTarget& t, const LightShadows::Face& face) {
                            const auto& l = face.light;
                            chunks.forEachReady([&](const ChunkCoord& coord,
                                                    Chunk&) {
                                if (!lod.drawsChunk(coord) ||
                                    std::abs(world.wrap().deltaChunk(coord.x,
                                                                     ccx)) > 3)
                                    return;
                                auto* mesh = chunks.getMesh(coord);
                                if (!mesh) return;
                                const double wx =
                                    l[0] +
                                    world.wrap().deltaX(coord.x * 16.0 + 8,
                                                        l[0]) -
                                    8;
                                const double lx = wx - l[0],
                                             ly = coord.y * 16.0 - l[1],
                                             lz = coord.z * 16.0 - l[2];
                                if (!LightShadowMath::reaches(
                                        lx, ly, lz, lx + 16, ly + 16, lz + 16,
                                        face.radius))
                                    return;
                                t.drawShadowCaster(
                                    mesh->get(), face.shader,
                                    std::vector(face.chunkUniforms), false,
                                    LightShadowMath::translation(lx, ly, lz),
                                    face.view);
                            });
                            lod.submitShadowCasters(t, face, chunks, camera);
                        },
                        now);
                    // renderShadows applied the game's order; the smoke
                    // tool's glow view differs.
                    applyOrder(RenderViews::MaxShadowViews);
                }
                if (walking) {
                    const int cx = int(camera.position.x) / 16,
                              cy = int(camera.position.y) / 16,
                              cz = int(camera.position.z) / 16;
                    for (const auto& [x, z] : previousVisible) {
                        const int y =
                            std::max(surface.height(x * 16 + 8, z * 16 + 8),
                                     SEA_LEVEL_Y) /
                            16;
                        if (std::abs(x - cx) >
                                TerrainLod::DetailCoverage::KeepRadius ||
                            std::abs(z - cz) >
                                TerrainLod::DetailCoverage::KeepRadius ||
                            std::abs(y - cy) > 6 ||
                            !chunks.hasUploadedMesh(
                                {unsigned(x), unsigned(y), unsigned(z)}))
                            continue;
                        ++preservedChecks;
                        if (!lod.drawsColumn(x, z)) {
                            if (detailRegressions == 0)
                                std::cerr
                                    << "Lost drawable column at boundary: " << x
                                    << "," << z << " frame=" << frame << '\n';
                            ++detailRegressions;
                            result = 1;
                        }
                    }
                    previousVisible.clear();
                    chunks.forEachReady([&](const ChunkCoord& coord, Chunk&) {
                        if (lod.drawsColumn(coord.x, coord.z))
                            previousVisible.emplace(int(coord.x), int(coord.z));
                    });
                }
                maxResident = std::max(maxResident, lod.residentBytes());
                maxUpload = std::max(maxUpload, lod.uploadedBytes());
                const auto eye = camera.eye();
                size_t detailed = 0;
                // Lights scenario, last view: high above distant terrain.
                const bool farLights = lights && phase == 2;
                if (handover) {
                    blockEye.setVec4({eye.x, eye.y, eye.z, 4096});
                    chunks.forEachReady([&](const ChunkCoord& coord, Chunk&) {
                        if (!lod.drawsChunk(coord)) return;
                        auto* mesh = chunks.getMesh(coord);
                        if (!mesh) return;
                        // The copy nearest the camera (X wraps).
                        const int camCX = int(camera.position.x) / 16;
                        const double wx =
                            double(camCX +
                                   world.wrap().deltaChunk(coord.x, camCX)) *
                            Chunk::SIZE_X;
                        auto transform = relativeTransform(
                            camera, wx, double(coord.y) * Chunk::SIZE_Y,
                            double(coord.z) * Chunk::SIZE_Z);
                        std::vector<ShaderProperty*> uniforms{
                            &blockTexture, &blockEye, &water};
                        if (lights) {
                            if (!volumes.bind(coord, uniforms, wx)) return;
                            lightRenderer.appendUniforms(
                                uniforms, LightRenderer::Program::Block);
                        }
                        target.drawVertexBuffer(
                            mesh->get(), &blockShader, std::move(uniforms),
                            false, std::move(transform), DrawLayer::GAME);
                        ++detailed;
                    });
                }
                if (lights) {
                    float worldView[16], worldProj[16], viewProj[16];
                    bx::mtxLookAt(worldView, {eye.x, eye.y, eye.z},
                                  {camera.position.x, camera.position.y,
                                   camera.position.z},
                                  {0, 1, 0});
                    bx::mtxProj(worldProj, 60.f, 1280.f / 720, 1.f, 4608.f,
                                bgfx::getCaps()->homogeneousDepth);
                    bx::mtxMul(viewProj, worldView, worldProj);
                    flora.render(target, camera,
                                 Frustum::fromViewProj(viewProj), lightRenderer,
                                 frame / 60.0, 0, water);
                    floraMaxMs = std::max(floraMaxMs, flora.cpuMilliseconds());
                    mark = SmokeClock::now();
                    trees.render(target, camera,
                                 Frustum::fromViewProj(viewProj), lightRenderer,
                                 frame / 60.0, 0, water);
                    sections[3] = since(mark);
                    treesMaxMs = std::max(treesMaxMs, trees.cpuMilliseconds());
                }
                float view[16], projection[16];
                // Camera-relative world view, as in Renderer: the eye is the
                // origin and draws carry relativeTransform().
                bx::mtxLookAt(
                    view, {0, 0, 0},
                    {camera.position.x - eye.x, camera.position.y - eye.y,
                     camera.position.z - eye.z},
                    {0, 1, 0});
                bx::mtxProj(projection, 60.f, 1280.f / 720, 1.f, 4608.f,
                            bgfx::getCaps()->homogeneousDepth);
                bgfx::setViewTransform(0, view, projection);
                bgfx::setViewTransform(1, view, projection);
                // NO_GLARE=1 renders without glare, for image diffs.
                if (lights && !std::getenv("NO_GLARE"))
                    lightRenderer.drawGlare(target, camera, {1280, 720});
                skyCamera.setVec4({camera.yaw, camera.pitch, 1280.f / 720,
                                   lod.eyeUnderwater() ? 1.f : 0.f});
                skyCamera.bind();
                bgfx::setVertexBuffer(0, sky.vbh);
                bgfx::setIndexBuffer(sky.ibh);
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
                if (!magenta)
                    bgfx::submit(2, assets.getProgram("terrain_sky"));
                else
                    bgfx::touch(2);
                size_t draws = 0;
                for (const auto& cmd : target.commands) {
                    const auto& draw = std::get<DrawVertexBufferCmd>(cmd.data);
                    bgfx::setTransform(draw.transform.data());
                    if (draw.isStaticMesh) {
                        const auto mesh = std::get<StaticMesh>(draw.mesh);
                        bgfx::setVertexBuffer(0, mesh.vbh);
                        bgfx::setIndexBuffer(mesh.ibh);
                    } else {
                        const auto mesh = std::get<DynamicMesh>(draw.mesh);
                        bgfx::setVertexBuffer(0, mesh.vbh, 0,
                                              mesh.activeVertices);
                        bgfx::setIndexBuffer(mesh.ibh, 0, mesh.activeIndices);
                    }
                    for (auto* uniform : draw.uniforms) uniform->bind();
                    if (draw.instances)
                        bgfx::setInstanceDataBuffer(draw.instances);
                    const bool glow = draw.layer == DrawLayer::GAME_GLOW;
                    const bool shadow = draw.layer == DrawLayer::SHADOW;
                    bgfx::setState(
                        glow     ? BGFX_STATE_WRITE_RGB |
                                       BGFX_STATE_DEPTH_TEST_LESS |
                                       BGFX_STATE_BLEND_ADD
                        : shadow ? BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_Z |
                                       BGFX_STATE_DEPTH_TEST_LESS
                                 : BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                       BGFX_STATE_WRITE_Z |
                                       BGFX_STATE_DEPTH_TEST_LESS);
                    bgfx::submit(glow     ? 1
                                 : shadow ? draw.view
                                          : 0,
                                 assets.getProgram(*draw.shader));
                    ++draws;
                }
                // Keep frames bounded, while allowing background generators to
                // progress without busy spinning the headless renderer.
                if (lod.backlog() == 0 && lod.jobCount() == 0 &&
                    (!handover || (sunlight.isIdle() && blockLight.isIdle() &&
                                   !chunks.hasGenerationWork() &&
                                   (detailed > 0 || farLights))))
                    ++settled;
                else
                    settled = 0;
                const bool transition = walking && phase == 1 &&
                                        lod.patchCount() < 32 && detailed > 0 &&
                                        !capturedTransition;
                // SMOKE_FLORA_GALLERY=1: before the first capture, plant every
                // species in rows by growth stage (seedling .. withering)
                // around the camera target, replacing generated plants.
                if (lights && phase == 0 && settled == 5 &&
                    std::getenv("SMOKE_FLORA_GALLERY")) {
                    const auto& species = assets.getStorage<Flora>().items;
                    std::unordered_map<ChunkCoord, FloraLayer> gallery;
                    for (size_t s = 0; s < species.size(); ++s)
                        for (int st = 0; st < FloraStageCount; ++st) {
                            // The first age (in days) with this stage.
                            int age = -1;
                            for (int a = 0; a < 3000 && age < 0; ++a) {
                                const auto state =
                                    flora_growth::at(species[s], 4242, a);
                                if (state.alive && int(state.stage) == st &&
                                    state.progress > 0.4f)
                                    age = a;
                            }
                            if (age < 0) continue;
                            const double x =
                                camera.position.x +
                                (double(s) - species.size() / 2.0) * 2.5;
                            const double z =
                                camera.position.z + (double(st) - 2.5) * 2.5;
                            const int y = chunks.generatedSurfaceY(unsigned(x),
                                                                   unsigned(z));
                            const ChunkCoord c{unsigned(x) / 16,
                                               unsigned(y) / 16,
                                               unsigned(z) / 16};
                            FloraInstance p;
                            p.species = species[s].id;
                            p.seed = 4242;
                            p.x = uint8_t((x - c.x * 16) * 16);
                            p.z = uint8_t((z - c.z * 16) * 16);
                            p.y = uint8_t(y - int(c.y) * 16);
                            p.birthDay = -age;
                            gallery[c].instances.push_back(p);
                        }
                    for (auto& [c, layer] : gallery) {
                        // Keep the resolved day: a reset one re-samples.
                        if (const auto live = chunks.flora(c))
                            layer.resolvedDay = live->resolvedDay;
                        chunks.setFlora(c, std::make_shared<FloraLayer>(layer));
                    }
                }
                if (settled == 10 || frame + 1 == maxFrames || transition) {
                    if (transition) capturedTransition = true;
                    std::vector<uint8_t> pixels(1280 * 720 * 4);
                    // Finish this frame first: the blit then runs alone and
                    // sees every view of it, whatever the view order.
                    bgfx::frame();
                    bgfx::blit(3, readback, 0, 0, color);
                    const auto readyFrame =
                        bgfx::readTexture(readback, pixels.data());
                    auto rendered = bgfx::frame();
                    while (rendered < readyFrame) rendered = bgfx::frame();
                    std::ofstream image(
                        output +
                            (travel || structure || seams || wrapScene || lights
                                 ? "_" + std::to_string(phase)
                                 : "") +
                            (transition ? "_transition" : "") + ".ppm",
                        std::ios::binary);
                    if (seams && phase >= 1) {
                        size_t gaps = 0;
                        for (int y = 144; y < 576; ++y)
                            for (int x = 256; x < 1024; ++x) {
                                const auto* p = &pixels[(y * 1280 + x) * 4];
                                if (p[0] > 200 && p[1] < 60 && p[2] > 200)
                                    ++gaps;
                            }
                        seamPixels += gaps;
                        std::cout << "pose=" << phase << " seam_pixels=" << gaps
                                  << '\n';
                    }
                    if (wrapScene) {
                        // Below the horizon only terrain may show.
                        size_t gaps = 0;
                        for (int y = 480; y < 720; ++y)
                            for (int x = 0; x < 1280; ++x) {
                                const auto* p = &pixels[(y * 1280 + x) * 4];
                                if (p[0] > 200 && p[1] < 60 && p[2] > 200)
                                    ++gaps;
                            }
                        std::cout << "wrap_view=" << phase
                                  << " gap_pixels=" << gaps << '\n';
                        if (gaps) result = 1;
                    }
                    lastPixels = pixels;
                    if (lights && (phase == 5 || phase == 1) &&
                        std::getenv("SHADOW_DUMP")) {
                        // Slot 0's six faces, log-scaled distance.
                        const int w = LightShadowMath::Columns *
                                      LightShadows::TileSize,
                                  h = lightRenderer.shadows().atlasRows() *
                                      LightShadows::TileSize;
                        const auto back = bgfx::createTexture2D(
                            uint16_t(w), uint16_t(h), false, 1,
                            bgfx::TextureFormat::R32F,
                            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
                        std::vector<float> texels(size_t(w) * h);
                        bgfx::blit(
                            3, back, 0, 0,
                            lightRenderer.shadows().atlasTextureHandle());
                        const auto ready =
                            bgfx::readTexture(back, texels.data());
                        auto f = bgfx::frame();
                        while (f < ready) f = bgfx::frame();
                        std::ofstream dump(
                            output + "_shadow" + std::to_string(phase) + ".pgm",
                            std::ios::binary);
                        dump << "P5\n" << 6 * 256 << " 256\n255\n";
                        for (int y = 0; y < 256; ++y)
                            for (int face = 0; face < 6; ++face)
                                for (int x = 0; x < 256; ++x) {
                                    const float v =
                                        texels[size_t(y * 2) * w +
                                               face * LightShadows::TileSize +
                                               x * 2];
                                    const float d = std::max(v * 8000.f, 0.01f);
                                    dump.put(char(std::clamp(
                                        int(std::log2(d) * 18 + 40), 0, 255)));
                                }
                        {
                            // The whole atlas as raw floats (distance /
                            // radius), for offline analysis.
                            std::ofstream raw(output + "_atlas" +
                                                  std::to_string(phase) +
                                                  ".f32",
                                              std::ios::binary);
                            raw.write(
                                reinterpret_cast<const char*>(texels.data()),
                                std::streamsize(texels.size() * sizeof(float)));
                        }
                        {
                            // Face 0 at full resolution: far texels white.
                            std::ofstream full(output + "_face0_" +
                                                   std::to_string(phase) +
                                                   ".pgm",
                                               std::ios::binary);
                            full << "P5\n512 512\n255\n";
                            for (int y = 0; y < 512; ++y)
                                for (int x = 0; x < 512; ++x)
                                    full.put(char(
                                        texels[size_t(y) * w + x] > 0.05f ? 255
                                                                          : 0));
                        }
                        float lo = 1e9, hi = -1e9;
                        for (int face = 0; face < 6; ++face)
                            for (int y = 0; y < 512; ++y)
                                for (int x = 0; x < 512; ++x) {
                                    const float v =
                                        texels[size_t(y) * w +
                                               face * LightShadows::TileSize +
                                               x];
                                    lo = std::min(lo, v);
                                    hi = std::max(hi, v);
                                }
                        std::cout << "shadow_tile_range=" << lo * 8000 << ".."
                                  << hi * 8000 << '\n';
                        bgfx::destroy(back);
                    }
                    image << "P6\n1280 720\n255\n";
                    for (int y = 0; y < 720; ++y)
                        for (int x = 0; x < 1280; ++x)
                            image.write(reinterpret_cast<const char*>(
                                            pixels.data() + (y * 1280 + x) * 4),
                                        3);
                    if (!image) result = 1;
                    std::cout
                        << "phase=" << phase << " frame=" << frame
                        << " detail_draws=" << detailed
                        << " patches=" << lod.patchCount()
                        << " backlog=" << lod.backlog() << " draws=" << draws
                        << " resident_MiB=" << lod.residentBytes() / 1048576.
                        << " max_resident_MiB=" << maxResident / 1048576.
                        << " max_upload_KiB=" << maxUpload / 1024.
                        << " elapsed_s="
                        << std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - begin)
                               .count()
                        << " max_cpu_frame_ms=" << maxFrame
                        << " preserved_detail_checks=" << preservedChecks
                        << " detail_regressions=" << detailRegressions
                        << " edited_columns=" << edits.size()
                        << " structure_slabs=" << lod.structureCount()
                        << " voxel_slabs=" << lod.voxelSlabs()
                        << " voxel_slabs_l0=" << lod.voxelSlabs(0)
                        << " voxel_slabs_l1=" << lod.voxelSlabs(1)
                        << " voxel_slabs_l2=" << lod.voxelSlabs(2)
                        << " voxel_groups=" << lod.structureGroups()
                        << " occluded_texels="
                        << (lod.skyOcclusion() ? lod.skyOcclusion()->occluded()
                                               : 0)
                        << " plants=" << flora.drawnPlants()
                        << " plant_draws=" << flora.drawCalls()
                        << " plant_meshes=" << flora.meshCount()
                        << " plant_cpu_ms=" << flora.cpuMilliseconds()
                        << " plant_max_cpu_ms=" << floraMaxMs
                        << " trees=" << trees.drawnTrees()
                        << " billboards=" << trees.drawnBillboards()
                        << " tree_draws=" << trees.drawCalls()
                        << " tree_meshes=" << trees.meshCount()
                        << " tree_cpu_ms=" << trees.cpuMilliseconds()
                        << " failed_meshes=" << IRenderer::failedDynamicMeshes
                        << " tree_max_cpu_ms=" << treesMaxMs << '\n';
                    if ((!transition && lod.backlog() != 0) || draws == 0 ||
                        (handover && detailed == 0 && !farLights) ||
                        // Structure proxies have their own 1/8 cap.
                        maxResident > TerrainLod::GpuBudget * 9 / 8 ||
                        maxUpload > TerrainLod::UploadBudget)
                        result = 1;
                }
                mark = SmokeClock::now();
                bgfx::frame();
                renderer.flush();
                sections[5] = since(mark);
                sections[4] = since(frameStart);
                if (cutting) {
                    treeSim->update(0);
                    const auto sunAt = [&](const TilePos& p) {
                        const Tile* t = chunks.tileAt(p.x, p.y, p.z);
                        return t ? int(t->lightLevel.sun_exposure) : -1;
                    };
                    if (limbCut.state == 0) {
                        for (const auto& tree : chunks.landmarkTrees(
                                 camera.position.x, camera.position.z, 4000)) {
                            const auto& k = *tree->skeleton;
                            int32_t limb = -1;
                            for (size_t i = 0; i < k.segments.size(); ++i) {
                                const auto& sg = k.segments[i];
                                const TreeVec dir = (sg.b - sg.a).normalized();
                                if (sg.live() && sg.order == 1 &&
                                    sg.parent >= 0 &&
                                    k.segments[size_t(sg.parent)].order == 0 &&
                                    std::abs(dir.y) < 0.8f &&
                                    (limb < 0 ||
                                     sg.r0 > k.segments[size_t(limb)].r0))
                                    limb = int32_t(i);
                            }
                            if (limb < 0) continue;
                            const float clear =
                                k.segments[size_t(
                                               k.segments[size_t(limb)].parent)]
                                    .r0 +
                                k.segments[size_t(limb)].r0;
                            int32_t sawn = -1;
                            const auto a = tree->anchor;
                            for (int32_t at = limb; at >= 0;) {
                                const auto& sg = k.segments[size_t(at)];
                                if (sawn >= 0) {
                                    const TreeVec m = lerp(sg.a, sg.b, 0.5f);
                                    const int x = a.x + int(std::lround(m.x)),
                                              z = a.z + int(std::lround(m.z));
                                    limbCut.samples.push_back(
                                        {x, surface.height(x, z) + 3, z});
                                } else if (std::hypot(sg.a.x, sg.a.z) > clear) {
                                    sawn = at;
                                }
                                int32_t next = -1;
                                for (size_t i = 0; i < k.segments.size(); ++i)
                                    if (k.segments[i].parent == at &&
                                        k.segments[i].attachT >= 1 &&
                                        k.segments[i].live())
                                        next = int32_t(i);
                                at = next;
                            }
                            if (sawn < 0 || limbCut.samples.size() < 6) {
                                limbCut.samples.clear();
                                continue;
                            }
                            const auto& sg = k.segments[size_t(sawn)];
                            limbCut.cut = lerp(sg.a, sg.b, 0.5f);
                            limbCut.anchor = a;
                            const TreeVec dir = (sg.b - sg.a).normalized();
                            const float d[3]{dir.x, dir.y, dir.z};
                            for (int i = 1; i < 3; ++i)
                                if (std::abs(d[i]) > std::abs(d[limbCut.dim]))
                                    limbCut.dim = i;
                            limbCut.reach = int(std::ceil(sg.r0)) + 4;
                            // Every 4th sample, so the log stays short.
                            std::vector<TilePos> kept;
                            for (size_t i = 0; i < limbCut.samples.size();
                                 i += 4)
                                kept.push_back(limbCut.samples[i]);
                            limbCut.samples = std::move(kept);
                            // First on the limb at the cut (sawing needs its
                            // cross-section loaded), then down to the ground.
                            camera.position = {
                                float(a.x + limbCut.cut.x),
                                float(a.y + limbCut.cut.y + sg.r0 + 2),
                                float(a.z + limbCut.cut.z)};
                            std::cout
                                << "limb_cut found r=" << sg.r0
                                << " limb_y=" << a.y + limbCut.cut.y
                                << " ground_y=" << limbCut.samples[1].y - 3
                                << '\n';
                            limbCut.state = 1;
                            // SMOKE_LIMB_CUT=walk: load the ground under the
                            // whole limb before going up to cut it.
                            if (std::string(std::getenv("SMOKE_LIMB_CUT")) ==
                                "walk") {
                                limbCut.state = 4;
                                limbCut.saved = camera.position;
                            }
                            break;
                        }
                    } else if (limbCut.state == 1 &&
                               (limbCut.quiet = !chunks.hasGenerationWork() &&
                                                        sunlight.isIdle()
                                                    ? limbCut.quiet + 1
                                                    : 0) >= 60) {
                        const auto& a = limbCut.anchor;
                        int c[3]{a.x + int(std::lround(limbCut.cut.x)),
                                 a.y + int(std::lround(limbCut.cut.y + 0.5f)),
                                 a.z + int(std::lround(limbCut.cut.z))};
                        std::vector<TilePos> slice;
                        size_t unloaded = 0;
                        for (int i = -limbCut.reach; i <= limbCut.reach; ++i)
                            for (int j = -limbCut.reach; j <= limbCut.reach;
                                 ++j) {
                                int q[3]{c[0], c[1], c[2]};
                                q[(limbCut.dim + 1) % 3] += i;
                                q[(limbCut.dim + 2) % 3] += j;
                                const Tile* t = chunks.tileAt(q[0], q[1], q[2]);
                                if (!t) ++unloaded;
                                if (t && chunks.grownBlocks().test(t->block_id))
                                    slice.push_back({q[0], q[1], q[2]});
                            }
                        std::cout << "limb_cut before (from the limb) frame="
                                  << frame;
                        for (const auto& p : limbCut.samples)
                            std::cout << ' ' << sunAt(p);
                        std::cout << '\n';
                        const size_t cuts = treeSim->cutCount();
                        for (const auto& p : slice) editor.removeBlock(p);
                        std::cout << "limb_cut sawn tiles=" << slice.size()
                                  << " unloaded=" << unloaded
                                  << " cuts=" << treeSim->cutCount() - cuts
                                  << '\n';
                        limbCut.state = 2;
                        limbCut.frame = frame;
                        // Walk mode stays at the cut: the ground under the
                        // far limb was loaded before and is still resident.
                        if (std::string(std::getenv("SMOKE_LIMB_CUT")) !=
                            "walk") {
                            const auto& stand = limbCut.samples[1];
                            camera.position = {float(stand.x), float(stand.y),
                                               float(stand.z)};
                        }
                    } else if (limbCut.state == 2) {
                        bool lit = true;
                        for (const auto& p : limbCut.samples)
                            lit = lit && sunAt(p) != 0 && sunAt(p) < 15 ? false
                                                                        : lit;
                        lit = lit && treeSim->pendingClears() == 0 &&
                              sunlight.isIdle() && !chunks.hasGenerationWork();
                        const int since = frame - limbCut.frame;
                        if (since % 60 == 0 || lit || since >= 3000) {
                            std::cout
                                << "limb_cut after frames=" << since
                                << " clears=" << treeSim->pendingClears()
                                << " gen_queue=" << chunks.pendingGeneration()
                                << " sun_idle=" << sunlight.isIdle() << " sun";
                            for (const auto& p : limbCut.samples)
                                std::cout << ' ' << sunAt(p);
                            std::cout << '\n';
                        }
                        if (lit || since >= 3000) {
                            std::cout << "limb_cut settled frames=" << since
                                      << '\n';
                            limbCut.state = 3;
                            limbCut.quiet = 0;
                        }
                    } else if (limbCut.state == 4) {
                        const auto& p = limbCut.samples[limbCut.visit];
                        camera.position = {float(p.x), float(p.y), float(p.z)};
                        limbCut.quiet =
                            !chunks.hasGenerationWork() && sunlight.isIdle()
                                ? limbCut.quiet + 1
                                : 0;
                        if (limbCut.quiet >= 60) {
                            std::cout << "limb_cut prewalk=" << limbCut.visit
                                      << " sun=" << sunAt(p) << '\n';
                            limbCut.quiet = 0;
                            if (++limbCut.visit >= limbCut.samples.size()) {
                                limbCut.visit = 2;
                                limbCut.state = 1;
                                camera.position = limbCut.saved;
                            }
                        }
                    } else if (limbCut.state == 3) {
                        // Walk to the far samples (loaded only now).
                        const auto& p = limbCut.samples[limbCut.visit];
                        camera.position = {float(p.x), float(p.y), float(p.z)};
                        limbCut.quiet = !chunks.hasGenerationWork() &&
                                                sunlight.isIdle() &&
                                                treeSim->pendingClears() == 0
                                            ? limbCut.quiet + 1
                                            : 0;
                        if (limbCut.quiet >= 60) {
                            // The sample and a ring around it.
                            std::cout << "limb_cut visit=" << limbCut.visit
                                      << " sun=" << sunAt(p) << " ring";
                            for (const auto [dx, dz] :
                                 {std::pair{8, 0}, std::pair{-8, 0},
                                  std::pair{0, 8}, std::pair{0, -8}})
                                std::cout << ' '
                                          << sunAt({p.x + dx,
                                                    surface.height(p.x + dx,
                                                                   p.z + dz) +
                                                        3,
                                                    p.z + dz});
                            std::cout << '\n';
                            limbCut.quiet = 0;
                            if (++limbCut.visit >= limbCut.samples.size()) {
                                std::cout << "limb_cut done\n";
                                break;
                            }
                        }
                    }
                    settled = 0;
                }
                if (approaching) {
                    if (approach.state == 0 &&
                        ((settled >= 5 && phase == 0) || frame > 6000)) {
                        approach.state = 1;
                        std::cout
                            << "approach_start frame=" << frame << " elapsed_s="
                            << std::chrono::duration<double>(SmokeClock::now() -
                                                             begin)
                                   .count()
                            << '\n';
                    }
                    if (approach.state >= 1) {
                        const size_t gaps =
                            lod.structureGaps(chunks, camera, 512);
                        const size_t near =
                            lod.structureGaps(chunks, camera, 128);
                        approach.maxGaps = std::max(approach.maxGaps, gaps);
                        approach.maxGaps128 =
                            std::max(approach.maxGaps128, near);
                        approach.gapFrames += gaps > 0;
                        approach.gapFrames128 += near > 0;
                        approach.times.push_back(sections);
                        const double d =
                            std::hypot(approach.targetX - camera.position.x,
                                       approach.targetZ - camera.position.z);
                        // Loaded columns within 128 tiles whose open ground
                        // reads no sky at four probes: chunks blacked out
                        // while sky slabs above them generate (a tall
                        // structure raises the column top).
                        size_t black = 0;
                        const int cx0 = int(camera.position.x) / 16;
                        const int cz0 = int(camera.position.z) / 16;
                        for (int dz = -8; dz <= 8; ++dz)
                            for (int dx = -8; dx <= 8; ++dx) {
                                int dark = 0;
                                for (const int p : {2, 13})
                                    for (const int q : {2, 13}) {
                                        const int x = (cx0 + dx) * 16 + p;
                                        const int z = (cz0 + dz) * 16 + q;
                                        if (x < 0 || z < 0) continue;
                                        const int y =
                                            chunks.generatedSurfaceY(
                                                unsigned(x), unsigned(z)) +
                                            1;
                                        const Tile* t = chunks.tileAt(x, y, z);
                                        dark += t && t->isAir() &&
                                                t->lightLevel.sun_exposure == 0;
                                    }
                                black += dark == 4;
                            }
                        for (int dz = -8; dz <= 8; ++dz)
                            for (int dx = -8; dx <= 8; ++dx) {
                                const int x = (cx0 + dx) * 16 + 8;
                                const int z = (cz0 + dz) * 16 + 8;
                                if (x < 0 || z < 0) continue;
                                const int y = chunks.generatedSurfaceY(
                                                  unsigned(x), unsigned(z)) +
                                              1;
                                const ChunkCoord c{
                                    unsigned(world.wrap().wrapChunk(x / 16)),
                                    unsigned(y / 16), unsigned(z / 16)};
                                if (!chunks.isReady(c)) continue;
                                const int first =
                                    approach.seenAt.try_emplace(c, frame)
                                        .first->second;
                                if (!approach.shownAfter.contains(c) &&
                                    !sunlight.isEstimated(c))
                                    approach.shownAfter[c] = frame - first;
                                if (!approach.litAfter.contains(c) &&
                                    sunlight.isComplete(c) &&
                                    !sunlight.isEstimated(c))
                                    approach.litAfter[c] = frame - first;
                            }
                        // What the unlit ground chunks near the eye wait
                        // on (SunLightPropagator::Wait), every 30 frames.
                        if (approach.frames % 30 == 0) {
                            std::array<int, 8> waits{};
                            for (const auto& [c, first] : approach.seenAt)
                                if (!approach.litAfter.contains(c) &&
                                    std::abs(int(c.x) - cx0) <= 8 &&
                                    std::abs(int(c.z) - cz0) <= 8)
                                    ++waits[size_t(sunlight.waitingOn(c))];
                            std::cout << "approach_waits";
                            for (const int w : waits) std::cout << ' ' << w;
                            std::cout << '\n';
                        }
                        approach.maxBlack = std::max(approach.maxBlack, black);
                        approach.blackFrames += black > 0;
                        if (approach.frames++ % 30 == 0 || sections[4] > 33)
                            std::cout
                                << "approach_black columns=" << black << '\n'
                                << "approach d=" << int(d)
                                << " frame_ms=" << sections[4]
                                << " chunks_ms=" << sections[0]
                                << " lod_ms=" << sections[1]
                                << " lod_render_ms=" << sections[2]
                                << " trees_ms=" << sections[3]
                                << " flush_ms=" << sections[5]
                                << " gaps512=" << gaps << " gaps128=" << near
                                << " backlog=" << lod.backlog()
                                << " gen_queue=" << chunks.pendingGeneration()
                                << " gen_jobs=" << chunks.generationInFlight()
                                << " mesh_jobs=" << chunks.meshInFlight()
                                << " l0=" << lod.voxelSlabs(0)
                                << " l1=" << lod.voxelSlabs(1)
                                << " l2=" << lod.voxelSlabs(2) << '\n';
                        // Arrived: keep measuring until everything settled.
                        if (approach.state == 2 && lod.backlog() == 0 &&
                            !chunks.hasGenerationWork() && gaps == 0) {
                            const char* names[] = {"chunks",     "lod",
                                                   "lod_render", "trees",
                                                   "frame",      "flush"};
                            for (size_t i = 0; i < 6; ++i) {
                                std::vector<double> v;
                                for (const auto& t : approach.times)
                                    v.push_back(t[i]);
                                std::sort(v.begin(), v.end());
                                double sum = 0;
                                for (double x : v) sum += x;
                                std::cout
                                    << "approach_" << names[i]
                                    << " mean_ms=" << sum / v.size()
                                    << " p95_ms=" << v[size_t(v.size() * .95)]
                                    << " p99_ms=" << v[size_t(v.size() * .99)]
                                    << " max_ms=" << v.back() << '\n';
                            }
                            size_t over16 = 0, over33 = 0;
                            for (const auto& t : approach.times) {
                                over16 += t[4] > 16.7;
                                over33 += t[4] > 33.3;
                            }
                            std::cout
                                << "approach_done frames=" << approach.frames
                                << " frames_over_16ms=" << over16
                                << " frames_over_33ms=" << over33
                                << " gap_frames512=" << approach.gapFrames
                                << " gap_frames128=" << approach.gapFrames128
                                << " max_gaps512=" << approach.maxGaps
                                << " max_gaps128=" << approach.maxGaps128
                                << " black_frames=" << approach.blackFrames
                                << " max_black=" << approach.maxBlack << '\n';
                            // Ready to full light, under tall structures
                            // (wood 48+ tiles above the chunk) and elsewhere.
                            for (const bool tall : {false, true})
                                for (const bool shown : {false, true}) {
                                    const auto& after =
                                        shown ? approach.shownAfter
                                              : approach.litAfter;
                                    std::vector<int> v;
                                    size_t n = 0;
                                    for (const auto& [c, first] :
                                         approach.seenAt) {
                                        const auto trees =
                                            chunks.treeColumn(c.x, c.z);
                                        if ((trees && trees->woodTop >
                                                          int(c.y) * 16 + 48) !=
                                            tall)
                                            continue;
                                        ++n;
                                        if (const auto it = after.find(c);
                                            it != after.end())
                                            v.push_back(it->second);
                                    }
                                    std::sort(v.begin(), v.end());
                                    std::cout << (shown ? "approach_shown"
                                                        : "approach_lit")
                                              << " tall=" << tall
                                              << " chunks=" << n
                                              << " unlit=" << n - v.size();
                                    if (!v.empty())
                                        std::cout
                                            << " p50_frames=" << v[v.size() / 2]
                                            << " p90_frames="
                                            << v[v.size() * 9 / 10]
                                            << " max_frames=" << v.back();
                                    std::cout << '\n';
                                }
                            size_t ready = 0;
                            chunks.forEachReady(
                                [&](const ChunkCoord&, Chunk&) { ++ready; });
                            std::cout << "approach_ready_chunks=" << ready
                                      << '\n';
                            // Fully lit tiles: sun 15 exactly where the map
                            // gives direct 15, never below its direct light.
                            size_t cells = 0, wrong = 0, unmapped = 0;
                            chunks.forEachReady([&](const ChunkCoord& c,
                                                    Chunk& chunk) {
                                if (!sunlight.isComplete(c)) return;
                                const auto t = skyMap.tile(c.x, c.z);
                                if (!t) {
                                    ++unmapped;
                                    return;
                                }
                                for (int z = 0; z < 16; ++z)
                                    for (int x = 0; x < 16; ++x)
                                        for (int y = 0; y < 16; ++y) {
                                            const int sun =
                                                chunk.tiles[z][y][x]
                                                    .lightLevel.sun_exposure;
                                            const int d = t->direct(
                                                sky::Tile::index(x, z),
                                                int(c.y) * 16 + y);
                                            ++cells;
                                            wrong += (sun == 15) != (d == 15) ||
                                                     sun < d;
                                        }
                            });
                            std::cout << "sky_check cells=" << cells
                                      << " wrong=" << wrong
                                      << " unmapped_chunks=" << unmapped
                                      << " columns=" << skyMap.builtCount()
                                      << '\n';
                            break;
                        }
                        settled = 0;  // no phase changes meanwhile
                    }
                }
                maxFrame = std::max(
                    maxFrame, std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - frameStart)
                                  .count());
                if (settled >= 10 && seams && phase >= 1) {
                    // Sweep oblique views with sub-tile offsets: T-junction
                    // gaps depend on the projected position of each seam.
                    if (phase >= 12) break;
                    ++phase;
                    settled = 0;
                    camera.yaw = -.5f + (phase - 1) / 11.f;
                    camera.position.x += .37f;
                    camera.position.y += .23f;
                    camera.pitch = .1f + .02f * (phase % 3);
                }
                if (settled >= 10 && seams && phase == 0) {
                    ++phase;
                    settled = 0;
                    const int px = int(camera.position.x),
                              pz = int(camera.position.z);
                    int top = 0;
                    for (int z = pz + 6; z <= pz + 12; ++z)
                        for (int x = px - 20; x <= px + 20; ++x)
                            top = std::max(top, surface.height(x, z));
                    const int base = top + 1;
                    std::vector<EditRequest> wall;
                    for (int z = pz + 8; z <= pz + 9; ++z)
                        for (int y = base; y < base + 24; ++y)
                            for (int x = px - 20; x <= px + 20; ++x) {
                                // Rectangles of different sizes: greedy quads
                                // meet in T-junctions everywhere.
                                const bool second =
                                    ((x / 3) + (y / 2) + (x * y) % 5) % 3 == 0;
                                wall.push_back({{x, y, z},
                                                EditMode::Replace,
                                                uint8_t(second ? 2 : 1)});
                            }
                    editor.apply(wall);
                    camera.position = {px + .5f, float(base + 12), pz + 2.f};
                    camera.yaw = -.5f;
                    camera.pitch = .1f;
                    camera.distance = .5f;
                }
                if (settled >= 10 && lights) {
                    // Pixels differing from the baseline capture.
                    auto changedPixels = [&] {
                        size_t changed = 0;
                        for (size_t i = 0; i < lastPixels.size(); i += 4)
                            for (int c = 0; c < 3; ++c)
                                if (std::abs(int(lastPixels[i + c]) -
                                             int(boxBaseline[i + c])) > 6) {
                                    ++changed;
                                    break;
                                }
                        return changed;
                    };
                    if (phase == 1) {
                        // The brightest face neighbor: test_light.json may
                        // be a spotlight pointing any way.
                        int lit = 0;
                        for (const auto& p : lamps) {
                            int best = 0;
                            for (const auto& d :
                                 {std::array{1, 0, 0}, std::array{-1, 0, 0},
                                  std::array{0, 1, 0}, std::array{0, -1, 0},
                                  std::array{0, 0, 1}, std::array{0, 0, -1}})
                                if (const auto* t = chunks.tileAt(
                                        p.x + d[0], p.y + d[1], p.z + d[2]))
                                    best = std::max(
                                        best,
                                        int(t->lightLevel.artificial_light));
                            lit += best;
                        }
                        std::cout
                            << "lamps=" << lamps.size()
                            << " lamp_neighbor_light=" << lit
                            << " far_lights=" << lightRenderer.lightCount()
                            << " glares=" << lightRenderer.glareCount() << '\n';
                        if (lamps.empty() || lit == 0 ||
                            lightRenderer.lightCount() == 0)
                            result = 1;
                    }
                    if (phase == 1)
                        std::cout
                            << "glow_lights=" << lightRenderer.glowLights()
                            << '\n';
                    if (phase == 1 && lightRenderer.glowLights() < 40)
                        result = 1;
                    if (phase == 2) {
                        std::cout << "far_glares=" << lightRenderer.glareCount()
                                  << " registry=" << registry.size() << '\n';
                        if (lightRenderer.glareCount() == 0) result = 1;
                    }
                    if (phase == 4 || phase == 6) boxBaseline = lastPixels;
                    if (phase == 5) {
                        // An 8 km light walled in by a hollow box: shadows
                        // keep everything outside exactly as dark as without
                        // it, and hide its glare.
                        const size_t changed = changedPixels();
                        std::cout
                            << "boxed_light_changed_pixels=" << changed
                            << " shadowed=" << lightRenderer.shadowedCount()
                            << " shadow_faces="
                            << lightRenderer.shadows().facesRendered() << '\n';
                        if (changed > 200 || lightRenderer.shadowedCount() == 0)
                            result = 1;
                    }
                    if (phase == 7) {
                        // Control: without its walls the same light must
                        // visibly light the scene.
                        const size_t changed = changedPixels();
                        std::cout << "open_light_changed_pixels=" << changed
                                  << '\n';
                        // Far beyond the 15-block voxel light: a light
                        // shadowed by its own block changed ~59k pixels, the
                        // working light ~277k.
                        if (changed < 150000) result = 1;
                        break;
                    }
                    ++phase;
                    settled = 0;
                    const float fx = std::sin(camera.yaw),
                                fz = std::cos(camera.yaw);
                    auto expectApplied = [&](const auto& statuses) {
                        for (const auto status : statuses)
                            if (status != EditStatus::Applied &&
                                status != EditStatus::Unchanged) {
                                std::cerr << "lights: edit not applied\n";
                                result = 1;
                                return;
                            }
                    };
                    if (phase == 1) {
                        const auto& blocks = assets.getStorage<Block>();
                        const auto it =
                            blocks.indexByName.find("blocks.test_light");
                        if (it == blocks.indexByName.end()) {
                            std::cerr << "blocks.test_light missing\n";
                            result = 1;
                            break;
                        }
                        const uint8_t id = blocks.items[it->second].id;
                        // SINGLE_LAMP=1: one lamp, so it is always shadowed.
                        const bool single = std::getenv("SINGLE_LAMP");
                        for (int along : {-12, 0, 12})
                            for (int side : {-8, 8}) {
                                if (single && (along != 0 || side != 8))
                                    continue;
                                const int x = int(camera.position.x +
                                                  fx * along + fz * side);
                                const int z = int(camera.position.z +
                                                  fz * along - fx * side);
                                const char* lift = std::getenv("LAMP_HEIGHT");
                                const TilePos p{
                                    x,
                                    surface.height(x, z) + 1 +
                                        (lift ? std::atoi(lift) : 0),
                                    z};
                                if (editor.placeBlock(p, id) ==
                                    EditStatus::Applied) {
                                    lamps.push_back(p);
                                    // LAMP_BOX=1: wall the lamp in on five
                                    // faces, open toward -z.
                                    if (std::getenv("LAMP_BOX"))
                                        for (const auto& d :
                                             {std::array{1, 0, 0},
                                              std::array{-1, 0, 0},
                                              std::array{0, 1, 0},
                                              std::array{0, -1, 0},
                                              std::array{0, 0, 1},
                                              // LAMP_BOX=2: closed.
                                              std::string(std::getenv(
                                                  "LAMP_BOX")) == "2"
                                                  ? std::array{0, 0, -1}
                                                  : std::array{0, 0, 1}})
                                            editor.setBlock(
                                                {p.x + d[0], p.y + d[1],
                                                 p.z + d[2]},
                                                1);
                                }
                            }
                        lightRenderer.setSkyDarkness(.9f);
                        home = camera;
                        // A "town": small warm entity lights beyond the voxel
                        // range, shown by the distant glow map and glare.
                        for (int i = 0; i < 40; ++i) {
                            const int tx = int(camera.position.x + fx * 150 +
                                               fz * ((i % 8) * 6 - 21));
                            const int tz = int(camera.position.z + fz * 150 -
                                               fx * ((i % 8) * 6 - 21)) +
                                           (i / 8) * 6;
                            LightSource torch;
                            torch.owner = LightSource::Owner::Entity;
                            torch.emission.radius = 12;
                            torch.emission.color = {1.f, .7f, .4f};
                            torch.position = {
                                tx + .5, surface.height(tx, tz) + 2.5, tz + .5};
                            registry.add(torch);
                        }
                    } else if (phase == 2) {
                        // High above, looking down at the lamps: their
                        // glare must show over the lit LOD terrain.
                        camera.position.x -= fx * 350;
                        camera.position.z -= fz * 350;
                        camera.position.y =
                            surface.height(int(camera.position.x),
                                           int(camera.position.z)) +
                            160;
                        camera.pitch = -.4f;
                    } else if (phase == 3) {
                        // Back home; its chunks reload before any edit.
                        camera = home;
                    } else if (phase == 4) {
                        // Remove the lamps, build a hollow limestone box
                        // (block 1) and capture it without a light.
                        std::vector<EditRequest> edits;
                        for (const auto& p : lamps)
                            edits.push_back({p, EditMode::Replace, 0});
                        const int x = int(home.position.x),
                                  z = int(home.position.z);
                        const int y = surface.height(x, z) + 1;
                        for (int dy = 0; dy < 5; ++dy)
                            for (int dz = -2; dz <= 2; ++dz)
                                for (int dx = -2; dx <= 2; ++dx) {
                                    const bool shell = std::abs(dx) == 2 ||
                                                       std::abs(dz) == 2 ||
                                                       dy == 0 || dy == 4;
                                    edits.push_back({{x + dx, y + dy, z + dz},
                                                     EditMode::Replace,
                                                     uint8_t(shell ? 1 : 0)});
                                }
                        expectApplied(editor.apply(edits));
                        boxLamp = {x, y + 2, z};
                    } else if (phase == 5) {
                        addBeacon();
                    } else if (phase == 6) {
                        // Remove the light, the walls and the roof (keep the
                        // floor) and capture the open scene without it.
                        registry.remove(beaconId);
                        std::vector<EditRequest> open;
                        for (int dy = 1; dy < 5; ++dy)
                            for (int dz = -2; dz <= 2; ++dz)
                                for (int dx = -2; dx <= 2; ++dx)
                                    open.push_back(
                                        {{boxLamp.x + dx, boxLamp.y - 2 + dy,
                                          boxLamp.z + dz},
                                         EditMode::Replace,
                                         0});
                        expectApplied(editor.apply(open));
                    } else if (phase == 7) {
                        addBeacon();
                    }
                }
                if (settled >= 10 && wrapScene) {
                    if (phase >= 1) break;
                    ++phase;
                    settled = 0;
                    // Past the seam (canonical x near 0), looking back.
                    camera.position.x = 24;
                    camera.yaw = -bx::kPiHalf;
                    camera.position.y = surface.height(int(camera.position.x),
                                                       int(camera.position.z)) +
                                        20;
                }
                if (settled >= 10 && structure) {
                    if (phase >= 2) break;
                    ++phase;
                    settled = 0;
                    if (phase == 1) {
                        // A 16x16 pit 8 deep, and a 3x3 pillar 40 tall with
                        // an 11x11 platform overhanging it.
                        const int px = int(camera.position.x) + 40,
                                  pz = int(camera.position.z) + 20;
                        const int base = surface.height(px, pz);
                        std::vector<EditRequest> requests;
                        for (int z = pz - 30; z < pz - 14; ++z)
                            for (int x = px - 30; x < px - 14; ++x)
                                for (int y = surface.height(x, z);
                                     y > surface.height(x, z) - 8; --y)
                                    requests.push_back(
                                        {{x, y, z}, EditMode::Remove});
                        for (int y = 0; y <= 40; ++y)
                            for (int z = pz - 1; z <= pz + 1; ++z)
                                for (int x = px - 1; x <= px + 1; ++x)
                                    if (base + y > surface.height(x, z))
                                        requests.push_back({{x, base + y, z},
                                                            EditMode::Replace,
                                                            1});
                        for (int z = pz - 5; z <= pz + 5; ++z)
                            for (int x = px - 5; x <= px + 5; ++x)
                                requests.push_back(
                                    {{x, base + 40, z}, EditMode::Replace, 1});
                        const auto statuses = editor.apply(requests);
                        std::cout
                            << "edits_applied="
                            << std::count(statuses.begin(), statuses.end(),
                                          EditStatus::Applied)
                            << "/" << statuses.size() << '\n';
                    } else {
                        // Look back at the edits from beyond the keep radius.
                        camera.position.x += 400;
                        camera.yaw = -bx::kPiHalf;
                        camera.position.y =
                            surface.height(int(camera.position.x),
                                           int(camera.position.z)) +
                            30;
                    }
                }
                if (settled >= 10) {
                    if (!travel || phase >= 2) break;
                    ++phase;
                    settled = 0;
                    if (walking) {
                        camera.position.x += phase == 1 ? 128 : -128;
                        // Force the other regression: uploaded meshes remain
                        // visible while their lighting/mesh revisions change.
                        chunks.markAllDirty();
                    } else {
                        camera.position.x += phase == 1 ? 512 : 4096;
                        camera.position.z += phase == 1 ? 128 : 2048;
                        camera.position.y =
                            surface.height(int(camera.position.x),
                                           int(camera.position.z)) +
                            30;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (travel && (phase < 2 || settled < 10)) result = 1;
            if (structure &&
                (phase < 2 || edits.size() == 0 || lod.structureCount() == 0))
                result = 1;
            if (seams) {
                std::cout << "seam_pixels_total=" << seamPixels << '\n';
                if (phase < 12 || seamPixels) result = 1;
            }
            if (walking && (preservedChecks == 0 || detailRegressions != 0))
                result = 1;
        }
        renderer.flush();
        bgfx::destroy(sky.vbh);
        bgfx::destroy(sky.ibh);
        bgfx::destroy(framebuffer);
        bgfx::destroy(readback);
        bgfx::frame();
        bgfx::frame();
    }
    bgfx::frame();
    bgfx::frame();
    bgfx::shutdown();
    return result;
}
