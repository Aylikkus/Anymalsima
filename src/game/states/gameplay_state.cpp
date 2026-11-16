#include "gameplay_state.hpp"

#include <bx/math.h>

#include <iostream>

#include "gettext_wrapper.hpp"
#include "main_menu_state.hpp"
#include "render/frustrum.hpp"

// How many chunks out from the camera chunk to request and draw.
static constexpr int CHUNK_RADIUS = DETAIL_CHUNK_RADIUS;

GameplayState::GameplayState(Game& game, UIContext& context,
                             std::unique_ptr<World>&& world, SaveSlot saveSlot)
    : slot(std::move(saveSlot)) {
    // name, stage, texture_handle
    u_texture_array.initTexture("u_texture_array", 0,
                                context.assets->getMaterialAlbedoArray());
    // Normal maps share the albedo array's layer indices.
    u_normal_array.initTexture("u_normal_array", 1,
                               context.assets->getMaterialNormalArray());
    this->world = std::move(world);
    regionStore =
        std::make_unique<RegionStore>(slot.directory, context.threadPool);
    uiContext = &context;
    // "terrainDetail" (0 Low, 1 Medium, 2 High) applies on world load.
    const int detail = std::clamp(
        context.playerSettings
            ? context.playerSettings->getExtraOr<int>("terrainDetail", 0)
            : 0,
        0, TerrainLod::QualityCount - 1);
    const auto& quality = TerrainLod::Qualities[detail];
    camera.viewDistance = quality.viewDistance;
    worldEye.initVec4("u_world_eye", {0, 0, 0, quality.viewDistance});
    try {
        auto file = save_catalog::readTerrainEdits(slot);
        edits = std::make_unique<TerrainEdits>(std::move(file.columns));
        editsExtra = std::move(file.extra);
    } catch (const std::exception& e) {
        // Edits are derived from chunks: visited areas are rebuilt.
        std::cerr << "Terrain edits unreadable (" << e.what()
                  << "); starting empty\n";
        edits = std::make_unique<TerrainEdits>();
    }
    terrainLod = std::make_unique<TerrainLodManager>(
        *this->world, *context.assets, context.threadPool, quality,
        edits.get());
    lightRenderer = std::make_unique<LightRenderer>(*context.renderer);
    {
        std::vector<ShaderProperty*> lightUniforms;
        lightRenderer->appendUniforms(lightUniforms);
        terrainLod->setExtraUniforms(std::move(lightUniforms));
    }
    lightSources = std::make_unique<LightSourceRegistry>();
    loadLightSources(context.assets->getStorage<Block>());
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

    auto makeToolButton = [&](const std::string& label, int column,
                              EditTool value) {
        auto* button = ui_manager->createWidget(
            std::make_unique<TextButton>(context, "ui.black.9slice",
                                         "mouse_click"),
            root_ptr);
        button->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
        button->setText(label);
        button->setFontRole(FontRole::Bold);
        button->setGridPlacement({float(column), 15, 3, 1});
        // Clicking the armed tool again disarms it.
        button->onClick.connect([this, value]() {
            setTool(tool == value ? EditTool::None : value);
        });
        return button;
    };
    placeButton = makeToolButton(_("PLACE BLOCK"), 0, EditTool::Place);
    removeButton = makeToolButton(_("REMOVE BLOCK"), 3, EditTool::Remove);
    lightButton = makeToolButton(_("PLACE LIGHT"), 6, EditTool::PlaceLight);
    {
        const auto& blocks = context.assets->getStorage<Block>();
        const auto it = blocks.indexByName.find(TestLightBlock);
        if (it != blocks.indexByName.end())
            testLightId = blocks.items[it->second].id;
        else
            std::cerr << TestLightBlock
                      << " is not defined; PLACE LIGHT is disabled\n";
    }

    // Debug sky dimmer: there is no day/night cycle yet, and sky light
    // outshines block light outdoors.
    skyText = ui_manager->createWidget(std::make_unique<Text>(), root_ptr);
    skyText->setGridPlacement({9.5f, 15, 3, 1});
    skyText->setFontRole(FontRole::Bold);
    skyText->setVerticalAlign(VerticalAlign::Middle);
    skyText->setText(_F("SKY {0}%", 100));
    skySlider = ui_manager->createWidget(
        std::make_unique<Slider>(context, "ui.white.track_horizontal",
                                 "ui.white.thumb_circle"),
        root_ptr);
    skySlider->setRange(0.0f, 100.0f);
    skySlider->setStep(1.0f);
    skySlider->setAxis(ScrollAxis::Horizontal);
    skySlider->setGridPlacement({12, 15, 5, 1});
    skySlider->setValue(100.0f);
    skySlider->onValueChanged.connect([this](float value) {
        skyText->setText(_F("SKY {0}%", int(value)));
        lightRenderer->setSkyDarkness(1.f - value / 100.f);
    });

    generator = std::make_unique<LocationGenerator>();
    mesher = std::make_unique<ChunkMesher>(*context.assets);
    chunkManager = std::make_unique<ChunkManager>(
        *generator, context.threadPool, mesher.get(), *this->world.get(),
        context.assets);
    sunLight = std::make_unique<SunLightPropagator>(
        *chunkManager.get(), context.assets->getStorage<Block>(),
        context.threadPool);
    blockLight = std::make_unique<BlockLightPropagator>(
        *chunkManager, context.assets->getStorage<Block>(), context.threadPool,
        lightSources.get());
    lightVolumes = std::make_unique<LightVolumeManager>(
        *chunkManager, context.assets->getStorage<Block>(), blockLight.get());
    editor = std::make_unique<WorldEditor>(*chunkManager, sunLight.get(),
                                           context.assets->getStorage<Block>());
    blockLight->watch(*editor);
    // Comparing saved chunks with generated terrain is only meaningful when
    // the save's generator produced the same surface heights.
    const bool sameGenerator =
        slot.meta.generatorVersion >= WorldMeta::SurfaceVersion;
    if (!sameGenerator)
        std::cerr << "World generator " << slot.meta.generatorVersion
                  << " predates surface version " << WorldMeta::SurfaceVersion
                  << "; earlier edits are not detected for distant terrain\n";
    editTracker = std::make_unique<TerrainEditTracker>(*chunkManager, *edits,
                                                       sameGenerator);
    editTracker->watch(*editor);
    locationMgr = std::make_unique<LocationManager>(*chunkManager, *this->world,
                                                    regionStore.get());

    camera.terrainSky = true;
    if (slot.meta.camera) {
        const auto& saved = *slot.meta.camera;
        camera.position = saved.position;
        camera.yaw = saved.yaw;
        camera.pitch = saved.pitch;
        camera.distance = saved.distance;
    } else {
        const Vec2u initial = slot.meta.startRegion;
        const float regionTiles =
            CHUNKS_PER_REGION * (Chunk::SIZE_X + Chunk::SIZE_Z) / 2;

        float centerX = (float(initial.x) * regionTiles + regionTiles * 0.5f);
        float centerZ = (float(initial.y) * regionTiles + regionTiles * 0.5f);

        float centerY =
            chunkManager->generatedSurfaceY(static_cast<unsigned>(centerX),
                                            static_cast<unsigned>(centerZ)) +
            10.0f;

        camera.position = {centerX, centerY, centerZ};
        camera.pitch = -1.5f;
        camera.yaw = 0;
        camera.distance = 10.0f;
    }

    const Vec2u startRegion{unsigned(camera.position.x) / 128,
                            unsigned(camera.position.z) / 128};
    locationMgr->loadRegion(startRegion, 2);
    focusRegion = startRegion;

    requestChunksAroundCamera();
}

