#include "gameplay_state.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "gettext_wrapper.hpp"
#include "main_menu_state.hpp"
#include "render/frustrum.hpp"
#include "world/day_night.hpp"
#include "world/generated_hydrology.hpp"
#include "world/world_calendar.hpp"
#include "world/world_climate.hpp"

namespace {

using Profile = TileShapeProfile;
constexpr int WallThickness[] = {10, 25, 50, 75};
constexpr const char* FacingNames[] = {"+Z", "+X", "−Z", "−X"};
constexpr const char* RampForms[] = {__("Straight"), __("Outer corner"),
                                     __("Inner corner")};
// Each row lists run 1, both pieces of run 2, and all three of run 3.
constexpr Profile RampProfiles[][6] = {
    {Profile::Ramp, Profile::RampHalfLow, Profile::RampHalfHigh,
     Profile::RampThirdLow, Profile::RampThirdMiddle, Profile::RampThirdHigh},
    {Profile::OuterCorner, Profile::OuterHalfLow, Profile::OuterHalfHigh,
     Profile::OuterThirdLow, Profile::OuterThirdMiddle,
     Profile::OuterThirdHigh},
    {Profile::InnerCorner, Profile::InnerHalfLow, Profile::InnerHalfHigh,
     Profile::InnerThirdLow, Profile::InnerThirdMiddle,
     Profile::InnerThirdHigh},
};
constexpr const char* ShapeNames[] = {__("Cube"),  __("Slab"),
                                      __("Ramp"),  __("Wall"),
                                      __("Layer"), __("Ramp infill")};
constexpr auto LayerSizes = std::array{
    tile_geometry::LayerThickness::Cm5,  tile_geometry::LayerThickness::Cm10,
    tile_geometry::LayerThickness::Cm25, tile_geometry::LayerThickness::Cm50,
    tile_geometry::LayerThickness::Cm75, tile_geometry::LayerThickness::Cm100};

std::string pieceEditLabel(CellPieceEdit status) {
    switch (status) {
        case CellPieceEdit::Applied:
            return _("Ready");
        case CellPieceEdit::Overlap:
            return _("Pieces overlap");
        case CellPieceEdit::Blocked:
            return _("Blocked by a plant");
        case CellPieceEdit::NotLoaded:
            return _("Tile unavailable");
        case CellPieceEdit::Stale:
            return _("Tile changed");
        case CellPieceEdit::TooMany:
            return _("Piece limit reached");
        case CellPieceEdit::Invalid:
            return _("Invalid piece or tile");
    }
    return "";
}

// Settlement reads the chunk's own solids, never the environmental snow
// projection that is derived from it.
class ChunkSnowTerrain final : public snow::SnowTerrain {
   public:
    ChunkSnowTerrain(ChunkManager& chunks, const World& world)
        : chunks(chunks), world(world) {}

    std::optional<tile_geometry::CellGeometry> cell(
        TilePos pos) const override {
        return chunks.rawCellGeometryAt(pos);
    }
    int wrapX(int x) const override { return int(chunks.wrap().wrapTile(x)); }
    bool inBounds(int x, int z) const override {
        if (world.width <= 0 || world.height <= 0) return x >= 0 && z >= 0;
        return x >= 0 && x < chunks.wrap().tiles && z >= 0 &&
               z < world.height * CHUNKS_PER_REGION * Chunk::SIZE_Z;
    }
    int minY() const override { return 0; }
    int maxY() const override { return WORLD_HEIGHT_Y; }

   private:
    ChunkManager& chunks;
    const World& world;
};

std::string describeShape(TileShape shape) {
    const auto profile = shape.profile();
    const auto facing = FacingNames[unsigned(shape.facing())];
    if (profile == Profile::Cube) return _(ShapeNames[0]);
    if (profile == Profile::Slab) return _(ShapeNames[1]);
    if (profile >= Profile::Wall10 && profile <= Profile::Wall75) {
        const auto thickness =
            WallThickness[int(profile) - int(Profile::Wall10)];
        return fmt::format(fmt::runtime(_("Wall, {0} cm, {1}")), thickness,
                           facing);
    }
    if (profile == Profile::DiagonalRamp)
        return fmt::format(fmt::runtime(_("Corner ramp, {0}")), facing);
    for (unsigned form = 0; form < std::size(RampProfiles); ++form) {
        unsigned index = 0;
        for (unsigned run = 1; run <= 3; ++run)
            for (unsigned segment = 1; segment <= run; ++segment, ++index)
                if (RampProfiles[form][index] == profile)
                    return fmt::format(
                        fmt::runtime(_("{0} ramp, {1} m, piece {2}, {3}")),
                        _(RampForms[form]), run, segment, facing);
    }
    return {};
}

}  // namespace

