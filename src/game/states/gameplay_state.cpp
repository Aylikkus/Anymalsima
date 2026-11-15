#include "gameplay_state.hpp"

#include <bx/math.h>

#include <iostream>

#include "render/frustrum.hpp"

// How many chunks out from the camera column to request on X and Z axes.
static constexpr int CHUNK_RADIUS = 8;

GameplayState::GameplayState(Game& game, UIContext& context,
                             std::unique_ptr<World>&& world,
                             const Region& initial) {
    // name, stage, texture_handle
    u_texture_array.initTexture("u_texture_array", 0,
                                context.assets->getMaterialAlbedoArray());
    this->world = std::move(world);
    ui_manager = std::make_unique<UIManager>(context);

    auto root = std::make_unique<Panel>();
    auto* root_ptr = root.get();
    root->setBackgroundColor({0, 0, 0, 0});
    root->setAnchor({0, 0, 1, 1});
    root->setGrid({24, 16});
    root->setGridHighlight(false);

    ui_manager->setRoot(std::move(root));

    auto tile_debug_panel =
        ui_manager->createWidget(std::make_unique<Panel>(), root_ptr);
    tile_debug_panel->setGrid({32, 32});
    tile_debug_panel->setAnchor({0, 0, 1, 1});
    tile_debug_panel->setBackgroundColor({30, 30, 30, 180});

    tileDebugText =
        ui_manager->createWidget(std::make_unique<Text>(), tile_debug_panel);
    tileDebugText->setGridPlacement({0, 0, 6, 8});
    tileDebugText->setText("No tile selected");

    generator = std::make_unique<LocationGenerator>();
    mesher = std::make_unique<ChunkMesher>(*context.assets);
    chunkManager = std::make_unique<ChunkManager>(
        *generator, context.threadPool, mesher.get(), *this->world.get(),
        context.assets);
    sunLight = std::make_unique<SunLightPropagator>(
        *chunkManager.get(), context.assets->getStorage<Block>(),
        context.threadPool);
    locationMgr =
        std::make_unique<LocationManager>(*chunkManager, *this->world);

    locationMgr->loadRegion(initial.coord, 2);

    const float regionTiles =
        CHUNKS_PER_REGION * (Chunk::SIZE_X + Chunk::SIZE_Z) / 2;

    float centerX = (float(initial.coord.x) * regionTiles + regionTiles * 0.5f);
    float centerZ = (float(initial.coord.y) * regionTiles + regionTiles * 0.5f);

    float centerY =
        chunkManager->generatedSurfaceY(static_cast<unsigned>(centerX),
                                        static_cast<unsigned>(centerZ)) +
        10.0f;

    camera.position = {centerX, centerY, centerZ};
    camera.pitch = -1.5f;
    camera.yaw = 0;
    camera.distance = 10.0f;

    requestChunksAroundCamera();
}

void GameplayState::handleInput(Game& game, const InputEvent& e,
                                UIContext& context, float dt) {
    if (!ui_manager->handleInput(e, context)) {
        switch (e.type) {
            case InputEvent::MouseClick:
                if (e.button == InputEvent::RIGHT_BUTTON) {
                    rotating = e.pressed;
                    lastMouseX = e.x;
                    lastMouseY = e.y;
                }
                break;

            case InputEvent::MouseMove:
                if (rotating) {
                    int dx = e.x - lastMouseX;
                    int dy = e.y - lastMouseY;

                    lastMouseX = e.x;
                    lastMouseY = e.y;

                    const float sensitivity = 0.005f;

                    camera.yaw += dx * sensitivity;
                    camera.pitch -= dy * sensitivity;

                    // clamp pitch to avoid flipping
                    const float limit = bx::kPiHalf - 0.01f;
                    camera.pitch = bx::clamp(camera.pitch, -limit, limit);
                }
                break;

            case InputEvent::MouseWheel:
                if (e.direction == InputEvent::Vertical && e.ctrl) {
                    camera.position.y += e.delta;
                } else if (e.direction == InputEvent::Vertical) {
                    camera.distance -= e.delta;
                    camera.distance = bx::clamp(camera.distance, 10, 150);
                }
                break;
            // gameplay_state.cpp — handleInput, KeyPress section
            case InputEvent::KeyHold: {
                float speed = 20.0f * dt;

                if (e.shift) {
                    speed *= 3;
                }

                // Flat forward: yaw only, no pitch so we stay on the
                // horizontal plane
                float flatFwdX = std::sin(camera.yaw);
                float flatFwdZ = std::cos(camera.yaw);

                // Right is perpendicular to flat forward (rotate 90° around
                // Y)
                float rightX = flatFwdZ;
                float rightZ = -flatFwdX;

                if (e.key == SDL_SCANCODE_W) {
                    camera.position.x += flatFwdX * speed;
                    camera.position.z += flatFwdZ * speed;
                }
                if (e.key == SDL_SCANCODE_S) {
                    camera.position.x -= flatFwdX * speed;
                    camera.position.z -= flatFwdZ * speed;
                }
                if (e.key == SDL_SCANCODE_A) {
                    camera.position.x -= rightX * speed;
                    camera.position.z -= rightZ * speed;
                }
                if (e.key == SDL_SCANCODE_D) {
                    camera.position.x += rightX * speed;
                    camera.position.z += rightZ * speed;
                }
                break;
            }

            default:
                break;
        }
    }
}