GameplayState::~GameplayState() {
    try {
        // Capture pending edits while their chunks are still loaded.
        editTracker->update();
        locationMgr->saveAll(true);
        regionStore->flush();
        saveTerrainEdits(true);
        saveLightSources(true);
        writeMeta();
    } catch (const std::exception& e) {
        std::cerr << "Saving world failed: " << e.what() << '\n';
    }
}

void GameplayState::writeMeta() {
    slot.meta.lastPlayed = WorldMeta::now();
    slot.meta.camera = WorldMeta::CameraState{camera.position, camera.yaw,
                                              camera.pitch, camera.distance};
    try {
        save_catalog::writeMeta(slot);
    } catch (const std::exception& e) {
        std::cerr << "Saving world.json failed: " << e.what() << '\n';
    }
}

void GameplayState::saveTerrainEdits(bool wait) {
    using namespace std::chrono_literals;
    if (editsWrite.valid()) {
        if (!wait && editsWrite.wait_for(0s) != std::future_status::ready)
            return;  // the previous write is still running
        try {
            editsWrite.get();
        } catch (const std::exception& e) {
            std::cerr << "Saving terrain edits failed: " << e.what() << '\n';
            edits->dirty = true;
        }
    }
    if (!edits->dirty) return;
    edits->dirty = false;
    auto write = [target = slot, columns = edits->snapshot(),
                  extra = editsExtra] {
        save_catalog::writeTerrainEdits(target, *columns, extra);
    };
    if (!wait) {
        editsWrite = uiContext->threadPool.submit_task(std::move(write));
        return;
    }
    try {
        write();
    } catch (const std::exception& e) {
        std::cerr << "Saving terrain edits failed: " << e.what() << '\n';
        edits->dirty = true;
    }
}