GameplayState::GameplayState(Game& game, UIContext& context,
                             std::shared_ptr<World> world, SaveSlot saveSlot,
                             std::optional<std::vector<TreeLandmark>> landmarks)
    : slot(std::move(saveSlot)) {
    // name, stage, texture_handle
    u_texture_array.initTexture("u_texture_array", 0,
                                context.assets->getMaterialAlbedoArray());
    // Normal maps share the albedo array's layer indices.
    u_normal_array.initTexture("u_normal_array", 1,
                               context.assets->getMaterialNormalArray());
    this->world = std::move(world);
    if (this->world->hydrology) {
        this->world->reservoirs = std::make_shared<Reservoirs>(
            this->world->hydrology, uint64_t(slot.meta.worldDays * 86400));
        save_catalog::readReservoirs(slot, *this->world->reservoirs);
        savedWaterRevision = this->world->reservoirs->revision();
    }
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
    // The shadowed-light budget follows the terrain detail setting.
    lightRenderer = std::make_unique<LightRenderer>(*context.renderer, detail);
    {
        std::vector<ShaderProperty*> patchLights, blockLights;
        lightRenderer->appendUniforms(patchLights, LightRenderer::Program::Lod);
        lightRenderer->appendUniforms(blockLights,
                                      LightRenderer::Program::Block);
        terrainLod->setExtraUniforms(std::move(patchLights),
                                     std::move(blockLights));
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

    tileDebugText =
        ui_manager->createWidget(std::make_unique<Text>(), tile_debug_panel);
    tileDebugText->setGridPlacement({0, 0, 13, 26});
    tileDebugText->setBackgroundColor({255, 255, 255, 0});
    tileDebugText->setText(_("No tile"));

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
    placeButton = makeToolButton(_("PLACE PIECE"), 0, EditTool::Place);
    removeButton = makeToolButton(_("REMOVE PIECE"), 3, EditTool::Remove);
    lightButton = makeToolButton(_("PLACE LIGHT"), 6, EditTool::PlaceLight);
    {
        const auto& blocks = context.assets->getStorage<Block>();
        const auto id = [&](const char* name, uint8_t fallback) {
            const auto found = blocks.indexByName.find(name);
            return found == blocks.indexByName.end()
                       ? fallback
                       : blocks.items[found->second].id;
        };
        stoneId = id("blocks.slate",
                     blocks.items.empty() ? 0 : blocks.items.front().id);
        timberId = id("blocks.oak_timber", 0);
        snowId = id("blocks.snow", 0);
        if (!snowId)
            std::cerr
                << "blocks.snow is not defined; snow rendering is disabled\n";
        const auto it = blocks.indexByName.find(TestLightBlock);
        if (it != blocks.indexByName.end())
            testLightId = blocks.items[it->second].id;
        else
            std::cerr << TestLightBlock
                      << " is not defined; PLACE LIGHT is disabled\n";
    }

    harvestButton = makeToolButton(_("HARVEST"), 9, EditTool::Harvest);
    // Debug: plant a tree sapling; clicking again while armed picks the
    // next species.
    plantButton = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        root_ptr);
    plantButton->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    plantButton->setFontRole(FontRole::Bold);
    plantButton->setGridPlacement({12, 15, 7, 1});
    plantButton->onClick.connect([this]() {
        if (tool == EditTool::PlantTree) {
            const size_t count = chunkManager->treeSpecies().species().size();
            if (count) plantSpecies = (plantSpecies + 1) % count;
            updatePlantLabel();
        } else {
            setTool(EditTool::PlantTree);
        }
    });
    // Debug game speed: normal (a day per 72 minutes), then a day every
    // 72 s, 6 s and 0.6 s, and finally paused time.
    timeButton = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        root_ptr);
    timeButton->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    timeButton->setFontRole(FontRole::Bold);
    timeButton->setGridPlacement({20, 15, 4, 1});
    timeButton->onClick.connect([this]() {
        static constexpr double Speeds[] = {0, 1, 60, 720, 7200};
        size_t next = 0;
        for (size_t i = 0; i < std::size(Speeds); ++i)
            if (clock.speed() == Speeds[i]) next = (i + 1) % std::size(Speeds);
        clock.setSpeed(Speeds[next]);
    });

    auto makeDebugButton = [&](GridPlacement placement, auto action) {
        auto* button = ui_manager->createWidget(
            std::make_unique<TextButton>(context, "ui.black.9slice",
                                         "mouse_click"),
            root_ptr);
        button->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
        button->setFontRole(FontRole::Bold);
        button->setGridPlacement(placement);
        button->onClick.connect([this, action]() {
            action();
            updateShapeControls();
        });
        return button;
    };
    snow5Button = makeDebugButton({0, 11, 6, 1}, [this] {
        snowAddVolume = 51250;
        setTool(EditTool::SnowAdd);
    });
    snow5Button->setText(_("SNOW 5 cm fixture"));
    snow50Button = makeDebugButton({6, 11, 6, 1}, [this] {
        snowAddVolume = 512500;
        setTool(EditTool::SnowAdd);
    });
    snow50Button->setText(_("SNOW +50 cm"));
    snow5mButton = makeDebugButton({12, 11, 6, 1}, [this] {
        snowAddVolume = 5125000;
        setTool(EditTool::SnowAdd);
    });
    snow5mButton->setText(_("SNOW +5 m"));
    snowClearButton = makeDebugButton({18, 11, 6, 1}, [this] {
        snowSettlement->clear();
        rebuildSnow();
        lastBuildAction = _("Snow cleared");
    });
    snowClearButton->setText(_("CLEAR SNOW"));
    shapeButton = makeDebugButton({0, 14, 6, 1}, [this] {
        shapeKind =
            ShapeKind((unsigned(shapeKind) + 1) % std::size(ShapeNames));
    });
    facingButton = makeDebugButton({6, 14, 6, 1}, [this] {
        facing = (facing + 1) % std::size(FacingNames);
    });
    thicknessButton = makeDebugButton({12, 14, 6, 1}, [this] {
        if (shapeKind == ShapeKind::Layer)
            layerThickness = (layerThickness + 1) % LayerSizes.size();
        else if (shapeKind == ShapeKind::RampInfill)
            infillHeight = infillHeight % 20 + 1;
        else
            thickness = (thickness + 1) % std::size(WallThickness);
    });
    formButton = makeDebugButton({0, 13, 6, 1}, [this] {
        rampForm = (rampForm + 1) % std::size(RampForms);
    });
    runButton = makeDebugButton({6, 13, 6, 1}, [this] {
        rampRun = rampRun % 3 + 1;
        rampSegment = std::min(rampSegment, rampRun);
    });
    segmentButton = makeDebugButton(
        {12, 13, 6, 1}, [this] { rampSegment = rampSegment % rampRun + 1; });
    walkButton = makeDebugButton({18, 14, 6, 1}, [this] { toggleWalk(); });
    walkButton->setText(_("Walk mode"));
    materialButton = makeDebugButton({0, 12, 6, 1},
                                     [this] { timber = timberId && !timber; });
    targetButton =
        makeDebugButton({6, 12, 6, 1}, [this] { sameTile = !sameTile; });
    elevationButton = makeDebugButton(
        {12, 12, 6, 1}, [this] { elevation = (elevation + 1) % 20; });
    updateShapeControls();
    previewColor.initVec4("u_color", {0, 0, 0, 1});

    generator = std::make_unique<LocationGenerator>();
    mesher = std::make_unique<ChunkMesher>(*context.assets);
    chunkManager = std::make_unique<ChunkManager>(
        *generator, context.threadPool, mesher.get(), *this->world.get(),
        context.assets, std::move(landmarks));
    snowTerrain =
        std::make_unique<ChunkSnowTerrain>(*chunkManager, *this->world);
    snowSettlement = std::make_unique<snow::SnowSettlement>(*snowTerrain);
    snowCover = std::make_unique<snow::SnowCover>();
    setupConstructionFixture();
    setupSnowFixture();
    if (constructionSite) {
        auto* button = makeDebugButton({18, 13, 6, 1}, [this] {
            if (probe) toggleWalk();
            const auto [x, y, z] = *constructionSite;
            camera.position = {x + .5f, y + 1.f, z + .5f};
            camera.yaw = 0;
            camera.pitch = -.48f;
            camera.distance = 12.f;
        });
        button->setText(_("Construction site"));
    }
    if (snowFixtureSite) {
        auto* button = makeDebugButton({18, 10, 6, 1}, [this] {
            if (probe) toggleWalk();
            const auto [x, y, z] = *snowFixtureSite;
            camera.position = {x + .5f, y + 3.f, z - 3.5f};
            camera.yaw = 0;
            camera.pitch = -.35f;
            camera.distance = 10.f;
        });
        button->setText(_("Snow site"));
    }
    if (snowDropSite) {
        auto* button = makeDebugButton({12, 10, 6, 1}, [this] {
            if (probe) toggleWalk();
            const auto [x, y, z] = *snowDropSite;
            camera.position = {x + .5f, y + 3.f, z + 2.5f};
            camera.yaw = bx::kPi;
            camera.pitch = -.35f;
            camera.distance = 9.f;
        });
        button->setText(_("Snow drop"));
    }
    // World-wide sources must be installed before any chunk is requested.
    try {
        auto file = save_catalog::readStructures(slot);
        chunkManager->setStructureOverrides(std::move(file.voxels));
        chunkManager->setTreeOverrides(
            std::make_shared<const TreeOverrideMap>(std::move(file.trees)));
        structuresExtra = std::move(file.extra);
        importLegacyStructures = file.migratedTrees;
        savedStructureOverrideRevision =
            chunkManager->structureOverrideRevision();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Structure save unreadable: ") +
                                 e.what());
    }
    skyMap = std::make_unique<SkyHeightMap>(*chunkManager, *edits,
                                            context.assets->getStorage<Block>(),
                                            context.threadPool);
    try {
        auto file = save_catalog::readSky(slot);
        skyMap->setOverrides(std::move(file.overrides));
        skyExtra = std::move(file.extra);
    } catch (const std::exception& e) {
        // Only edits the sources do not show (holes dug into tree wood)
        // are lost; loaded chunks are observed again when edited.
        std::cerr << "sky.dat unreadable (" << e.what()
                  << "); starting without overrides\n";
    }
    skyMap->dirty = false;
    // The map covers every chunk column with loaded chunks.
    skyReadyConnection = chunkManager->becameReady.connect(
        [this](const ChunkCoord& c) { skyMap->request(c.x, c.z); });
    sunLight = std::make_unique<SunLightPropagator>(
        *chunkManager.get(), context.assets->getStorage<Block>(),
        context.threadPool, skyMap.get());
    blockLight = std::make_unique<BlockLightPropagator>(
        *chunkManager, context.assets->getStorage<Block>(), context.threadPool,
        lightSources.get());
    lightVolumes = std::make_unique<LightVolumeManager>(
        *chunkManager, context.assets->getStorage<Block>(), blockLight.get(),
        sunLight.get());
    floraRenderer = std::make_unique<FloraRenderer>(
        *chunkManager, *context.assets, *this->world, *context.renderer);
    treeRenderer = std::make_unique<TreeRenderer>(
        *chunkManager, *context.assets, *this->world, *context.renderer,
        context.threadPool, quality.chunkRadius);
    // Coloured lamps tint plants and crowns (declared after blockLight).
    floraRenderer->setBlockLight(blockLight.get());
    floraRenderer->setSkyLight(*skyMap);
    treeRenderer->setSkyLight(*skyMap);
    treeRenderer->setBlockLight(blockLight.get());
    treeRenderer->setVoxelLandmarks(terrainLod->drawsStructureVoxels());
    // Trees hand their wood over to blocks only where render() draws them.
    treeRenderer->setChunkDrawn([this](const ChunkCoord& c) {
        return terrainLod->drawsChunk(c) && chunkManager->hasUploadedMesh(c);
    });
    editor = std::make_unique<WorldEditor>(*chunkManager, sunLight.get(),
                                           context.assets->getStorage<Block>());
    blockLight->watch(*editor);
    skyMap->watch(*editor);
    clock.set(slot.meta.worldDays);
    floraSim = std::make_unique<FloraSimulation>(*chunkManager, *context.assets,
                                                 *this->world, *skyMap);
    floraSim->watch(*editor);
    treeSim = std::make_unique<TreeSimulation>(*chunkManager, *editor);
    updatePlantLabel();
    // Comparing saved chunks with generated terrain is only meaningful when
    // the save's generator produced the same surface heights. readWorldMap
    // selects the saved generation's water baseline for both the tracker
    // and new chunks at the explored frontier.
    const bool sameGenerator =
        slot.meta.generatorVersion >= WorldMeta::SurfaceVersion;
    if (!sameGenerator)
        std::cerr << "World generator " << slot.meta.generatorVersion
                  << " predates surface version " << WorldMeta::SurfaceVersion
                  << "; earlier edits are not detected for distant terrain\n";
    editTracker = std::make_unique<TerrainEditTracker>(*chunkManager, *edits,
                                                       sameGenerator);
    editTracker->watch(*editor);
    chunkManager->setWaterTerrainEdits(edits.get());
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

    // Voxel geometry changes re-render the shadow faces that can see it.
    auto chunkChanged = [this](const ChunkCoord& c) {
        const std::array<double, 3> min{double(c.x) * Chunk::SIZE_X,
                                        double(c.y) * Chunk::SIZE_Y,
                                        double(c.z) * Chunk::SIZE_Z};
        lightRenderer->shadows().geometryChanged(
            min,
            {min[0] + Chunk::SIZE_X, min[1] + Chunk::SIZE_Y,
             min[2] + Chunk::SIZE_Z},
            this->world->wrap().tiles);
    };
    meshConnection = chunkManager->meshUploaded.connect(chunkChanged);
    removedConnection = chunkManager->removed.connect(chunkChanged);

    const Vec2u startRegion{unsigned(camera.position.x) / 128,
                            unsigned(camera.position.z) / 128};
    locationMgr->loadRegion(startRegion, 2);
    focusRegion = startRegion;

    requestChunksAroundCamera();
}

