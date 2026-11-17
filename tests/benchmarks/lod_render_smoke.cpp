#include <bx/math.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>

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
#include "world/sunlight_propagator.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_lod_manager.hpp"

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
        const auto world = lodScene(wrapScene ? 128 : seams ? 480 : 96);
        LodCaptureRenderer renderer;
        LocationGenerator generator;
        ThreadPool pool(4);
        ChunkMesher mesher(assets);
        ChunkManager chunks(generator, pool, &mesher, world, &assets);
        SunLightPropagator sunlight(chunks, assets.getStorage<Block>(), pool);
        LocationManager locations(chunks, world);
        TerrainEdits edits;
        TerrainEditTracker tracker(chunks, edits);
        WorldEditor editor(chunks, &sunlight, assets.getStorage<Block>());
        tracker.watch(editor);
        LightSourceRegistry registry;
        BlockLightPropagator blockLight(chunks, assets.getStorage<Block>(),
                                        pool, &registry);
        blockLight.watch(editor);
        LightVolumeManager volumes(chunks, assets.getStorage<Block>(),
                                   &blockLight);
        // SMOKE_QUALITY=0..2 picks the light (shadow) budget preset.
        const int lightQuality = std::getenv("SMOKE_QUALITY")
                                     ? std::atoi(std::getenv("SMOKE_QUALITY"))
                                     : 0;
        LightRenderer lightRenderer(renderer, lightQuality);
        // Plants, in the lights scenario (the only one binding light
        // volumes, like the game).
        FloraRenderer flora(chunks, assets, world, renderer);
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
            TerrainLodManager lod(world, assets, pool, TerrainLod::Qualities[0],
                                  structure ? &edits : nullptr);
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
                if (handover) {
                    const int cx = int(camera.position.x) / 16,
                              cy = int(camera.position.y) / 16,
                              cz = int(camera.position.z) / 16;
                    tracker.update();
                    locations.onPlayerRegionChanged(
                        {unsigned(cx) / 8, unsigned(cz) / 8});
                    chunks.setFocus(camera.position);
                    sunlight.setFocus(camera.position);
                    blockLight.setFocus(camera.position);
                    for (int z = cz - 3; z <= cz + 3; ++z)
                        for (int x = cx - 3; x <= cx + 3; ++x)
                            for (int y = cy - 3; y <= cy + 3; ++y)
                                chunks.request(
                                    {unsigned(world.wrap().wrapChunk(x)),
                                     unsigned(y), unsigned(z)});
                    chunks.update();
                    sunlight.update();
                    blockLight.update();
                    chunks.uploadReadyMeshes(renderer);
                    if (lights) volumes.update(camera.position);
                }
                const double now = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - begin)
                                       .count();
                if (lights)
                    lightRenderer.update(registry, camera, world.wrap(), now);
                lod.update(camera, {1280, 720});
                RenderTarget target;
                lod.render(target, renderer, chunks, camera, {1280, 720});
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
                                if (!lod.drawsColumn(coord.x, coord.z) ||
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
                        if (!lod.drawsColumn(coord.x, coord.z)) return;
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
                        std::vector<ShaderProperty*> uniforms{&blockTexture,
                                                              &blockEye};
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
                                 frame / 60.0);
                    floraMaxMs = std::max(floraMaxMs, flora.cpuMilliseconds());
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
                skyCamera.setVec4({camera.yaw, camera.pitch, 1280.f / 720, 0});
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
                        << " plants=" << flora.drawnPlants()
                        << " plant_draws=" << flora.drawCalls()
                        << " plant_meshes=" << flora.meshCount()
                        << " plant_cpu_ms=" << flora.cpuMilliseconds()
                        << " plant_max_cpu_ms=" << floraMaxMs << '\n';
                    if ((!transition && lod.backlog() != 0) || draws == 0 ||
                        (handover && detailed == 0 && !farLights) ||
                        // Structure proxies have their own 1/8 cap.
                        maxResident > TerrainLod::GpuBudget * 9 / 8 ||
                        maxUpload > TerrainLod::UploadBudget)
                        result = 1;
                }
                bgfx::frame();
                renderer.flush();
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