void GameplayState::loadLightSources(const DataStorage<Block>& blocks) {
    try {
        auto file = save_catalog::readLightSources(slot);
        lightsExtra = std::move(file.extra);
        for (const auto& record : file.lights) {
            // Emission comes from today's definitions; lights whose block
            // no longer reaches far are dropped.
            if (record.block == 0 || record.block > blocks.items.size())
                continue;
            const auto& block = blocks.get(record.block);
            if (block.light && block.light->isFar())
                lightSources->setBlockLight(record.tile, record.block,
                                            *block.light);
        }
    } catch (const std::exception& e) {
        // Derived from chunks: visited areas are rebuilt.
        std::cerr << "Light sources unreadable (" << e.what()
                  << "); starting empty\n";
    }
    lightSources->dirty = false;
}

void GameplayState::saveLightSources(bool wait) {
    using namespace std::chrono_literals;
    if (lightsWrite.valid()) {
        if (!wait && lightsWrite.wait_for(0s) != std::future_status::ready)
            return;
        try {
            lightsWrite.get();
        } catch (const std::exception& e) {
            std::cerr << "Saving light sources failed: " << e.what() << '\n';
            lightSources->dirty = true;
        }
    }
    if (!lightSources->dirty) return;
    lightSources->dirty = false;
    std::vector<light_sources_codec::Record> records;
    lightSources->forEach([&](LightSourceRegistry::Id, const LightSource& s) {
        if (s.owner == LightSource::Owner::Block)
            records.push_back({s.tile, s.block});
    });
    auto write = [target = slot, records = std::move(records),
                  extra = lightsExtra]() mutable {
        save_catalog::writeLightSources(target, std::move(records), extra);
    };
    if (!wait) {
        lightsWrite = uiContext->threadPool.submit_task(std::move(write));
        return;
    }
    try {
        write();
    } catch (const std::exception& e) {
        std::cerr << "Saving light sources failed: " << e.what() << '\n';
        lightSources->dirty = true;
    }
}

void GameplayState::updateAutosave(float dt) {
    // Rolling checkpoint: one region per frame keeps copies off the frame
    // budget; regions unloaded meanwhile were already saved on unload.
    if (!autosaveQueue.empty()) {
        locationMgr->saveRegion(autosaveQueue.back());
        autosaveQueue.pop_back();
        if (autosaveQueue.empty()) {
            saveTerrainEdits(false);
            saveLightSources(false);
            writeMeta();
        }
        return;
    }
    autosaveTimer += dt;
    if (autosaveTimer < AutosaveInterval) return;
    autosaveTimer = 0;
    autosaveQueue = locationMgr->loaded();
    if (autosaveQueue.empty()) {
        saveTerrainEdits(false);
        saveLightSources(false);
        writeMeta();
    }
}