void GameplayState::setupConstructionFixture() {
    const char* enabled = std::getenv("ANYM_CONSTRUCTION_FIXTURE");
    if (!enabled || std::string_view(enabled) != "1" || !stoneId || !timberId)
        return;
    const int x = int(slot.meta.startRegion.x) * 128 + 64;
    const int z = int(slot.meta.startRegion.y) * 128 + 64;
    int y = 0;
    std::array<std::array<int, 7>, 7> terrain{};
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            const int top = chunkManager->generatedSurfaceY(x + dx, z + dz);
            terrain[dz + 3][dx + 3] = top;
            y = std::max(y, top + 1);
        }
    if (y + 4 >= WORLD_HEIGHT_Y) return;

    using namespace structure_voxels;
    using tile_geometry::LayerThickness;
    using tile_geometry::TilePiece;
    constexpr uint64_t Key = 0xf000000000000005ULL;
    Override layout;
    layout.mode = OverrideMode::Replace;
    layout.anchorX = x;
    layout.anchorZ = z;
    layout.landmark = true;
    // Level deck with footings on the sampled terrain, spanning a chunk seam.
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            layout.voxels[{x + dx, y, z + dz}] = OverrideVoxel(stoneId);
            if (std::abs(dx) != 3 || std::abs(dz) != 3) continue;
            for (int foot = terrain[dz + 3][dx + 3] + 1; foot < y; ++foot)
                layout.voxels[{x + dx, foot, z + dz}] = OverrideVoxel(stoneId);
        }

    const auto put = [&](int dx, int dy, int dz,
                         std::initializer_list<TilePiece> pieces) {
        const auto& first = *pieces.begin();
        OverrideVoxel voxel(first.blockId,
                            first.legacyShape().value_or(TileShape{}));
        for (const auto& piece : pieces)
            voxel.pieces.push_back({piece, {PieceOrigin::Structure, Key}});
        layout.voxels[{x + dx, y + dy, z + dz}] = std::move(voxel);
    };
    const auto wall = TileShape::make(Profile::Wall25);
    put(-1, 1, -1,
        {TilePiece::layer(1, stoneId, LayerThickness::Cm5),
         TilePiece{2, timberId, wall, {0, .05f, 0}}});
    for (int dy = 1; dy <= 2; ++dy)
        for (int dx : {-2, 1})
            layout.voxels[{x + dx, y + dy, z + 1}] = OverrideVoxel(stoneId);
    for (int dx = -2; dx <= 1; ++dx)
        put(dx, 3, 1,
            {TilePiece{1,
                       timberId,
                       wall,
                       {0, .75f, 0},
                       tile_geometry::PieceFill::BelowProfile,
                       .25f}});
    constructionSite = TilePos{x, y, z};
    chunkManager->setGeneratedStructureKinds(applyOverrides(
        {}, {{"construction_fixture", {{Key, std::move(layout)}}}}));
}

