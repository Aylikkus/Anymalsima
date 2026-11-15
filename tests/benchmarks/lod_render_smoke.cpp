#include <bx/math.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

#include "lod_capture_renderer.hpp"
#include "lod_scene.hpp"
#include "resource_manager.hpp"
#include "world/location_manager.hpp"
#include "world/sunlight_propagator.hpp"
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
    const bool handover = scenario == "handover" || walking;
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
        const auto world = lodScene();
        LodCaptureRenderer renderer;
        LocationGenerator generator;
        ThreadPool pool(4);
        ChunkMesher mesher(assets);
        ChunkManager chunks(generator, pool, &mesher, world, &assets);
        SunLightPropagator sunlight(chunks, assets.getStorage<Block>(), pool);
        LocationManager locations(chunks, world);
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
        const bgfx::ViewId order[] = {2, 0, 3};
        bgfx::setViewOrder(0, 3, order);
        for (bgfx::ViewId view : {0, 2}) {
            bgfx::setViewFrameBuffer(view, framebuffer);
            bgfx::setViewRect(view, 0, 0, 1280, 720);
        }
        bgfx::setViewClear(2, BGFX_CLEAR_COLOR, 0xaec4d6ff);
        bgfx::setViewClear(0, BGFX_CLEAR_DEPTH, uint32_t(0), 1.f);
        Camera camera{{6144, 420, 6144}, 1.1f, -.18f, 40};
        if (argc > 3) {
            const std::string scene = argv[3];
            if (scene == "coast") camera = {{5400, 410, 5800}, -1.2f, -.2f, 40};
            if (scene == "elevated")
                camera = {{6144, 700, 6144}, .7f, -.55f, 40};
        }
        SurfaceSampler surface(world, assets);
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
            TerrainLodManager lod(world, assets, pool);
            size_t maxResident = 0, maxUpload = 0;
            double maxFrame = 0;
            int frame = 0, settled = 0, phase = 0;
            size_t preservedChecks = 0, detailRegressions = 0;
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
                    locations.onPlayerRegionChanged(
                        {unsigned(cx) / 8, unsigned(cz) / 8});
                    chunks.setFocus(camera.position);
                    sunlight.setFocus(camera.position);
                    for (int z = cz - 3; z <= cz + 3; ++z)
                        for (int x = cx - 3; x <= cx + 3; ++x)
                            for (int y = cy - 3; y <= cy + 3; ++y)
                                chunks.request(
                                    {unsigned(x), unsigned(y), unsigned(z)});
                    chunks.update();
                    sunlight.update();
                    chunks.uploadReadyMeshes(renderer);
                }
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
                if (handover) {
                    blockEye.setVec4({eye.x, eye.y, eye.z, 4096});
                    chunks.forEachReady([&](const ChunkCoord& coord, Chunk&) {
                        if (!lod.drawsColumn(coord.x, coord.z)) return;
                        auto* mesh = chunks.getMesh(coord);
                        if (!mesh) return;
                        std::array<float, 16> transform;
                        bx::mtxIdentity(transform.data());
                        target.drawVertexBuffer(mesh->get(), &blockShader,
                                                {&blockTexture, &blockEye},
                                                false, std::move(transform),
                                                DrawLayer::GAME);
                        ++detailed;
                    });
                }
                float view[16], projection[16];
                bx::mtxLookAt(
                    view, {eye.x, eye.y, eye.z},
                    {camera.position.x, camera.position.y, camera.position.z},
                    {0, 1, 0});
                bx::mtxProj(projection, 60.f, 1280.f / 720, 1.f, 4608.f,
                            bgfx::getCaps()->homogeneousDepth);
                bgfx::setViewTransform(0, view, projection);
                skyCamera.setVec4({camera.yaw, camera.pitch, 1280.f / 720, 0});
                skyCamera.bind();
                bgfx::setVertexBuffer(0, sky.vbh);
                bgfx::setIndexBuffer(sky.ibh);
                bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
                bgfx::submit(2, assets.getProgram("terrain_sky"));
                size_t draws = 0;
                for (const auto& cmd : target.commands) {
                    const auto& draw = std::get<DrawVertexBufferCmd>(cmd.data);
                    const auto mesh = std::get<DynamicMesh>(draw.mesh);
                    bgfx::setTransform(draw.transform.data());
                    bgfx::setVertexBuffer(0, mesh.vbh, 0, mesh.activeVertices);
                    bgfx::setIndexBuffer(mesh.ibh, 0, mesh.activeIndices);
                    for (auto* uniform : draw.uniforms) uniform->bind();
                    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                   BGFX_STATE_WRITE_Z |
                                   BGFX_STATE_DEPTH_TEST_LESS);
                    bgfx::submit(0, assets.getProgram(*draw.shader));
                    ++draws;
                }
                // Keep frames bounded, while allowing background generators to
                // progress without busy spinning the headless renderer.
                if (lod.backlog() == 0 && lod.jobCount() == 0 &&
                    (!handover ||
                     (sunlight.isIdle() && !chunks.hasGenerationWork() &&
                      detailed > 0)))
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
                        output + (travel ? "_" + std::to_string(phase) : "") +
                            (transition ? "_transition" : "") + ".ppm",
                        std::ios::binary);
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
                        << " detail_regressions=" << detailRegressions << '\n';
                    if ((!transition && lod.backlog() != 0) || draws == 0 ||
                        (handover && detailed == 0) ||
                        maxResident > TerrainLod::GpuBudget ||
                        maxUpload > TerrainLod::UploadBudget)
                        result = 1;
                }
                bgfx::frame();
                renderer.flush();
                maxFrame = std::max(
                    maxFrame, std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - frameStart)
                                  .count());
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