void GameplayState::update(Game& game, float dt) {
    chunkManager->setFocus(camera.position);
    sunLight->setFocus(camera.position);
    requestChunksAroundCamera();
    chunkManager->update();
    sunLight->update();

    // --- Tile debug raycast -------------------------------------------------
    float dirX, dirY, dirZ;
    cameraDirection(camera.yaw, camera.pitch, dirX, dirY, dirZ);

    auto hit = castTileRay(*chunkManager, camera.position.x, camera.position.y,
                           camera.position.z, dirX, dirY, dirZ,
                           /*maxDistance=*/64.0f);

    if (tileDebugText) {
        if (!hit) {
            tileDebugText->setText("No tile");
        } else {
            const Tile& t = hit->tile;
            std::stringstream ss;

            ss << "Tile [" << hit->tx << ", " << hit->ty << ", " << hit->tz
               << "]\n";

            ss << "\n--- Block ---\n";
            ss << "block_id:  " << (int)t.block_id << "\n";

            ss << "\n--- Liquid ---\n";
            ss << "liquid:      " << (int)t.liquid << "\n";
            ss << "liquidLevel: " << (int)t.liquidLevel << "\n";

            ss << "\n--- Light ---\n";
            ss << "sun_exposure:     " << (int)t.lightLevel.sun_exposure
               << "\n";
            ss << "artificial_light: " << (int)t.lightLevel.artificial_light
               << "\n";

            tileDebugText->setText(ss.str());
        }
    }
}

void GameplayState::render(RenderTarget& target, UIContext& context) {
    context.renderer->setCamera(camera);

    // Poll mesh futures and upload any that finished this frame.
    chunkManager->uploadReadyMeshes(*context.renderer);

    float vp[16];
    const float* view = context.renderer->getViewMatrix();
    const float* proj = context.renderer->getProjMatrix();
    bx::mtxMul(vp, view, proj);

    Frustum frustum = Frustum::fromViewProj(vp);

    int camCX = int(camera.position.x) / Chunk::SIZE_X;
    int camCY = int(camera.position.y) / Chunk::SIZE_Y;
    int camCZ = int(camera.position.z) / Chunk::SIZE_Z;

    chunkManager->forEachReady([&](const ChunkCoord& coord, Chunk& chunk) {
        int dx = std::abs(int(coord.x) - camCX);
        int dy = std::abs(int(coord.y) - camCY);
        int dz = std::abs(int(coord.z) - camCZ);

        if (dx > CHUNK_RADIUS || dy > CHUNK_RADIUS || dz > CHUNK_RADIUS) return;

        // Build the AABB for this chunk in world space.
        // X and Z are tile axes, Y is height (slab).
        float wx = float(coord.x) * Chunk::SIZE_X;
        float wy = float(coord.y) * Chunk::SIZE_Y;
        float wz = float(coord.z) * Chunk::SIZE_Z;

        AABB aabb{wx,
                  wy,
                  wz,
                  wx + Chunk::SIZE_X,
                  wy + Chunk::SIZE_Y,
                  wz + Chunk::SIZE_Z};

        if (!frustum.intersects(aabb)) return;  // culled

        MeshHandle* mesh = chunkManager->getMesh(coord);
        if (!mesh) return;  // still building, skip this frame

        std::array<float, 16> transform;
        bx::mtxIdentity(transform.data());
        std::vector<ShaderProperty*> uniforms{&u_texture_array};
        target.drawVertexBuffer(mesh->get(), &blockShader, std::move(uniforms),
                                false, std::move(transform), DrawLayer::GAME);
    });

    ui_manager->render(target, context);
}

void GameplayState::requestChunksAroundCamera() {
    // Convert camera world-tile position to chunk coordinates.
    int camCX = int(camera.position.x) / Chunk::SIZE_X;
    int camCY = int(camera.position.y) / Chunk::SIZE_Y;
    int camCZ = int(camera.position.z) / Chunk::SIZE_Z;

    // Calculate max distance squared for bounds
    int maxDist = CHUNK_RADIUS;

    // Iterate by distance from camera
    for (int d = 0; d <= maxDist; ++d) {
        for (int dz = -d; dz <= d; ++dz) {
            for (int dx = -d; dx <= d; ++dx) {
                for (int dy = -d; dy <= d; ++dy) {
                    // Skip if not on the outer shell of current distance
                    if (std::abs(dx) != d && std::abs(dy) != d &&
                        std::abs(dz) != d)
                        continue;

                    // Ensure we're within the radius
                    if (std::abs(dx) > CHUNK_RADIUS ||
                        std::abs(dy) > CHUNK_RADIUS ||
                        std::abs(dz) > CHUNK_RADIUS)
                        continue;

                    int cx = camCX + dx;
                    int cy = camCY + dy;
                    int cz = camCZ + dz;

                    if (cx < 0 || cy < 0 || cz < 0) continue;

                    chunkManager->request(
                        {(unsigned)cx, (unsigned)cy, (unsigned)cz});
                }
            }
        }
    }
}