// A reproducible playcheck scene for step 07: a one-square-metre level
// landing with a one-metre 45-degree ramp, a second pair straddling a chunk
// boundary, and a diagonal ramp with two low approaches. Snow is added by the
// debug buttons; the scene itself is generated content, like the construction
// deck.
void GameplayState::setupSnowFixture() {
    const char* enabled = std::getenv("ANYM_SNOW_FIXTURE");
    if (!enabled || std::string_view(enabled) != "1" || !stoneId) return;
    const int x = int(slot.meta.startRegion.x) * 128 + 64;
    const int z = int(slot.meta.startRegion.y) * 128 + 64;
    int maxTop = 0;
    for (int dz = -4; dz <= 8; ++dz)
        for (int dx = -4; dx <= 8; ++dx)
            maxTop = std::max(maxTop,
                              chunkManager->generatedSurfaceY(x + dx, z + dz));
    const int groundY = maxTop + 1;
    if (groundY + 6 >= WORLD_HEIGHT_Y) return;

    using namespace structure_voxels;
    constexpr uint64_t Key = 0xf000000000000007ULL;
    Override layout;
    layout.mode = OverrideMode::Replace;
    layout.anchorX = x;
    layout.anchorZ = z;
    layout.landmark = true;
    const auto support = [&](int px, int pz) {
        const int top = chunkManager->generatedSurfaceY(px, pz);
        for (int y = top + 1; y <= groundY; ++y)
            layout.voxels[{px, y, pz}] = OverrideVoxel(stoneId);
    };
    const auto cube = [&](int px, int py, int pz) {
        layout.voxels[{px, py, pz}] = OverrideVoxel(stoneId);
    };

    const auto ramp = [&](int px, int pz, TileShapeProfile profile) {
        support(px, pz);
        cube(px, groundY, pz);
        layout.voxels[{px, groundY + 1, pz}] =
            OverrideVoxel(stoneId, TileShape::make(profile));
    };
    const auto rock = [&](int px, int pz) {
        support(px, pz);
        cube(px, groundY, pz);
        cube(px, groundY + 1, pz);
    };

    // One-square-metre numeric landing: 5 cm is 50,000 cm3 above it and the
    // 45-degree toe adds 1,250 cm3 at the step 06 line. Walls close its other
    // sides so the amount reaches a level instead of draining.
    support(x, z);
    cube(x, groundY, z);
    rock(x - 1, z);
    rock(x + 1, z);
    ramp(x, z + 1, TileShapeProfile::Ramp);
    rock(x, z + 2);
    rock(x, z + 3);

    // The same ramp repeated across the chunk face at x (a chunk boundary).
    for (int dx = -1; dx <= 0; ++dx) {
        support(x + dx, z - 3);
        cube(x + dx, groundY, z - 3);
        ramp(x + dx, z - 2, TileShapeProfile::Ramp);
        rock(x + dx, z - 1);
        rock(x + dx, z - 4);
    }
    rock(x - 2, z - 3);
    rock(x + 1, z - 3);

    // Diagonal ramp with two lower approaches at its low corner.
    support(x + 4, z);
    cube(x + 4, groundY, z);
    support(x + 4, z + 1);
    cube(x + 4, groundY, z + 1);
    support(x + 5, z);
    cube(x + 5, groundY, z);
    ramp(x + 5, z + 1, TileShapeProfile::DiagonalRamp);
    rock(x + 4, z - 1);
    rock(x + 5, z - 1);
    rock(x + 3, z);
    rock(x + 3, z + 1);
    rock(x + 4, z + 2);
    rock(x + 5, z + 2);
    rock(x + 6, z);
    rock(x + 6, z + 1);

    // A ramp whose low toe opens over a floor one metre below. Snow deposited
    // on the face drains to that floor instead of coating the ramp.
    support(x, z + 6);
    cube(x, groundY, z + 6);
    layout.voxels[{x, groundY + 1, z + 6}] = OverrideVoxel(
        stoneId, TileShape::make(TileShapeProfile::Ramp, TileFacing::PosZ));
    const int floorY = groundY - 1;
    const int floorTop = chunkManager->generatedSurfaceY(x, z + 5);
    for (int y = floorTop + 1; y <= floorY; ++y)
        layout.voxels[{x, y, z + 5}] = OverrideVoxel(stoneId);
    cube(x, floorY, z + 5);
    rock(x - 1, z + 5);
    rock(x + 1, z + 5);
    rock(x, z + 4);

    snowFixtureSite = TilePos{x, groundY + 1, z};
    snowDropSite = TilePos{x, groundY + 1, z + 6};
    chunkManager->setGeneratedStructureKinds(
        applyOverrides({}, {{"snow_fixture", {{Key, std::move(layout)}}}}));
}

void GameplayState::rebuildSnow() {
    if (!snowCover || !snowSettlement || !snowTerrain || !snowId) return;
    snowCover->rebuild(*snowSettlement, *snowTerrain, snowId);
    chunkManager->setEnvironment(snowCover->environment());
}

void GameplayState::addSnow(const TileRaycastHit& hit) {
    auto outcome =
        snowSettlement->deposit(hit.tx, hit.tz, hit.ty + 1, snowAddVolume);
    if (outcome == snow::DepositOutcome::Pending) {
        snowSettlement->settle();
        outcome = snowSettlement->pendingCount()
                      ? snow::DepositOutcome::Pending
                      : snow::DepositOutcome::Settled;
    }
    rebuildSnow();
    lastBuildAction = fmt::format(
        fmt::runtime(_("Snow +{} cm3 at [{}, {}, {}]: {}")), snowAddVolume,
        hit.tx, hit.ty, hit.tz,
        outcome == snow::DepositOutcome::Settled ? _("settled")
        : outcome == snow::DepositOutcome::Sink  ? _("accounted sink")
                                                 : _("pending"));
}

GameplayState::~GameplayState() {
    // Chunk systems outlive the light renderer these listeners use.
    chunkManager->meshUploaded.disconnect(meshConnection);
    chunkManager->removed.disconnect(removedConnection);
    chunkManager->becameReady.disconnect(skyReadyConnection);
    // The editor is destroyed before the heightmap.
    skyMap->unwatch();
    try {
        // Capture pending edits while their chunks are still loaded.
        editTracker->update();
        chunkManager->commitWaterEdits();
        saveStructures(true);
        saveReservoirs();
        locationMgr->saveAll(true);
        regionStore->flush();
        saveTerrainEdits(true);
        saveLightSources(true);
        saveSky(true);
        writeMeta();
    } catch (const std::exception& e) {
        std::cerr << "Saving world failed: " << e.what() << '\n';
    }
}

void GameplayState::saveReservoirs() {
    if (!world->reservoirs ||
        savedWaterRevision == world->reservoirs->revision())
        return;
    try {
        save_catalog::writeReservoirs(slot, *world->reservoirs);
        savedWaterRevision = world->reservoirs->revision();
    } catch (const std::exception& e) {
        std::cerr << "Saving reservoirs failed: " << e.what() << '\n';
    }
}

void GameplayState::saveStructures(bool force) {
    if (!treeSim) return;
    const auto revision = chunkManager->structureOverrideRevision();
    if (!importLegacyStructures && !treeSim->dirty() &&
        revision == savedStructureOverrideRevision)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < nextStructureSave) return;
    try {
        save_catalog::writeStructures(slot, *chunkManager->treeOverrides(),
                                      chunkManager->structureOverrides(),
                                      structuresExtra);
        treeSim->clearDirty();
        savedStructureOverrideRevision = revision;
        importLegacyStructures = false;
        nextStructureSave = {};
    } catch (const std::exception& e) {
        nextStructureSave = now + std::chrono::seconds(1);
        std::cerr << "Saving structures.dat failed: " << e.what() << '\n';
    }
}

void GameplayState::updatePlantLabel() {
    if (!plantButton) return;
    const auto& species = chunkManager->treeSpecies().species();
    std::string name = "-";
    if (!species.empty()) {
        name = species[plantSpecies % species.size()].species.name;
        if (name.starts_with("trees.")) name = name.substr(6);
    }
    plantButton->setText("PLANT TREE: " + name);
}

