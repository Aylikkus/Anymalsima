#include <bx/math.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

#include "lod_capture_renderer.hpp"
#include "lod_scene.hpp"
#include "resource_manager.hpp"
#include "world/block_light_propagator.hpp"
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
        LightRenderer lightRenderer(renderer);
        std::vector<TilePos> lamps;
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
        // View 1: additive glow after the world (Renderer::GlowView). Its id
        // must stay below the readback blit's view 3: blits run in view-id
        // order regardless of setViewOrder.
        const bgfx::ViewId order[] = {2, 0, 1, 3};
        bgfx::setViewOrder(0, 4, order);
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
                std::vector<ShaderProperty*> lightUniforms;
                lightRenderer.appendUniforms(lightUniforms);
                lod.setExtraUniforms(std::move(lightUniforms));
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
                if (lights)
                    lightRenderer.update(registry, camera, world.wrap(),
                                         {1280, 720});
                lod.update(camera, {1280, 720});
                RenderTarget target;
                lod.render(target, renderer, chunks, camera, {1280, 720});
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
                            lightRenderer.appendUniforms(uniforms);
                        }
                        target.drawVertexBuffer(
                            mesh->get(), &blockShader, std::move(uniforms),
                            false, std::move(transform), DrawLayer::GAME);
                        ++detailed;
                    });
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
                    lightRenderer.drawGlare(target, camera);
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
                    const bool glow = draw.layer == DrawLayer::GAME_GLOW;
                    bgfx::setState(glow ? BGFX_STATE_WRITE_RGB |
                                              BGFX_STATE_DEPTH_TEST_LESS |
                                              BGFX_STATE_BLEND_ADD
                                        : BGFX_STATE_WRITE_RGB |
                                              BGFX_STATE_WRITE_A |
                                              BGFX_STATE_WRITE_Z |
                                              BGFX_STATE_DEPTH_TEST_LESS);
                    bgfx::submit(glow ? 1 : 0, assets.getProgram(*draw.shader));
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
                if (settled == 10 || frame + 1 == maxFrames || transition) {
                    if (transition) capturedTransition = true;
                    std::vector<uint8_t> pixels(1280 * 720 * 4);
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
                        << " structure_slabs=" << lod.structureCount() << '\n';
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
                    if (phase == 1) {
                        int lit = 0;
                        for (const auto& p : lamps)
                            if (const auto* t =
                                    chunks.tileAt(p.x, p.y + 1, p.z))
                                lit += t->lightLevel.artificial_light;
                        std::cout
                            << "lamps=" << lamps.size()
                            << " light_above_lamps=" << lit
                            << " far_lights=" << lightRenderer.lightCount()
                            << " glares=" << lightRenderer.glareCount() << '\n';
                        if (lamps.empty() || lit == 0 ||
                            lightRenderer.lightCount() == 0)
                            result = 1;
                    }
                    if (phase == 2) {
                        std::cout << "far_glares=" << lightRenderer.glareCount()
                                  << " registry=" << registry.size() << '\n';
                        if (lightRenderer.glareCount() == 0) result = 1;
                        break;
                    }
                    ++phase;
                    settled = 0;
                    const float fx = std::sin(camera.yaw),
                                fz = std::cos(camera.yaw);
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
                        for (int along : {-12, 0, 12})
                            for (int side : {-8, 8}) {
                                const int x = int(camera.position.x +
                                                  fx * along + fz * side);
                                const int z = int(camera.position.z +
                                                  fz * along - fx * side);
                                const TilePos p{x, surface.height(x, z) + 1, z};
                                if (editor.placeBlock(p, id) ==
                                    EditStatus::Applied)
                                    lamps.push_back(p);
                            }
                        lightRenderer.setSkyDarkness(.9f);
                    } else {
                        // High above, looking down at the lamps: their
                        // glare must show over the lit LOD terrain.
                        camera.position.x -= fx * 350;
                        camera.position.z -= fz * 350;
                        camera.position.y =
                            surface.height(int(camera.position.x),
                                           int(camera.position.z)) +
                            160;
                        camera.pitch = -.4f;
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