void GameplayState::handleInput(Game& game, const InputEvent& e,
                                UIContext& context, float dt) {
    if (!ui_manager->handleInput(e, context)) {
        switch (e.type) {
            case InputEvent::KeyPress:
                // Leaving the state saves the world in the destructor.
                if (e.pressed && e.key == SDL_SCANCODE_ESCAPE)
                    game.changeState<MainMenuState>(game, context);
                break;

            case InputEvent::MouseClick:
                if (e.button == InputEvent::LEFT_BUTTON && e.pressed &&
                    tool != EditTool::None)
                    useTool(e.x, e.y);
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

void GameplayState::setTool(EditTool value) {
    tool = value;
    const Color armed{120, 200, 120, 255};
    const Color idle{232, 232, 232, 255};
    placeButton->getButton()->setNormalColor(tool == EditTool::Place ? armed
                                                                     : idle);
    removeButton->getButton()->setNormalColor(tool == EditTool::Remove ? armed
                                                                       : idle);
    lightButton->getButton()->setNormalColor(
        tool == EditTool::PlaceLight ? armed : idle);
}

std::optional<TileRaycastHit> GameplayState::pickTile(int mouseX, int mouseY) {
    // Same view and projection as render(): bx::mtxLookAt from the orbit
    // eye toward camera.position, 60 degree vertical field of view.
    const Vec2 viewport = uiContext->getWindowSize();
    if (viewport.x <= 0 || viewport.y <= 0) return std::nullopt;
    const Vec3 eye = camera.eye();
    bx::Vec3 forward = bx::normalize(bx::Vec3{camera.position.x - eye.x,
                                              camera.position.y - eye.y,
                                              camera.position.z - eye.z});
    const bx::Vec3 right = bx::normalize(bx::cross(bx::Vec3{0, 1, 0}, forward));
    const bx::Vec3 up = bx::cross(forward, right);
    const float tanHalf = std::tan(bx::toRad(60.f) * 0.5f);
    const float ndcX = 2.f * float(mouseX) / viewport.x - 1.f;
    const float ndcY = 1.f - 2.f * float(mouseY) / viewport.y;
    const float aspect = viewport.x / viewport.y;
    const bx::Vec3 dir = bx::normalize(
        bx::add(forward, bx::add(bx::mul(right, ndcX * aspect * tanHalf),
                                 bx::mul(up, ndcY * tanHalf))));
    return castTileRay(*chunkManager, eye.x, eye.y, eye.z, dir.x, dir.y, dir.z,
                       camera.distance + 64.f, /*hitLiquids=*/false);
}

void GameplayState::useTool(int mouseX, int mouseY) {
    const auto hit = pickTile(mouseX, mouseY);
    if (!hit) return;
    if (tool == EditTool::Remove) {
        editor->removeBlock({hit->tx, hit->ty, hit->tz});
        return;
    }
    const auto& blocks = uiContext->assets->getStorage<Block>();
    if (blocks.items.empty() || (!hit->nx && !hit->ny && !hit->nz)) return;
    // Against the face under the cursor: the debug light, or else the first
    // defined block.
    if (tool == EditTool::PlaceLight && !testLightId) return;
    const uint8_t block =
        tool == EditTool::PlaceLight ? *testLightId : blocks.items.front().id;
    editor->placeBlock(
        {hit->tx + hit->nx, hit->ty + hit->ny, hit->tz + hit->nz}, block);
}

void GameplayState::update(Game& game, float dt) {
    // Before any region can unload this frame.
    editTracker->update();
    // X wraps around the world (stored canonically in [0, W)); Z is bounded.
    camera.position.x = float(world->wrap().wrapX(camera.position.x));
    if (camera.position.x >= float(world->wrap().tiles))
        camera.position.x = 0;  // float rounding of a value just below W
    camera.position.z =
        std::clamp(camera.position.z, 0.f, float(world->height * 128 - 1));
    const Vec2u region{unsigned(camera.position.x) / 128,
                       unsigned(camera.position.z) / 128};
    if (!focusRegion || *focusRegion != region) {
        locationMgr->onPlayerRegionChanged(region);
        focusRegion = region;
    }
    locationMgr->update();
    regionStore->update();
    updateAutosave(dt);
    terrainLod->update(camera, uiContext->getWindowSize());
    chunkManager->setFocus(camera.position);
    sunLight->setFocus(camera.position);
    blockLight->setFocus(camera.position);
    requestChunksAroundCamera();
    chunkManager->update();
    sunLight->update();
    blockLight->update();

    // --- Tile debug raycast -------------------------------------------------
    float dirX, dirY, dirZ;
    cameraDirection(camera.yaw, camera.pitch, dirX, dirY, dirZ);

    auto hit = castTileRay(*chunkManager, camera.position.x, camera.position.y,
                           camera.position.z, dirX, dirY, dirZ,
                           /*maxDistance=*/64.0f);

    if (tileDebugText) {
        if (!hit) {
            tileDebugText->setText(
                "No tile\nLOD: " + std::to_string(terrainLod->patchCount()) +
                " patches, " +
                std::to_string(terrainLod->residentBytes() / 1024) + " KiB\n" +
                std::to_string(terrainLod->jobCount()) + " jobs, " +
                std::to_string(terrainLod->backlog()) + " pending\n" +
                std::to_string(edits->size()) + " edited columns, " +
                std::to_string(terrainLod->structureCount()) +
                " structure slabs");
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
            if (const auto* e = blockLight->emission(t.block_id))
                ss << "emits: radius " << e->radius << ", falloff "
                   << e->falloff << "\n";
            ss << "far lights: " << lightRenderer->lightCount() << " lit, "
               << lightRenderer->glareCount() << " glare\n";

            tileDebugText->setText(ss.str());
        }
    }
}

void GameplayState::render(RenderTarget& target, UIContext& context) {
    context.renderer->setCamera(camera);

    // Poll mesh futures and upload any that finished this frame.
    chunkManager->uploadReadyMeshes(*context.renderer);
    // After uploads: deferred light volumes switch with their new meshes.
    lightVolumes->update(camera.position);
    lightRenderer->update(*lightSources, camera, world->wrap(),
                          context.getWindowSize());
    terrainLod->render(target, *context.renderer, *chunkManager, camera,
                       context.getWindowSize());
    const Vec3 eye = camera.eye();
    worldEye.setVec4({eye.x, eye.y, eye.z, camera.viewDistance});

    float vp[16], view[16], proj[16];
    bx::mtxLookAt(view, {eye.x, eye.y, eye.z},
                  {camera.position.x, camera.position.y, camera.position.z},
                  {0, 1, 0});
    const auto viewport = context.getWindowSize();
    bx::mtxProj(proj, 60.f, viewport.x / std::max(1.f, viewport.y), 1.f,
                camera.viewDistance + 512.f, bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(vp, view, proj);

    Frustum frustum = Frustum::fromViewProj(vp);

    int camCX = int(camera.position.x) / Chunk::SIZE_X;
    int camCY = int(camera.position.y) / Chunk::SIZE_Y;
    int camCZ = int(camera.position.z) / Chunk::SIZE_Z;
    const WorldWrap wrap = world->wrap();

    chunkManager->forEachMeshed([&](const ChunkCoord& coord) {
        // Chunks are drawn at their copy nearest the camera, so the X seam
        // is invisible.
        const int64_t deltaX = wrap.deltaChunk(coord.x, camCX);
        int dx = int(std::abs(deltaX));
        int dy = std::abs(int(coord.y) - camCY);
        int dz = std::abs(int(coord.z) - camCZ);

        if (dx > CHUNK_RADIUS || dy > CHUNK_RADIUS || dz > CHUNK_RADIUS ||
            !terrainLod->drawsColumn(coord.x, coord.z))
            return;

        // Build the AABB for this chunk in world space.
        // X and Z are tile axes, Y is height (slab).
        float wx = float(camCX + deltaX) * Chunk::SIZE_X;
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

        // Chunk meshes are chunk-local; place them relative to the eye.
        auto transform = relativeTransform(camera, wx, wy, wz);
        std::vector<ShaderProperty*> uniforms{&u_texture_array, &u_normal_array,
                                              &worldEye};
        lightRenderer->appendUniforms(uniforms);
        // Without a light volume the chunk would render unlit; skip it.
        if (!lightVolumes->bind(coord, uniforms, wx)) return;
        target.drawVertexBuffer(mesh->get(), &blockShader, std::move(uniforms),
                                false, std::move(transform), DrawLayer::GAME);
    });
    lightRenderer->drawGlare(target, camera);

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

                    // X wraps around the world.
                    int cx = int(world->wrap().wrapChunk(camCX + dx));
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