void GameplayState::writeMeta() {
    slot.meta.lastPlayed = WorldMeta::now();
    slot.meta.camera = WorldMeta::CameraState{
        probe ? probe->feet() : camera.position, camera.yaw, camera.pitch,
        probe ? orbitDistance : camera.distance};
    slot.meta.worldDays = clock.days();
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

void GameplayState::saveSky(bool wait) {
    using namespace std::chrono_literals;
    if (skyWrite.valid()) {
        if (!wait && skyWrite.wait_for(0s) != std::future_status::ready)
            return;  // the previous write is still running
        try {
            skyWrite.get();
        } catch (const std::exception& e) {
            std::cerr << "Saving sky.dat failed: " << e.what() << '\n';
            skyMap->dirty = true;
        }
    }
    if (!skyMap->dirty) return;
    skyMap->dirty = false;
    auto write = [target = slot, overrides = skyMap->overrides(),
                  extra = skyExtra] {
        save_catalog::writeSky(target, overrides, extra);
    };
    if (!wait) {
        skyWrite = uiContext->threadPool.submit_task(std::move(write));
        return;
    }
    try {
        write();
    } catch (const std::exception& e) {
        std::cerr << "Saving sky.dat failed: " << e.what() << '\n';
        skyMap->dirty = true;
    }
}

void GameplayState::loadLightSources(const DataStorage<Block>& blocks) {
    try {
        auto file = save_catalog::readLightSources(slot);
        lightsExtra = std::move(file.extra);
        for (const auto& record : file.lights) {
            // Emission comes from today's definitions; lights whose block
            // no longer emits are dropped.
            if (record.block == 0 || record.block > blocks.items.size())
                continue;
            const auto& block = blocks.get(record.block);
            if (block.light)
                lightSources->setBlockLight(record.tile, record.block,
                                            *block.light, record.openFaces);
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
            records.push_back({s.tile, s.block, s.openFaces});
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
            saveReservoirs();
            saveTerrainEdits(false);
            saveLightSources(false);
            saveSky(false);
            writeMeta();
        }
        return;
    }
    autosaveTimer += dt;
    if (autosaveTimer < AutosaveInterval) return;
    autosaveTimer = 0;
    autosaveQueue = locationMgr->loaded();
    if (autosaveQueue.empty()) {
        saveReservoirs();
        saveTerrainEdits(false);
        saveLightSources(false);
        saveSky(false);
        writeMeta();
    }
}

void GameplayState::handleInput(Game& game, const InputEvent& e,
                                UIContext& context, float dt) {
    if (e.type == InputEvent::MouseMove || e.type == InputEvent::MouseClick)
        cursor = Vec2i{e.x, e.y};
    if (e.type == InputEvent::MouseClick &&
        e.button == InputEvent::RIGHT_BUTTON && !e.pressed)
        rotating = false;
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
                if (probe) break;
                if (e.direction == InputEvent::Vertical && e.ctrl) {
                    camera.position.y += e.delta;
                } else if (e.direction == InputEvent::Vertical) {
                    camera.distance -= e.delta;
                    camera.distance = bx::clamp(camera.distance, 10, 150);
                }
                break;
            case InputEvent::KeyHold: {
                Vec2 input{};
                if (e.key == SDL_SCANCODE_W) input.y = 1;
                if (e.key == SDL_SCANCODE_S) input.y = -1;
                if (e.key == SDL_SCANCODE_A) input.x = -1;
                if (e.key == SDL_SCANCODE_D) input.x = 1;
                if (probe) {
                    walkInput = walkInput + input;
                    walkFast = e.shift;
                } else {
                    const float speed = (e.shift ? 60.f : 20.f) * dt;
                    const float sin = std::sin(camera.yaw);
                    const float cos = std::cos(camera.yaw);
                    camera.position.x +=
                        (input.y * sin + input.x * cos) * speed;
                    camera.position.z +=
                        (input.y * cos - input.x * sin) * speed;
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
    lastBuildAction.clear();
    const Color armed{120, 200, 120, 255};
    const Color idle{232, 232, 232, 255};
    placeButton->getButton()->setNormalColor(tool == EditTool::Place ? armed
                                                                     : idle);
    removeButton->getButton()->setNormalColor(tool == EditTool::Remove ? armed
                                                                       : idle);
    lightButton->getButton()->setNormalColor(
        tool == EditTool::PlaceLight ? armed : idle);
    harvestButton->getButton()->setNormalColor(
        tool == EditTool::Harvest ? armed : idle);
    plantButton->getButton()->setNormalColor(
        tool == EditTool::PlantTree ? armed : idle);
    if (snow5Button) {
        const auto snowArmed = [&](int64_t volume) {
            return tool == EditTool::SnowAdd && snowAddVolume == volume;
        };
        snow5Button->getButton()->setNormalColor(snowArmed(51250) ? armed
                                                                  : idle);
        snow50Button->getButton()->setNormalColor(snowArmed(512500) ? armed
                                                                    : idle);
        snow5mButton->getButton()->setNormalColor(snowArmed(5125000) ? armed
                                                                     : idle);
        snowClearButton->getButton()->setNormalColor(
            tool == EditTool::SnowClear ? armed : idle);
    }
    updatePlantLabel();
    updateShapeControls();
}

TileShape GameplayState::placementShape() const {
    const auto rotation = TileFacing(facing);
    switch (shapeKind) {
        case ShapeKind::Cube:
        case ShapeKind::Layer:
            return {};
        case ShapeKind::Slab:
            return TileShape::make(Profile::Slab);
        case ShapeKind::Ramp:
        case ShapeKind::RampInfill: {
            const auto index = rampRun * (rampRun - 1) / 2 + rampSegment - 1;
            return TileShape::make(RampProfiles[rampForm][index], rotation);
        }
        case ShapeKind::Wall:
            return TileShape::make(
                Profile(unsigned(Profile::Wall10) + thickness), rotation);
    }
    return {};
}

void GameplayState::updateShapeControls() {
    const bool placing = tool == EditTool::Place;
    const bool infill = placing && shapeKind == ShapeKind::RampInfill;
    const bool ramp = infill || (placing && shapeKind == ShapeKind::Ramp);
    const bool layer = placing && shapeKind == ShapeKind::Layer;
    const bool wall = placing && shapeKind == ShapeKind::Wall;
    shapeButton->setVisible(placing);
    facingButton->setVisible(ramp || wall);
    thicknessButton->setVisible(wall || layer || infill);
    materialButton->setVisible(placing);
    targetButton->setVisible(placing);
    elevationButton->setVisible(placing);
    materialButton->setText(timber ? _("Material: Oak timber")
                                   : _("Material: Stone"));
    targetButton->setText(sameTile ? _("Target: This tile")
                                   : _("Target: Next tile"));
    elevationButton->setText(
        fmt::format(fmt::runtime(_("Height: {} cm")), elevation * 5));
    formButton->setVisible(ramp);
    runButton->setVisible(ramp);
    segmentButton->setVisible(ramp);
    if (!placing) return;

    shapeButton->setText(fmt::format(fmt::runtime(_("Shape: {}")),
                                     _(ShapeNames[unsigned(shapeKind)])));
    facingButton->setText(
        fmt::format(fmt::runtime(_("Rotate: {}")), FacingNames[facing]));
    thicknessButton->setText(fmt::format(
        fmt::runtime(infill ? _("Fill to: {} cm") : _("Thickness: {} cm")),
        infill  ? int(infillHeight * 5)
        : layer ? int(LayerSizes[layerThickness])
                : WallThickness[thickness]));
    formButton->setText(
        fmt::format(fmt::runtime(_("Form: {}")), _(RampForms[rampForm])));
    runButton->setText(fmt::format(fmt::runtime(_("Run: {} m")), rampRun));
    segmentButton->setText(
        fmt::format(fmt::runtime(_("Piece: {0}/{1}")), rampSegment, rampRun));
}

void GameplayState::toggleWalk() {
    walkInput = {};
    walkFast = false;
    if (probe) {
        camera.position = probe->feet();
        probe.reset();
        camera.distance = orbitDistance;
        camera.nearPlane = 1.f;
        walkButton->setText(_("Walk mode"));
    } else {
        // Look below the orbit target. Allow for the footprint extending
        // uphill beyond the center of a ramp.
        probe = GroundedProbe::spawn(*chunkManager,
                                     camera.position + Vec3{0, .35f, 0});
        if (!probe) {
            walkButton->setText(_("No walkable ground"));
            return;
        }
        orbitDistance = camera.distance;
        camera.distance = 0;
        camera.nearPlane = .08f;
        camera.pitch = std::clamp(camera.pitch, -.45f, .45f);
        camera.position = probe->feet() + Vec3{0, GroundedProbe::EyeHeight, 0};
        walkButton->setText(_("Orbit camera"));
    }
}

void GameplayState::updateWalk(float dt) {
    const float speed = walkFast ? 7.f : 3.5f;
    const float scale =
        speed / std::max(1.f, std::hypot(walkInput.x, walkInput.y));
    const float sin = std::sin(camera.yaw);
    const float cos = std::cos(camera.yaw);
    probe->update(*chunkManager,
                  {scale * (walkInput.y * sin + walkInput.x * cos),
                   scale * (walkInput.y * cos - walkInput.x * sin)},
                  dt);
    walkInput = {};
    walkFast = false;
    camera.position = probe->feet() + Vec3{0, GroundedProbe::EyeHeight, 0};
}

std::optional<TileRaycastHit> GameplayState::pickTile(int mouseX, int mouseY,
                                                      bool liquids) {
    // Same view and 60 degree vertical field of view as render().
    const Vec2 viewport = uiContext->getWindowSize();
    if (viewport.x <= 0 || viewport.y <= 0) return std::nullopt;
    const Vec3 eye = camera.eye();
    const auto view = camera.direction();
    const bx::Vec3 forward{view.x, view.y, view.z};
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
                       camera.distance + 64.f, liquids);
}

std::optional<GameplayState::PiecePlacement> GameplayState::placementAt(
    const TileRaycastHit& hit) {
    TilePos pos{hit.tx, hit.ty, hit.tz};
    if (!sameTile) {
        if (hit.placementOffset == TilePos{}) return std::nullopt;
        pos.x += hit.placementOffset.x;
        pos.y += hit.placementOffset.y;
        pos.z += hit.placementOffset.z;
    }
    pos.x = int(chunkManager->wrap().wrapTile(pos.x));
    const auto cell = chunkManager->solidCellAt(pos);
    const auto id = [&]() -> tile_geometry::PieceId {
        if (!cell) return 0;
        auto candidate = tile_geometry::PieceId(0);
        for (const auto& entry : cell->cell.pieces()) {
            if (entry.geometry.id != candidate) break;
            ++candidate;
        }
        return candidate;
    }();
    const auto material = timber ? timberId : stoneId;
    const Vec3 offset{0, float(elevation * 5) / 100, 0};
    const bool infill = shapeKind == ShapeKind::RampInfill;
    const auto fill = infill ? tile_geometry::PieceFill::AboveProfile
                             : tile_geometry::PieceFill::BelowProfile;
    const float ceiling = infill ? float(infillHeight * 5) / 100 : 1.f;
    const auto piece =
        shapeKind == ShapeKind::Layer
            ? tile_geometry::TilePiece::layer(
                  id, material, LayerSizes[layerThickness], offset)
            : tile_geometry::TilePiece{id,     material, placementShape(),
                                       offset, fill,     ceiling};
    // Retired generated IDs are tracked with their owner, so a player piece
    // can reuse an otherwise unoccupied numeric ID.
    return PiecePlacement{
        pos, piece, cell ? cell->revision : SolidCellRevision{},
        cell ? editor->checkPiecePlacement(pos, cell->cell, piece)
             : CellPieceEdit::NotLoaded};
}

void GameplayState::renderPlacementPreview(RenderTarget& target,
                                           IRenderer& renderer) {
    if (!placementPreview) return;
    auto piece = placementPreview->piece;
    piece.id = 0;  // The preview buffer depends on geometry alone.
    piece.blockId = 1;
    if (previewShape != piece || !previewMesh.drawable()) {
        tile_geometry::CellGeometry geometry;
        if (geometry.tryAdd(piece) != tile_geometry::PiecePlacement::Added)
            return;
        std::vector<PosVertex> vertices;
        std::vector<uint32_t> indices;
        for (const auto& face : geometry.exteriorFaces()) {
            const auto base = uint32_t(vertices.size());
            for (auto point : face.vertices) {
                // Lift the preview just above coincident surfaces.
                point = point + face.plane.normal * .003f;
                vertices.push_back({point.x, point.y, point.z});
            }
            for (uint32_t i = 1; i + 1 < face.vertices.size(); ++i)
                indices.insert(indices.end(), {base, base + i, base + i + 1});
        }
        if (!previewMesh.drawable())
            previewMesh.initDynamic<PosVertex>(&renderer, 64, 96);
        previewMesh.update(vertices.data(), vertices.size(), indices.data(),
                           indices.size(), true);
        previewShape = piece;
    }

    const auto [x, y, z] = placementPreview->pos;
    const auto eye = camera.eye();
    const bool allowed = placementPreview->status == CellPieceEdit::Applied;
    previewColor.setVec4(allowed ? std::array{.06f, .36f, .20f, 1.f}
                                 : std::array{.60f, .08f, .05f, 1.f});
    static const std::string Shader = "color";
    target.drawVertexBuffer(
        previewMesh.get(), &Shader, {&previewColor}, false,
        relativeTransform(camera, world->wrap().imageX(x, eye.x), y, z),
        DrawLayer::GAME_GLOW);
}

void GameplayState::useTool(int mouseX, int mouseY) {
    const auto hit = pickTile(mouseX, mouseY);
    if (!hit) return;
    if (tool == EditTool::SnowAdd) {
        addSnow(*hit);
        return;
    }
    if (tool == EditTool::SnowClear) {
        snowSettlement->clear();
        rebuildSnow();
        lastBuildAction = _("Snow cleared");
        return;
    }
    if (tool == EditTool::Harvest) {
        // Plants standing on (or growing into) the tile under the cursor.
        // Items do not exist yet: the yield is shown in the debug panel.
        const auto yields =
            floraSim->harvest({hit->tx, hit->ty, hit->tz}, clock.days());
        lastHarvest.clear();
        for (const auto& y : yields)
            lastHarvest += std::to_string(y.count) + " " + y.item + "\n";
        if (lastHarvest.empty()) lastHarvest = "nothing to harvest\n";
        return;
    }
    if (tool == EditTool::Remove) {
        const auto result = editor->removePiece({hit->tx, hit->ty, hit->tz},
                                                hit->revision, hit->pieceId);
        lastBuildAction = result == CellPieceEdit::Applied
                              ? _("Piece removed")
                              : pieceEditLabel(result);
        return;
    }
    if (tool == EditTool::PlantTree) {
        const auto& species = chunkManager->treeSpecies().species();
        if (species.empty()) return;
        const auto& rule = species[plantSpecies % species.size()];
        lastTreeAction =
            treeSim->plant({hit->tx, hit->ty, hit->tz}, rule.id, clock.days())
                ? "planted " + rule.species.name
                : "cannot plant here (solid ground, open air above)";
        return;
    }
    if (tool == EditTool::Place) {
        if (const auto placement = placementAt(*hit)) {
            const auto result =
                placement->status == CellPieceEdit::Applied
                    ? editor->placePiece(placement->pos, placement->revision,
                                         placement->piece)
                    : placement->status;
            lastBuildAction = result == CellPieceEdit::Applied
                                  ? _("Piece placed")
                                  : pieceEditLabel(result);
        }
        return;
    }
    if (testLightId && hit->placementOffset != TilePos{}) {
        const auto [dx, dy, dz] = hit->placementOffset;
        editor->placeBlock({hit->tx + dx, hit->ty + dy, hit->tz + dz},
                           *testLightId);
    }
}

void GameplayState::update(Game& game, float dt) {
    // Before any region can unload this frame.
    editTracker->update();
    chunkManager->commitWaterEdits();
    if (probe) updateWalk(dt);
    // X wraps around the world (stored canonically in [0, W)); Z is bounded.
    camera.position.x = float(world->wrap().wrapX(camera.position.x));
    if (camera.position.x >= float(world->wrap().tiles))
        camera.position.x = 0;  // float rounding of a value just below W
    if (!probe)
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
    std::vector<structure_voxels::SourcePtr> generatedLandmarks;
    chunkManager->appendStructureLandmarks(generatedLandmarks, camera.eye(),
                                           camera.viewDistance);
    auto landmarks = treeRenderer->landmarkVoxels(clock.days());
    for (auto& source : landmarks)
        source = edits->maskStructure(source, chunkManager->wrap(), true);
    for (auto& source : generatedLandmarks)
        source = edits->maskStructure(source, chunkManager->wrap());
    landmarks.insert(landmarks.end(), generatedLandmarks.begin(),
                     generatedLandmarks.end());
    terrainLod->setStructures(std::move(landmarks));
    terrainLod->update(camera, uiContext->getWindowSize(),
                       lightRenderer->shadows().casterDetail());
    chunkManager->setFocus(camera.position);
    sunLight->setFocus(camera.position);
    blockLight->setFocus(camera.position);
    requestChunksAroundCamera();
    chunkManager->update(editor.get());
    skyMap->setFocus(camera.position);
    skyMap->update();
    auto skyLandmarks =
        treeRenderer->landmarkVoxels(chunkManager->woodDay(), false);
    for (auto& source : skyLandmarks)
        source = edits->maskStructure(source, chunkManager->wrap(), true);
    skyLandmarks.insert(skyLandmarks.end(), generatedLandmarks.begin(),
                        generatedLandmarks.end());
    skyMap->updateOcclusion(camera.eye(), std::move(skyLandmarks), *world,
                            *uiContext->assets);
    sunLight->update();
    blockLight->update();
    clock.advance(dt);
    chunkManager->advanceWater(uint64_t(clock.days() * 86400));
    floraSim->update(clock.days());
    treeSim->update(clock.days());
    saveStructures();
    if (timeButton) {
        const auto date = world_calendar::fromDays(clock.days());
        const double t = clock.timeOfDay() * 24;
        char label[80];
        std::snprintf(label, sizeof(label), "D%d/M%d/Y%lld %02d:%02d x%g",
                      date.day, date.month, static_cast<long long>(date.year),
                      int(t), int((t - int(t)) * 60), clock.speed());
        timeButton->setText(label);
    }

    // --- Tile debug raycast -------------------------------------------------
    if (tileDebugText) {
        const auto hit =
            cursor ? pickTile(cursor->x, cursor->y, tool == EditTool::None)
                   : std::nullopt;
        std::string controls;
        if (probe)
            controls += probe->grounded() ? _("Walking: WASD, Shift to run")
                                          : _("Falling");
        if (!controls.empty()) controls += '\n';
        if (constructionSite) {
            const auto [x, y, z] = *constructionSite;
            controls +=
                fmt::format("Construction deck [{}, {}, {}]\n", x, y, z);
        }
        if (snowFixtureSite) {
            const auto [x, y, z] = *snowFixtureSite;
            controls +=
                fmt::format("Snow fixture landing [{}, {}, {}]\n", x, y, z);
        }
        if (snowDropSite) {
            const auto [x, y, z] = *snowDropSite;
            controls += fmt::format("Snow drop ramp [{}, {}, {}]\n", x, y, z);
        }
        if (constructionSite || snowFixtureSite || snowDropSite) {
            const auto& at = camera.position;
            controls += fmt::format("Camera [{:.1f}, {:.1f}, {:.1f}]\n", at.x,
                                    at.y, at.z);
        }
        placementPreview =
            tool == EditTool::Place && hit ? placementAt(*hit) : std::nullopt;
        if (placementPreview) {
            const auto [x, y, z] = placementPreview->pos;
            controls += fmt::format("[{}, {}, {}] {}\n", x, y, z,
                                    pieceEditLabel(placementPreview->status));
        }
        if (!lastBuildAction.empty()) controls += lastBuildAction + "\n";
        // Voxel streaming counters: a stuck queue shows here first.
        const std::string streaming =
            "Chunks: " + std::to_string(chunkManager->pendingGeneration()) +
            " queued, " + std::to_string(chunkManager->generationInFlight()) +
            " generating, " + std::to_string(chunkManager->meshInFlight()) +
            " meshing\nTree sets: " +
            std::to_string(chunkManager->treeJobsInFlight()) + " building, " +
            std::to_string(chunkManager->pendingTreeSets()) + " waiting\n";
        if (!hit) {
            const auto date = world_calendar::fromDays(clock.days());
            const auto climate =
                world_climate::at(*world, camera.position.x, camera.position.z,
                                  clock.days(), camera.position.y);
            const std::string climateLine = fmt::format(
                "D{}/M{}/Y{} climate {:.1f} C (mean {:.1f}, season {:+.1f}, "
                "daily {:+.1f}, elevation {:+.1f})\n",
                date.day, date.month, date.year, climate.temperatureC,
                climate.meanC, climate.seasonalC, climate.dailyC,
                climate.elevationC);
            tileDebugText->setText(
                controls + climateLine + _("No tile") + "\n" + streaming +
                "LOD: " + std::to_string(terrainLod->patchCount()) +
                " patches, " +
                std::to_string(terrainLod->residentBytes() / 1024) + " KiB\n" +
                std::to_string(terrainLod->jobCount()) + " LOD jobs, " +
                std::to_string(terrainLod->terrainBacklog()) +
                " terrain pending, " +
                std::to_string(terrainLod->structureBacklog()) +
                " structure pending\n" + std::to_string(edits->size()) +
                " edited columns, " +
                std::to_string(terrainLod->structureCount()) +
                " structure slabs");
        } else {
            const Tile& t = hit->tile;
            std::stringstream ss;

            ss << controls << "Tile [" << hit->tx << ", " << hit->ty << ", "
               << hit->tz << "]\n";
            ss << streaming;

            ss << "\n--- Block ---\n";
            ss << "block_id:  " << (int)t.block_id << "\n";
            if (t.isSolid()) {
                const auto& block =
                    uiContext->assets->getStorage<Block>().get(t.block_id);
                ss << block.name << " · " << hit->pieceId << "\n";
                ss << fmt::format(fmt::runtime(_("Picked shape: {}\n")),
                                  describeShape(hit->shape));
            }

            ss << "\n--- Snow ---\n";
            {
                const auto volume = snowSettlement->volumeAt(hit->tx, hit->tz);
                const auto level = snowSettlement->levelAt(hit->tx, hit->tz);
                ss << "volume: " << volume << " cm3";
                if (level) {
                    const auto key = std::pair{
                        int(chunkManager->wrap().wrapTile(hit->tx)), hit->tz};
                    const auto found = snowSettlement->columns().find(key);
                    const double base = found == snowSettlement->columns().end()
                                            ? 0
                                            : double(found->second.baseY) +
                                                  found->second.baseLocal;
                    ss << fmt::format(", level {:.3f} m, depth {:.3f} m",
                                      *level, *level - base);
                }
                ss << "\n";
            }

            ss << "\n--- Climate ---\n";
            {
                const auto date = world_calendar::fromDays(clock.days());
                const auto climate = world_climate::at(
                    *world, double(hit->tx) + .5, double(hit->tz) + .5,
                    clock.days(), double(hit->ty) + .5);
                ss << fmt::format("date: D{} M{} Y{}\n", date.day, date.month,
                                  date.year);
                ss << fmt::format(
                    "mean {:.1f} C, seasonal {:+.1f}, daily {:+.1f}, "
                    "elevation {:+.1f}\n",
                    climate.meanC, climate.seasonalC, climate.dailyC,
                    climate.elevationC);
                ss << fmt::format("local {:.1f} C\n", climate.temperatureC);
            }

            ss << "\n--- Liquid ---\n";
            ss << "liquid:      " << (int)t.liquid << "\n";
            ss << "liquidLevel: "
               << int(terrainLod->surfaceFill({hit->tx, hit->ty, hit->tz}, t))
               << "\n";

            ss << "\n--- Light ---\n";
            ss << "sun_exposure:     " << (int)t.lightLevel.sun_exposure
               << "\n";
            ss << "artificial_light: " << (int)t.lightLevel.artificial_light
               << "\n";
            // Sky heightmap at the air tile in front of the face.
            {
                const auto [dx, dy, dz] = hit->placementOffset;
                const int64_t ax = hit->tx + dx, az = hit->tz + dz;
                const int ay = hit->ty + dy;
                const auto top = skyMap->top(ax, az);
                const auto direct = skyMap->directSky(ax, ay, az);
                ss << "sky top: "
                   << (top ? (*top == sky::None ? std::string("none")
                                                : std::to_string(*top))
                           : std::string("-"))
                   << ", direct: "
                   << (direct ? std::to_string(int(*direct)) : std::string("-"))
                   << "\n";
            }
            if (const auto* e = blockLight->emission(t.block_id))
                ss << "emits: radius " << e->radius << ", falloff "
                   << e->falloff << "\n";
            ss << "far lights: " << lightRenderer->lightCount() << " lit, "
               << lightRenderer->shadowedCount() << " shadowed\n";
            ss << "glare: " << lightRenderer->glareCount()
               << ", distant glow: " << lightRenderer->glowLights() << "\n";

            if (t.cover)
                ss << "cover: " << int(t.cover) << " density "
                   << int(t.coverDensity()) << "\n";
            static constexpr const char* StageNames[] = {
                "seedling",  "young",    "mature",
                "flowering", "fruiting", "withering"};
            for (const auto& info : floraSim->plantsAt(
                     {hit->tx, hit->ty, hit->tz}, clock.days())) {
                ss << "\n" << info.species->name << ": ";
                if (!info.state.alive)
                    ss << "seed";
                else
                    ss << StageNames[int(info.state.stage)] << " "
                       << int(info.state.progress * 100) << "%";
                ss << ", age " << int(clock.days() - info.plant.birthDay)
                   << " d";
                if (info.plant.flags & FloraInstance::Harvested)
                    ss << ", harvested";
            }
            if (!lastHarvest.empty()) ss << "\n\nHarvest:\n" << lastHarvest;
            if (const auto tree = treeSim->describe({hit->tx, hit->ty, hit->tz},
                                                    clock.days());
                !tree.empty())
                ss << "\n\nTree: " << tree;
            if (!lastTreeAction.empty()) ss << "\n" << lastTreeAction;

            tileDebugText->setText(ss.str());
        }
    }
}

void GameplayState::render(RenderTarget& target, UIContext& context) {
    // Poll mesh futures and upload any that finished this frame.
    chunkManager->uploadReadyMeshes(*context.renderer);
    // After uploads: deferred light volumes switch with their new meshes.
    lightVolumes->update(camera.position);
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    camera.sky = skyAtTime(clock.timeOfDay(), world->genSettings.seed);
    lightRenderer->setSky(camera.sky);
    lightRenderer->update(*lightSources, camera, world->wrap(), now);
    lightRenderer->setSkyOcclusion(skyMap->skyOcclusion());
    terrainLod->render(target, *context.renderer, *chunkManager, camera,
                       context.getWindowSize(), lightVolumes.get());
    camera.underwater = terrainLod->eyeUnderwater();
    Camera displayedCamera = camera;
    displayedCamera.viewDistance = terrainLod->visibleDistance();
    context.renderer->setCamera(displayedCamera);
    auto& water = terrainLod->waterUniform();
    if (terrainLod->drawnVersion() != shadowLodVersion) {
        shadowLodVersion = terrainLod->drawnVersion();
        lightRenderer->shadows().lodChanged();
    }
    renderShadowCasters(target, now);
    const Vec3 eye = camera.eye();
    worldEye.setVec4({eye.x, eye.y, eye.z, displayedCamera.viewDistance});

    float vp[16], view[16], proj[16];
    const auto look = eye + camera.direction();
    bx::mtxLookAt(view, {eye.x, eye.y, eye.z}, {look.x, look.y, look.z},
                  {0, 1, 0});
    const auto viewport = context.getWindowSize();
    bx::mtxProj(proj, 60.f, viewport.x / std::max(1.f, viewport.y),
                camera.nearPlane, displayedCamera.viewDistance + 512.f,
                bgfx::getCaps()->homogeneousDepth);
    bx::mtxMul(vp, view, proj);

    Frustum frustum = Frustum::fromViewProj(vp);

    int camCX = int(camera.position.x) / Chunk::SIZE_X;
    const WorldWrap wrap = world->wrap();

    chunkManager->forEachMeshed([&](const ChunkCoord& coord) {
        // Chunks are drawn at their copy nearest the camera, so the X seam
        // is invisible.
        auto* ground = terrainLod->drawsChunk(coord)
                           ? chunkManager->getMesh(coord)
                           : nullptr;
        auto* waterMesh = terrainLod->drawsWater(coord)
                              ? chunkManager->getWaterMesh(coord)
                              : nullptr;
        if (!ground && !waterMesh) return;
        const int64_t deltaX = wrap.deltaChunk(coord.x, camCX);

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

        const float waterOffset =
            waterMesh ? terrainLod->lakeMeshOffset(
                            chunkManager->waterMeshSurface(coord))
                      : 0;
        aabb.minY += std::min(0.f, waterOffset);
        aabb.maxY += std::max(0.f, waterOffset);
        if (!frustum.intersects(aabb)) return;

        // Chunk meshes are chunk-local; place them relative to the eye.
        auto transform = relativeTransform(camera, wx, wy, wz);
        std::vector<ShaderProperty*> uniforms{&u_texture_array, &u_normal_array,
                                              &worldEye, &water};
        lightRenderer->appendUniforms(uniforms, LightRenderer::Program::Block);
        // Without a light volume the chunk would render unlit; skip it.
        if (!lightVolumes->bind(coord, uniforms, wx)) return;
        if (ground)
            target.drawVertexBuffer(ground->get(), &blockShader,
                                    std::vector(uniforms), false,
                                    std::array(transform), DrawLayer::GAME);
        if (waterMesh)
            target.drawVertexBuffer(
                waterMesh->get(), &blockShader, std::move(uniforms), false,
                relativeTransform(camera, wx, wy + waterOffset, wz),
                DrawLayer::GAME);
    });
    floraRenderer->render(target, displayedCamera, frustum, *lightRenderer, now,
                          clock.days(), water);
    treeRenderer->render(target, displayedCamera, frustum, *lightRenderer, now,
                         clock.days(), water);
    lightRenderer->drawGlare(target, displayedCamera, viewport);
    renderPlacementPreview(target, *context.renderer);

    ui_manager->render(target, context);
}

void GameplayState::renderShadowCasters(RenderTarget& target, double now) {
    const WorldWrap wrap = world->wrap();
    lightRenderer->renderShadows(
        target,
        [&](RenderTarget& t, const LightShadows::Face& face) {
            const auto& l = face.light;
            // The detailed chunks drawn this frame (render()'s selection),
            // placed at their copy nearest the light.
            chunkManager->forEachMeshed([&](const ChunkCoord& coord) {
                if (!terrainLod->drawsChunk(coord)) return;
                const double ox = double(coord.x) * Chunk::SIZE_X;
                const double wx = l[0] + wrap.deltaX(ox + 8, l[0]) - 8;
                const double lx = wx - l[0];
                const double ly = double(coord.y) * Chunk::SIZE_Y - l[1];
                const double lz = double(coord.z) * Chunk::SIZE_Z - l[2];
                if (!LightShadowMath::reaches(lx, ly, lz, lx + Chunk::SIZE_X,
                                              ly + Chunk::SIZE_Y,
                                              lz + Chunk::SIZE_Z, face.radius))
                    return;
                if (!face.frustum.intersects(AABB{
                        float(lx), float(ly), float(lz),
                        float(lx + Chunk::SIZE_X), float(ly + Chunk::SIZE_Y),
                        float(lz + Chunk::SIZE_Z)}))
                    return;
                MeshHandle* mesh = chunkManager->getMesh(coord);
                if (!mesh) return;
                t.drawShadowCaster(
                    mesh->get(), face.shader, std::vector(face.chunkUniforms),
                    false, LightShadowMath::translation(lx, ly, lz), face.view);
            });
            terrainLod->submitShadowCasters(t, face, *chunkManager);
        },
        now);
}

void GameplayState::requestChunksAroundCamera() {
    // Convert camera world-tile position to chunk coordinates.
    int camCX = int(camera.position.x) / Chunk::SIZE_X;
    int camCY = int(camera.position.y) / Chunk::SIZE_Y;
    int camCZ = int(camera.position.z) / Chunk::SIZE_Z;

    const int radius = terrainLod->settings().chunkRadius;

    // Chunks still queued from earlier camera positions leave the queue:
    // fast travel would otherwise pile up requests nobody needs.
    const ChunkCoord centre{unsigned(world->wrap().wrapChunk(camCX)),
                            unsigned(std::max(0, camCY)),
                            unsigned(std::max(0, camCZ))};
    if (requestCentre != centre) {
        requestCentre = centre;
        chunkManager->withdrawOutside(centre, radius);
    }

    // Iterate by distance from camera
    for (int d = 0; d <= radius; ++d) {
        for (int dz = -d; dz <= d; ++dz) {
            for (int dx = -d; dx <= d; ++dx) {
                for (int dy = -d; dy <= d; ++dy) {
                    // Skip if not on the outer shell of current distance
                    if (std::abs(dx) != d && std::abs(dy) != d &&
                        std::abs(dz) != d)
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
