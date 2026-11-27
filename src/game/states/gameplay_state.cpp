#include "gameplay_state.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "gettext_wrapper.hpp"
#include "main_menu_state.hpp"
#include "render/frustrum.hpp"
#include "world/day_night.hpp"
#include "world/generated_hydrology.hpp"
#include "world/polar_snow.hpp"
#include "world/precipitation.hpp"
#include "world/world_calendar.hpp"
#include "world/world_climate.hpp"
#include "world/world_weather.hpp"

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

}  // namespace

// Settlement reads the chunk's own solids, never the environmental snow
// projection that is derived from it.
class ChunkSnowTerrain final : public snow::SnowTerrain {
   public:
    ChunkSnowTerrain(ChunkManager& chunks, const World& world,
                     const SkyHeightMap& sky, const TerrainEdits& edits,
                     std::optional<TilePos> fixture)
        : chunks(chunks),
          world(world),
          sky(sky),
          edits(edits),
          polar(world),
          fixture(fixture) {}

    void resetQueries() {
        slabs.clear();
        generation.reset();
        generationKey = UINT64_MAX;
    }

    std::optional<tile_geometry::CellGeometry> cell(
        TilePos pos) const override {
        if (pos.y > boundsAt(pos.x, pos.z))
            return tile_geometry::CellGeometry{};
        return chunks.rawCellGeometryAt(pos);
    }
    int top(int x, int z) const override { return boundsAt(x, z); }
    int wrapX(int x) const override { return int(chunks.wrap().wrapTile(x)); }
    bool inBounds(int x, int z) const override {
        if (world.width <= 0 || world.height <= 0) return x >= 0 && z >= 0;
        return x >= 0 && x < chunks.wrap().tiles && z >= 0 &&
               z < world.height * CHUNKS_PER_REGION * Chunk::SIZE_Z;
    }
    int minY() const override { return 0; }
    int maxY() const override { return WORLD_HEIGHT_Y; }
    void setSeason(int day) { season = day; }
    bool mayHaveInitialPack(int x, int z) const override {
        // Chunk projection tests both edge rows. The fixture is positioned
        // on a chunk's first row in Z.
        return inFixture(x, z) || polar.depth(x, z, season) > 0.;
    }
    std::optional<snow::PackRun> initialPack(int x, int z) const override {
        if (fixture)
            if (const auto pack =
                    snow::polar::fixturePack(world, *fixture, x, z))
                return pack;
        if (!snow::polar::supported(world)) return std::nullopt;
        const auto* ground = generationAt(x, z);
        return ground ? polar.at(x, z, *ground, season) : std::nullopt;
    }
    snow::Volume initialPackCapacity(int x, int z) const override {
        if (inFixture(x, z)) return snow::MaximumVolume;
        const auto* ground = generationAt(x, z);
        return ground ? snow::polar::capacity(*ground) : snow::MaximumVolume;
    }

   private:
    const ColumnGenerationInput* generationAt(int x, int z) const {
        const unsigned cx = unsigned(x / Chunk::SIZE_X);
        const unsigned cz = unsigned(z / Chunk::SIZE_Z);
        const auto key = editKey(cx, cz);
        if (generationKey != key) {
            generation = chunks.generatedColumns(cx, cz);
            generationKey = key;
        }
        if (!generation) return nullptr;
        return &generation->columns[(z % Chunk::SIZE_Z) * Chunk::SIZE_X +
                                    x % Chunk::SIZE_X];
    }

    bool inFixture(int x, int z) const {
        if (!fixture || z < fixture->z || z >= fixture->z + 6) return false;
        const auto dx = chunks.wrap().deltaTile(x, fixture->x);
        return dx >= -3 && dx < 3;
    }

    struct Slabs {
        std::array<const Chunk*, Y_CHUNKS> ready{};
        // Zero is uncached, and each cached bound is stored as y + 2.
        std::array<uint16_t, sky::Tile::Size> tops{};
        std::shared_ptr<const ColumnEdit> edit;
    };
    int boundsAt(int x, int z) const {
        const unsigned cx = unsigned(x / Chunk::SIZE_X);
        const unsigned cz = unsigned(z / Chunk::SIZE_Z);
        const int lx = x % Chunk::SIZE_X, lz = z % Chunk::SIZE_Z;
        const auto key = editKey(cx, cz);
        auto [slab, newSlab] = slabs.try_emplace(key);
        if (newSlab) {
            slab->second.edit = edits.find(key);
            for (unsigned cy = 0; cy < Y_CHUNKS; ++cy)
                slab->second.ready[cy] = chunks.getChunk({cx, cy, cz});
        }
        auto& [ready, tops, edit] = slab->second;
        auto& cached = tops[sky::Tile::index(lx, lz)];
        if (cached) return int(cached) - 2;
        const auto known = sky.rawTop(x, z);
        if (!known) {
            cached = WORLD_HEIGHT_Y + 1;
            return WORLD_HEIGHT_Y - 1;
        }

        int top = std::max(-1, *known);
        // Inspect Ready slabs above the sky bound for zero-opacity solids
        // and edits that arrived after the latest sky build.
        for (int cy = Y_CHUNKS - 1; cy >= 0; --cy) {
            const int high = (cy + 1) * Chunk::SIZE_Y - 1;
            if (high <= top) break;
            const auto* chunk = ready[cy];
            if (!chunk) {
                if (edit && edit->slab(uint16_t(cy))) {
                    top = high;
                    break;
                }
                continue;
            }
            for (int y = high; y > top && y / Chunk::SIZE_Y == cy; --y) {
                if (!chunk->tiles[lz][y % Chunk::SIZE_Y][lx].isSolid())
                    continue;
                top = y;
                break;
            }
        }
        cached = uint16_t(top + 2);
        return top;
    }

    ChunkManager& chunks;
    const World& world;
    const SkyHeightMap& sky;
    const TerrainEdits& edits;
    snow::polar::Baselines polar;
    int season = 0;
    std::optional<TilePos> fixture;
    mutable std::map<uint64_t, Slabs> slabs;
    mutable uint64_t generationKey = UINT64_MAX;
    mutable std::shared_ptr<const ChunkColumnInputs> generation;
};

namespace {

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
    snowSoft.initVec4("u_material_soft", {-1.f, 0.f, 0.f, 0.f});
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
    weatherRenderer = std::make_unique<WeatherRenderer>(*context.renderer);
    {
        std::vector<ShaderProperty*> patchLights, blockLights;
        lightRenderer->appendUniforms(patchLights, LightRenderer::Program::Lod);
        lightRenderer->appendUniforms(blockLights,
                                      LightRenderer::Program::Block);
        patchLights.push_back(&snowSoft);
        blockLights.push_back(&snowSoft);
        terrainLod->setExtraUniforms(std::move(patchLights),
                                     std::move(blockLights));
    }
    lightSources = std::make_unique<LightSourceRegistry>();
    loadLightSources(context.assets->getStorage<Block>());
    thermalSources = std::make_unique<ThermalSourceRegistry>();
    {
        const auto& items = context.assets->getStorage<ThermalItem>();
        for (const auto& item : items.items) {
            if (item.name == "items.heater") heaterItemId = item.id;
            if (item.name == "items.cooler") coolerItemId = item.id;
        }
        loadThermalSources(items);
    }
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
        heaterBlockId = id("blocks.heater", 0);
        coolerBlockId = id("blocks.cooler", 0);
        thermostatBlockId = id("blocks.thermostat", 0);
        if (snowId) {
            const auto layer =
                context.assets->getMaterialAlbedoLayer("materials.snow");
            snowSoft.setVec4({float(layer), .85f, 0.f, 0.f});
        } else {
            std::cerr
                << "blocks.snow is not defined; snow rendering is disabled\n";
        }
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
    snowClearButton = makeDebugButton({18, 11, 6, 1}, [this] { clearSnow(); });
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
    setupConstructionFixture();
    setupSnowFixture();
    setupThermalFixture();
    setupWeatherTravelFixture();
    setupPolarPackFixture();
    if (polarPackSite)
        makeDebugButton({6, 7, 6, 1}, [this] {
            if (probe) toggleWalk();
            const auto [x, y, z] = *polarPackSite;
            camera.position = {x + .5f, y + 6.f, z + 2.5f};
            camera.yaw = 0;
            camera.pitch = -.25f;
            camera.distance = 15.f;
        })->setText(_("Polar pack"));
    if (const char* fixture = std::getenv("ANYM_WEATHER_DISTANCE_FIXTURE");
        fixture && std::string_view(fixture) == "1" && this->world->width > 1 &&
        this->world->height > 1) {
        distantWeatherFixture = weather_presentation::Fixture{
            std::max(1, int(slot.meta.startRegion.x)),
            std::min(this->world->height - 2, int(slot.meta.startRegion.y))};
        makeDebugButton({0, 7, 6, 1}, [this] {
            visitDistantWeatherSite(false, false);
        })->setText(_("Weather ridge"));
        makeDebugButton({6, 7, 6, 1}, [this] {
            visitDistantWeatherSite(false, true);
        })->setText(_("Weather boundary"));
        makeDebugButton({12, 7, 6, 1}, [this] {
            visitDistantWeatherSite(true, false);
        })->setText(_("Weather seam"));
        makeDebugButton({18, 7, 6, 1}, [this] {
            visitDistantWeatherSite(true, true);
        })->setText(_("Weather seam edge"));
    }
    if (constructionSite) {
        auto* button = makeDebugButton({18, 13, 6, 1}, [this] {
            advanceSnow();
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
            advanceSnow();
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
            advanceSnow();
            if (probe) toggleWalk();
            const auto [x, y, z] = *snowDropSite;
            camera.position = {x + .5f, y + 3.f, z + 2.5f};
            camera.yaw = bx::kPi;
            camera.pitch = -.35f;
            camera.distance = 9.f;
        });
        button->setText(_("Snow drop"));
    }
    if (const char* fixture = std::getenv("ANYM_WEATHER_DEPOSITION_FIXTURE"))
        depositionFixture = std::string_view(fixture) == "1";
    depositionFixture |=
        weatherTravelSites.has_value() || polarPackSite.has_value();

    // The original inspection controls remain presentation-only. The
    // separate deposition fixture also drives the active physical sample.
    if (const char* weatherFixture = std::getenv("ANYM_WEATHER_FIXTURE");
        depositionFixture || distantWeatherFixture ||
        (weatherFixture && std::string_view(weatherFixture) == "1")) {
        auto* sixHours = makeDebugButton(
            {0, 9, 6, 1}, [this] { skipWeatherTime(6.0 / 24.0); });
        sixHours->setText(_("WEATHER +6H"));
        auto* oneDay =
            makeDebugButton({6, 9, 6, 1}, [this] { skipWeatherTime(1.0); });
        oneDay->setText(_("WEATHER +1D"));
        auto* thirtyDays =
            makeDebugButton({12, 9, 6, 1}, [this] { skipWeatherTime(30.0); });
        thirtyDays->setText(_("WEATHER +30D"));
        auto* halfYear =
            makeDebugButton({12, 8, 6, 1}, [this] { skipWeatherTime(180.0); });
        halfYear->setText(_("WEATHER +180D"));
        if (!distantWeatherFixture) {
            weatherForceButton = makeDebugButton({18, 9, 6, 1}, [this] {
                weatherForce = (weatherForce + 1) % 5;
                if (depositionFixture)
                    snowSettlement->setWeatherForce(clock.days(), weatherForce);
                updateWeatherControls();
            });
            weatherSiteButton =
                makeDebugButton({18, 8, 6, 1}, [this] { cycleWeatherSite(); });
        }
        if (depositionFixture) {
            auto* halfHour = makeDebugButton(
                {0, 8, 6, 1}, [this] { skipWeatherTime(.5 / 24); });
            halfHour->setText(_("WEATHER APPLY 30M"));
        }
        updateWeatherControls();
    }
    if (weatherTravelSites) {
        makeDebugButton({0, 10, 6, 1}, [this] {
            visitWeatherTravelSite(0);
        })->setText(_("Travel site A"));
        makeDebugButton({6, 10, 6, 1}, [this] {
            visitWeatherTravelSite(1);
        })->setText(_("Travel site B"));
        makeDebugButton({12, 10, 6, 1}, [this] {
            weatherTravelHold = !weatherTravelHold;
            snowCatchingUp = !weatherTravelHold;
        })->setText(_("Toggle catch-up hold"));
        makeDebugButton({18, 10, 6, 1}, [this] {
            if (weatherTravelHold) advanceSnow(true, 1. / 24);
        })->setText(_("Catch up 1h"));
    }
    if (const char* fixture = std::getenv("ANYM_THERMAL_FIXTURE");
        fixture && std::string_view(fixture) == "1") {
        if (heaterItemId)
            makeDebugButton({0, 6, 6, 1}, [this] {
                spawnThermalItem(heaterItemId);
            })->setText(_("HEATER ITEM"));
        if (coolerItemId)
            makeDebugButton({6, 6, 6, 1}, [this] {
                spawnThermalItem(coolerItemId);
            })->setText(_("COOLER ITEM"));
        makeDebugButton({12, 6, 6, 1}, [this] {
            dropThermalItem();
        })->setText(_("THERMAL DROP"));
        makeDebugButton({18, 6, 6, 1}, [this] {
            if (carriedThermalItem) thermalItemCarried = true;
        })->setText(_("THERMAL TAKE"));
        makeDebugButton({18, 7, 6, 1}, [this] {
            clearThermalItems();
        })->setText(_("CLEAR HEAT ITEMS"));
        if (thermalFixtureSite) {
            makeDebugButton({0, 7, 6, 1}, [this] {
                if (probe) toggleWalk();
                const auto [x, y, z] = *thermalFixtureSite;
                camera.position = {x + .5f, y + 3.f, z - 4.f};
                camera.yaw = 0;
                camera.pitch = -.28f;
                camera.distance = 12.f;
            })->setText(_("Heat site"));
            if (thermalDoorSite) {
                makeDebugButton({0, 5, 6, 1}, [this] {
                    if (!editor || !stoneId) return;
                    thermalDoorOpen = !thermalDoorOpen;
                    const uint8_t block = thermalDoorOpen ? 0 : stoneId;
                    editor->setBlock(*thermalDoorSite, block);
                    editor->setBlock(
                        {thermalDoorSite->x, thermalDoorSite->y + 1,
                         thermalDoorSite->z},
                        block);
                    lastBuildAction =
                        thermalDoorOpen ? _("Door opened") : _("Door closed");
                })->setText(_("ROOM DOOR"));
            }
            if (thermalRoofSite) {
                makeDebugButton({6, 5, 6, 1}, [this] {
                    if (!editor || !stoneId) return;
                    thermalRoofOpen = !thermalRoofOpen;
                    editor->setBlock(*thermalRoofSite,
                                     thermalRoofOpen ? 0 : stoneId);
                    lastBuildAction =
                        thermalRoofOpen ? _("Roof opened") : _("Roof closed");
                })->setText(_("ROOM ROOF"));
            }
            if (thermalHeaterSite) {
                makeDebugButton({12, 5, 6, 1}, [this] {
                    if (!editor || !heaterBlockId) return;
                    thermalHeaterOn = !thermalHeaterOn;
                    editor->setBlock(*thermalHeaterSite,
                                     thermalHeaterOn ? heaterBlockId : 0);
                    lastBuildAction =
                        thermalHeaterOn ? _("Heater on") : _("Heater off");
                })->setText(_("FIXED HEAT"));
            }
            if (thermostatBlockId) {
                makeDebugButton({18, 5, 6, 1}, [this] {
                    if (!editor || !stoneId) return;
                    thermalThermostatOn = !thermalThermostatOn;
                    const TilePos site{thermalFixtureSite->x + 2,
                                       thermalFixtureSite->y,
                                       thermalFixtureSite->z};
                    editor->setBlock(site, thermalThermostatOn
                                               ? thermostatBlockId
                                               : stoneId);
                    lastBuildAction = thermalThermostatOn
                                          ? _("Thermostat on (21 C)")
                                          : _("Thermostat off");
                })->setText(_("TARGET HEAT"));
            }
            makeDebugButton({0, 4, 6, 1}, [this] {
                if (!editor || !coolerBlockId) return;
                thermalCoolerOn = !thermalCoolerOn;
                const TilePos site{thermalFixtureSite->x, thermalFixtureSite->y,
                                   thermalFixtureSite->z + 2};
                editor->setBlock(site,
                                 thermalCoolerOn ? coolerBlockId : stoneId);
                lastBuildAction =
                    thermalCoolerOn ? _("Cooler on") : _("Cooler off");
            })->setText(_("COOLER"));
        }
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
    snowTerrain = std::make_unique<ChunkSnowTerrain>(
        *chunkManager, *this->world, *skyMap, *edits, polarPackSite);
    snowSettlement = std::make_unique<snow::SnowSettlement>(*snowTerrain);
    snowCover = std::make_unique<snow::SnowCover>();
    save_catalog::readSnow(slot, *snowSettlement);
    // v5 ledgers can carry the old automatic replay snow on open ground.
    // Rebase those columns onto the dated seasonal model once. Roofs,
    // tunnels and cuts keep their records because their support is not the
    // generated surface.
    if (this->world->width > 0 && !this->world->regions.empty()) {
        // The generated surface is available from the world map without
        // streaming chunks. Untouched columns can only hold automatic snow;
        // player-edited columns keep every deposit (tunnel, roof, floor).
        SurfaceSampler surface(*this->world, *context.assets);
        const auto dropped =
            snowSettlement->dropGroundLoose([&](int x, int z, int base) {
                const auto key = editKey(unsigned(x / Chunk::SIZE_X),
                                         unsigned(z / Chunk::SIZE_Z));
                if (edits->contains(key)) return false;
                const auto shape = surface.groundShape(x, z);
                const int ground =
                    surface.height(x, z) + (shape.code() ? 0 : 1);
                return base >= ground;
            });
        if (dropped)
            std::cerr << "Rebased " << dropped
                      << " old loose snow columns onto the seasonal pack\n";
    }
    // This projection starts empty. Only streaming notifications should
    // schedule columns restored from the ledger.
    const auto restored = snowSettlement->takeProjectionChanges();
    snowSettlement->takeFootprintChanges();
    snowCover->updateLooseExterior(*snowSettlement, restored, snowResident,
                                   snowId);
    snowPackChanges = snowSettlement->takePackChanges();
    // The ledger checkpoint is written before world.json.
    clock.set(std::max(slot.meta.worldDays, snowSettlement->weatherHorizon()));
    publishSnowSurface(true);
    savedSnowRevision = snowSettlement->revision();
    // The map covers every chunk column with loaded chunks.
    skyReadyConnection =
        chunkManager->becameReady.connect([this](const ChunkCoord& c) {
            skyMap->request(c.x, c.z);
            const snow::ChunkColumn column{c.x, c.z};
            snowResident.insert(column);
            if (snowSettlement->hasSnowInChunkColumn(column)) {
                snowDirtyColumns.insert(column);
                snowFullProjection.insert(column);
                // Make the ground loose tops visible to the distant height
                // field before this chunk can unload again.
                snowPackChanges.insert(column);
            }
        });
    snowColumnReleased =
        chunkManager->columnReleased.connect([this](uint64_t key) {
            const snow::ChunkColumn column{unsigned(key >> 32), unsigned(key)};
            snowResident.erase(column);
            if (snowSettlement->hasSnowInChunkColumn(column)) {
                snowDirtyColumns.insert(column);
                snowFullProjection.insert(column);
            }
        });
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
    editor->setEnvironmentRemoval([this](TilePos pos, auto id) {
        snowTerrain->resetQueries();
        if (!snowSettlement->excavatePack(pos, id) &&
            !snowSettlement->excavateLoose(pos, id))
            return false;
        const snow::ChunkColumn column{unsigned(pos.x / Chunk::SIZE_X),
                                       unsigned(pos.z / Chunk::SIZE_Z)};
        collectSnowChanges();
        snowDirtyColumns.erase(column);
        publishSnow({column});
        return true;
    });
    blockLight->watch(*editor);
    thermalTracker = std::make_unique<ThermalSourceTracker>(
        *chunkManager, context.assets->getStorage<Block>(), *thermalSources);
    thermalTracker->watch(*editor);
    thermalQuery = std::make_unique<thermal::Query>(
        *this->world, *thermalSources, thermal::voxelBarrier(*chunkManager));
    roomAir = std::make_unique<room::RoomAir>(
        *this->world, *chunkManager, *thermalSources,
        thermal::voxelBarrier(*chunkManager));
    {
        std::vector<room::Record> records;
        records.reserve(pendingRoomRecords.size());
        for (const auto& record : pendingRoomRecords)
            records.push_back({record.x, record.y, record.z, record.airC,
                               record.wallC, record.lastDay});
        roomAir->loadRecords(records);
    }
    pendingRoomRecords.clear();
    roomAir->watch(*editor);
    thermalQuery->setRoomProvider([this](std::array<double, 3> position,
                                         double worldDays,
                                         thermal::RoomSample& out) {
        return roomAir->sample(position, worldDays, out);
    });
    skyMap->watch(*editor);
    snowEditConnection =
        editor->blockChanged.connect([this](const BlockChange& change) {
            const auto [x, y, z] = change.pos;
            if (!snowSettlement->columnAt(x, z, snowTerrain->maxY()) &&
                !snowSettlement->surfacePack(x, z))
                return;
            snowTerrain->resetQueries();
            snowSettlement->supportChanged(change.pos);
            const int wx = snowTerrain->wrapX(x);
            const snow::ChunkColumn column{unsigned(wx / Chunk::SIZE_X),
                                           unsigned(z / Chunk::SIZE_Z)};
            snowDirtyColumns.insert(column);
            snowDirtyFootprints[column].emplace(wx, z);
        });
    if (weatherTravelSites || distantWeatherFixture || polarPackSite)
        clock.setSpeed(0);
    if (slot.meta.generatorVersion < WorldMeta::PolarPackVersion)
        snowSettlement->migrateUnvisitedWeather(clock.days());
    else
        snowSettlement->beginWeather(clock.days());
    snowSettlement->setWeatherForce(clock.days(), 0);
    weatherVisualDays = clock.days();
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
        if (snow::polar::supported(*this->world)) {
            const snow::polar::Baselines stock(*this->world);
            const double depth =
                stock.depth(int(centerX), int(centerZ),
                            snow::polar::seasonDay(slot.meta.worldDays));
            centerY += float(std::min(depth + 1., double(snow::MaximumDepth)));
        }

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
    removedConnection = chunkManager->removed.connect(
        [this, chunkChanged](const ChunkCoord& c) {
            chunkChanged(c);
            const snow::ChunkColumn column{c.x, c.z};
            if (snowResident.contains(column) &&
                snowSettlement->hasSnowInChunkColumn(column)) {
                snowDirtyColumns.insert(column);
                snowFullProjection.insert(column);
            }
        });

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

// A reproducible step 15/16 scene: a stone pad carrying a small sealed room
// with a doorway, a roof, a fixed heater, an optional thermostat and an
// optional cooler. The movable debug item is spawned from the Heat site
// controls. The room-air model reads the same blocks, so opening the door or
// removing the roof changes its leakage and heat loss.
void GameplayState::setupThermalFixture() {
    const char* enabled = std::getenv("ANYM_THERMAL_FIXTURE");
    if (!enabled || std::string_view(enabled) != "1" || !stoneId ||
        !heaterBlockId || !coolerBlockId)
        return;
    const int x = int(slot.meta.startRegion.x) * 128 + 64;
    const int z = int(slot.meta.startRegion.y) * 128 + 64;
    int maxTop = 0;
    for (int dz = -4; dz <= 4; ++dz)
        for (int dx = -4; dx <= 4; ++dx)
            maxTop = std::max(maxTop,
                              chunkManager->generatedSurfaceY(x + dx, z + dz));
    const int groundY = maxTop + 1;
    if (groundY + 7 >= WORLD_HEIGHT_Y) return;

    using namespace structure_voxels;
    constexpr uint64_t Key = 0xf000000000000015ULL;
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
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            support(x + dx, z + dz);
            layout.voxels[{x + dx, groundY, z + dz}] = OverrideVoxel(stoneId);
        }
    // Three-metre-high walls around a 3 x 3 interior. The two lower blocks on
    // the near wall stay open as a doorway.
    for (int dy = 1; dy <= 3; ++dy)
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                const bool boundary =
                    dx == -2 || dx == 2 || dz == -2 || dz == 2;
                if (!boundary) continue;
                if (dz == -2 && dx == 0 && dy <= 2) continue;
                layout.voxels[{x + dx, groundY + dy, z + dz}] =
                    OverrideVoxel(stoneId);
            }
    // Roof above the interior and the wall tops.
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx)
            layout.voxels[{x + dx, groundY + 4, z + dz}] =
                OverrideVoxel(stoneId);
    // Fixed heater on the west wall; thermostat and cooler sites stay empty
    // until their debug controls place them.
    layout.voxels[{x - 2, groundY + 1, z}] = OverrideVoxel(heaterBlockId);
    thermalHeaterSite = TilePos{x - 2, groundY + 1, z};
    thermalDoorSite = TilePos{x, groundY + 1, z - 2};
    thermalRoofSite = TilePos{x, groundY + 4, z};
    thermalFixtureSite = TilePos{x, groundY + 1, z};
    thermalDoorOpen = false;
    thermalRoofOpen = false;
    thermalHeaterOn = true;
    thermalThermostatOn = false;
    thermalCoolerOn = false;
    chunkManager->setGeneratedStructureKinds(
        applyOverrides({}, {{"thermal_fixture", {{Key, std::move(layout)}}}}));
}

void GameplayState::spawnThermalItem(uint8_t itemId) {
    if (!thermalSources || !itemId) return;
    const auto& items = uiContext->assets->getStorage<ThermalItem>();
    if (itemId == 0 || itemId > items.items.size()) return;
    const auto& item = items.get(itemId);
    if (!item.thermal.emits()) return;
    if (carriedThermalItem) {
        thermalSources->remove(*carriedThermalItem);
        carriedThermalItem.reset();
    }
    ThermalSource source;
    source.owner = ThermalSource::Owner::Item;
    source.item = itemId;
    source.emission = item.thermal;
    const auto position = [this] {
        if (probe) {
            const auto feet = probe->feet();
            return std::array<double, 3>{feet.x, double(feet.y) + 1.0, feet.z};
        }
        return std::array<double, 3>{camera.position.x, camera.position.y,
                                     camera.position.z};
    }();
    source.position = position;
    carriedThermalItem = thermalSources->add(std::move(source));
    thermalItemCarried = true;
    lastBuildAction = fmt::format(fmt::runtime(_("Carried {}")), item.name);
}

void GameplayState::updateThermalItem() {
    if (!thermalItemCarried || !carriedThermalItem || !thermalSources) return;
    if (!thermalSources->find(*carriedThermalItem)) {
        carriedThermalItem.reset();
        thermalItemCarried = false;
        return;
    }
    const std::array<double, 3> position = [this] {
        if (probe) {
            const auto feet = probe->feet();
            return std::array<double, 3>{double(feet.x), double(feet.y) + 1.0,
                                         double(feet.z)};
        }
        return std::array<double, 3>{double(camera.position.x),
                                     double(camera.position.y),
                                     double(camera.position.z)};
    }();
    thermalSources->place(*carriedThermalItem, position);
}

void GameplayState::dropThermalItem() {
    if (!carriedThermalItem || !thermalSources) return;
    if (const auto* source = thermalSources->find(*carriedThermalItem)) {
        auto position = source->position;
        if (probe)
            position[1] = double(probe->feet().y) + .2;
        else
            position[1] = std::max(0.0, position[1] - 1.5);
        thermalSources->place(*carriedThermalItem, position);
    }
    thermalItemCarried = false;
    lastBuildAction = _("Dropped the thermal item");
}

void GameplayState::clearThermalItems() {
    if (!thermalSources) return;
    std::vector<ThermalSourceRegistry::Id> ids;
    thermalSources->forEachItem(
        [&](ThermalSourceRegistry::Id id, const ThermalSource&) {
            ids.push_back(id);
        });
    for (const auto id : ids) thermalSources->remove(id);
    carriedThermalItem.reset();
    thermalItemCarried = false;
    lastBuildAction = _("Cleared thermal items");
}

void GameplayState::loadThermalSources(const DataStorage<ThermalItem>& items) {
    try {
        auto file = save_catalog::readThermalSources(slot);
        thermalExtra = std::move(file.extra);
        pendingRoomRecords = std::move(file.rooms);
        for (const auto& record : file.records) {
            const auto found = items.indexByName.find(record.item);
            if (found == items.indexByName.end()) {
                std::cerr << "Dropping thermal item " << record.item
                          << ": definition not found\n";
                continue;
            }
            const auto& item = items.items[found->second];
            if (!item.thermal.emits()) continue;
            ThermalSource source;
            source.owner = ThermalSource::Owner::Item;
            source.item = item.id;
            source.emission = item.thermal;
            source.position = record.position;
            source.key = record.key;
            const auto id = thermalSources->add(std::move(source));
            if (record.carried) {
                carriedThermalItem = id;
                thermalItemCarried = true;
            }
        }
    } catch (const std::exception& e) {
        // Item sources are debug content today; a corrupt file must not make
        // the world unreadable.
        std::cerr << "Thermal sources unreadable (" << e.what()
                  << "); starting empty\n";
    }
    thermalSources->dirty = false;
}

void GameplayState::saveThermalSources(bool wait) {
    using namespace std::chrono_literals;
    if (thermalWrite.valid()) {
        if (!wait && thermalWrite.wait_for(0s) != std::future_status::ready)
            return;
        try {
            thermalWrite.get();
        } catch (const std::exception& e) {
            std::cerr << "Saving thermal sources failed: " << e.what() << '\n';
            thermalSources->dirty = true;
        }
    }
    const bool roomsDirty = roomAir && roomAir->dirty();
    if (!thermalSources->dirty && !roomsDirty) return;
    thermalSources->dirty = false;
    if (roomAir) roomAir->clearDirty();
    const auto& items = uiContext->assets->getStorage<ThermalItem>();
    std::vector<thermal_sources_codec::Record> records;
    thermalSources->forEachItem([&](ThermalSourceRegistry::Id id,
                                    const ThermalSource& source) {
        if (!source.item || source.item > items.items.size()) return;
        records.push_back({source.position, items.get(source.item).name,
                           source.key,
                           carriedThermalItem && *carriedThermalItem == id});
    });
    std::vector<thermal_sources_codec::RoomRecord> rooms;
    if (roomAir) {
        for (const auto& record : roomAir->saveRecords())
            rooms.push_back({record.x, record.y, record.z, record.airC,
                             record.wallC, record.lastDay});
    }
    auto write = [target = slot, records = std::move(records),
                  rooms = std::move(rooms), extra = thermalExtra]() mutable {
        save_catalog::writeThermalSources(target, std::move(records),
                                          std::move(rooms), extra);
    };
    if (!wait) {
        thermalWrite = uiContext->threadPool.submit_task(std::move(write));
        return;
    }
    try {
        write();
    } catch (const std::exception& e) {
        std::cerr << "Saving thermal sources failed: " << e.what() << '\n';
        thermalSources->dirty = true;
    }
}

void GameplayState::applyWeatherOverride(world_weather::Sample& weather) const {
    weather_history::applyOverride(weather, weatherForce, depositionFixture);
}

void GameplayState::setupPolarPackFixture() {
    const char* enabled = std::getenv("ANYM_POLAR_SNOW_FIXTURE");
    if (!enabled || std::string_view(enabled) != "1" || !stoneId || !snowId ||
        slot.meta.generatorVersion < WorldMeta::PolarPackVersion)
        return;
    const auto wrap = world->wrap();
    const int x = int(
        wrap.wrapTile((slot.meta.startRegion.x + 1) * WorldWrap::RegionSize));
    const int z = slot.meta.startRegion.y * WorldWrap::RegionSize +
                  WorldWrap::RegionSize / 2;
    std::array<int, 36> ground{};
    int highest = 0;
    for (int dz = 0; dz < 6; ++dz)
        for (int dx = 0; dx < 6; ++dx) {
            const int px = int(wrap.wrapTile(x + dx - 3));
            auto& height = ground[dz * 6 + dx];
            height = chunkManager->generatedSurfaceY(px, z + dz);
            highest = std::max(highest, height);
        }
    // Start four metres below a slab boundary. The deep pack crosses it.
    const int base =
        ((highest + 5) / Chunk::SIZE_Y) * Chunk::SIZE_Y + Chunk::SIZE_Y - 4;
    if (base + snow::MaximumDepth >= WORLD_HEIGHT_Y) return;

    using namespace structure_voxels;
    Override layout;
    layout.mode = OverrideMode::Replace;
    layout.anchorX = x;
    layout.anchorZ = z;
    layout.landmark = true;
    for (int dz = 0; dz < 6; ++dz)
        for (int dx = 0; dx < 6; ++dx) {
            const int px = int(wrap.wrapTile(x + dx - 3));
            for (int y = ground[dz * 6 + dx] + 1; y < base; ++y)
                layout.voxels[{px, y, z + dz}] = OverrideVoxel(stoneId);
        }
    constexpr uint64_t Key = 0xf000000000000013ULL;
    chunkManager->setGeneratedStructureKinds(applyOverrides(
        {}, {{"polar_pack_fixture", {{Key, std::move(layout)}}}}));
    polarPackSite = TilePos{x, base, z};
}

void GameplayState::setupWeatherTravelFixture() {
    const char* enabled = std::getenv("ANYM_WEATHER_TRAVEL_FIXTURE");
    if (!enabled || std::string_view(enabled) != "1" || !stoneId ||
        world->width < 8)
        return;

    using namespace structure_voxels;
    constexpr uint64_t FixtureKey = 0xf00000000000000bULL;
    const int z = int(slot.meta.startRegion.y) * WorldWrap::RegionSize + 64;
    std::array<TilePos, 2> sites;
    Overrides::mapped_type layouts;
    for (size_t site = 0; site < sites.size(); ++site) {
        const int x =
            int(world->wrap().wrapRegion(slot.meta.startRegion.x + 4 * site)) *
                WorldWrap::RegionSize +
            64;
        int floor = 0;
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -3; dx <= 3; ++dx)
                floor = std::max(
                    floor, chunkManager->generatedSurfaceY(x + dx, z + dz));
        floor += 32;
        if (floor + 4 >= WORLD_HEIGHT_Y) return;

        Override layout;
        layout.mode = OverrideMode::Replace;
        layout.anchorX = x;
        layout.anchorZ = z;
        layout.landmark = true;
        const auto put = [&](int x, int y, int z) {
            layout.voxels[{x, y, z}] = OverrideVoxel(stoneId);
        };
        for (const int px : {x - 2, x + 2}) {
            for (int y = chunkManager->generatedSurfaceY(px, z) + 1; y <= floor;
                 ++y)
                put(px, y, z);
            // High banks isolate the two one-square-metre receiving pads.
            for (const auto& [dx, dz] :
                 {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                put(px + dx, floor, z + dz);
                put(px + dx, floor + 1, z + dz);
            }
        }
        put(x + 2, floor + 3, z);
        sites[site] = {x, floor, z};
        layouts.emplace(FixtureKey + site, std::move(layout));
    }
    weatherTravelSites = sites;
    chunkManager->setGeneratedStructureKinds(
        applyOverrides({}, {{"weather_travel_fixture", std::move(layouts)}}));
}

void GameplayState::visitWeatherTravelSite(size_t site) {
    if (probe) toggleWalk();
    const auto [x, y, z] = (*weatherTravelSites)[site];
    camera.position = {x + .5f, y + 1.5f, z + .5f};
    camera.yaw = 0;
    camera.pitch = -.74f;
    camera.distance = 11.f;
    snowCatchingUp = !weatherTravelHold;
}

world_weather::Sample GameplayState::weatherAt(const Vec3& position) const {
    auto weather = world_weather::at(
        *world, position.x, position.z, clock.days(), double(position.y),
        snowSettlement->modelForWeatherInterval(
            weather_history::intervalAt(clock.days())));
    if (distantWeatherFixture) {
        using namespace weather_presentation;
        const int x = regionAt(position.x), z = regionAt(position.z);
        const auto wrap = world->wrap();
        if (z >= 0 && z < world->height)
            distantWeatherFixture->apply(weather, wrap, int(wrap.wrapRegion(x)),
                                         z);
        else
            weather_history::applyOverride(weather, 1, false);
        const float edge = regionWeight(position.x, position.z, x, z);
        weather.cloudCover = std::lerp(.08f, weather.cloudCover, edge);
        weather.visualDensity *= edge;
    } else {
        applyWeatherOverride(weather);
    }
    return weather;
}

void GameplayState::visitDistantWeatherSite(bool seam, bool boundary) {
    if (probe) toggleWalk();
    using namespace weather_presentation;
    auto& fixture = *distantWeatherFixture;
    fixture.x = seam ? 0 : std::max(1, int(slot.meta.startRegion.x));
    const int x = fixture.x * RegionSize + (boundary ? 8 : 44);
    const int z = fixture.z * RegionSize + 98;
    const int top = chunkManager->generatedSurfaceY(x, z);
    camera.yaw = boundary ? -bx::kPiHalf : -.76f;
    camera.pitch = boundary ? -.13f : .14f;
    camera.distance = 10.f;
    const Vec3 eye{float(x), float(top) + (boundary ? 8.f : 104.f), float(z)};
    camera.position = eye + camera.direction() * camera.distance;
}

void GameplayState::updateWeatherControls() {
    static constexpr const char* ForceNames[] = {"AUTO", "CLEAR", "CLOUDY",
                                                 "RAIN", "SNOW"};
    static constexpr const char* SiteNames[] = {
        "START", "NORTH POLE", "EQUATOR", "SOUTH POLE", "X SEAM"};
    if (weatherForceButton)
        weatherForceButton->setText(
            std::string(_("WEATHER FORCE: ")) +
            _(ForceNames[weatherForce % std::size(ForceNames)]));
    if (weatherSiteButton)
        weatherSiteButton->setText(
            std::string(_("WEATHER SITE: ")) +
            _(SiteNames[weatherSiteIndex % std::size(SiteNames)]));
}

void GameplayState::cycleWeatherSite() {
    advanceSnow();
    static constexpr const char* SiteNames[] = {
        "START", "NORTH POLE", "EQUATOR", "SOUTH POLE", "X SEAM"};
    constexpr size_t SiteCount = std::size(SiteNames);
    weatherSiteIndex = (weatherSiteIndex + 1) % SiteCount;
    const Vec2u start = slot.meta.startRegion;
    Vec2u region = start;
    switch (weatherSiteIndex) {
        case 1:
            region = {start.x, 0};
            break;
        case 2:
            region = {start.x, unsigned(std::max(0, world->height / 2))};
            break;
        case 3:
            region = {start.x, unsigned(std::max(0, world->height - 1))};
            break;
        case 4:
            region = {0, start.y};
            break;
        default:
            break;
    }
    if (probe) toggleWalk();
    double x = (double(region.x) + .5) * WorldWrap::RegionSize;
    if (weatherSiteIndex == 4) x = .5;  // stand on the wrapped X seam
    const double z = (double(region.y) + .5) * WorldWrap::RegionSize;
    const int top = chunkManager->generatedSurfaceY(int(x), int(z));
    camera.position = {float(x), float(top) + 12.f, float(z)};
    camera.yaw = 0;
    camera.pitch = -.12f;
    camera.distance = 12.f;
    lastBuildAction =
        fmt::format(fmt::runtime(_("Weather site: {} [{}, {}]")),
                    _(SiteNames[weatherSiteIndex]), region.x, region.y);
    updateWeatherControls();
}

void GameplayState::startWeatherClockMeasurement(double speed) {
    clock.setSpeed(speed);
    measureSnow = true;
    snowWork = {};
}

void GameplayState::collectSnowChanges() {
    snowDirtyColumns.merge(snowSettlement->takeProjectionChanges());
    snowPackChanges.merge(snowSettlement->takePackChanges());
    for (const auto& [x, z] : snowSettlement->takeFootprintChanges())
        snowDirtyFootprints[{unsigned(x / Chunk::SIZE_X),
                             unsigned(z / Chunk::SIZE_Z)}]
            .emplace(x, z);
}

void GameplayState::updateSnowSeason() {
    if (!snowSurface || !snowSurface->initialPacks || depositionFixture ||
        !snow::polar::supported(*world) || clock.speed() > 1.)
        return;
    const int day = snow::polar::seasonDay(clock.days());
    if (day == snowSurface->day) return;

    const int previous = snowSurface->day;
    for (const auto& column : snowResident) {
        const int x = int(column.first) * Chunk::SIZE_X;
        const int z = int(column.second) * Chunk::SIZE_Z;
        if (snow::polar::seasonChanges(*world, x, z, x + Chunk::SIZE_X - 1,
                                       z + Chunk::SIZE_Z - 1, previous, day)) {
            snowDirtyColumns.insert(column);
            snowSeasonProjection.insert(column);
        }
    }
    publishSnowSurface();
}

void GameplayState::publishSnowSurface(bool all) {
    if (!snowSurface) all = true;
    const int day = depositionFixture ? 0
                    : !all && clock.speed() > 1.
                        ? snowSurface->day
                        : snow::polar::seasonDay(clock.days());
    if (!all && snowPackChanges.empty() && day == snowSurface->day) return;
    auto next =
        snowSurface && !all
            ? std::make_shared<snow::polar::SurfaceSnapshot>(*snowSurface)
            : std::make_shared<snow::polar::SurfaceSnapshot>();
    next->block = snowId;
    next->fixture = polarPackSite;
    next->initialPacks = snowSettlement->initialPacksEnabled();
    next->day = day;
    snowTerrain->setSeason(day);
    for (const auto& column : snowPackChanges) {
        auto pack = std::make_shared<snow::PackTops>();
        auto loose = std::make_shared<snow::PackTops>();
        snowSettlement->updatePackTops(*pack, *loose, column);
        if (pack->empty())
            next->edits.erase(column);
        else
            next->edits.insert_or_assign(column, std::move(pack));
        if (loose->empty())
            next->loose.erase(column);
        else
            next->loose.insert_or_assign(column, std::move(loose));
    }
    snowSurface = std::move(next);
    if (polarPackSite && all) {
        for (int dx : {-3, 2}) {
            const int x = snowTerrain->wrapX(polarPackSite->x + dx);
            snowPackChanges.emplace(x / Chunk::SIZE_X,
                                    polarPackSite->z / Chunk::SIZE_Z);
        }
    }
    terrainLod->setSnow(snowSurface, snowPackChanges);
    skyMap->setSnowSurface(snowSurface, snowPackChanges);
    snowPackChanges.clear();
}

void GameplayState::rebuildSnow(size_t maxColumns, bool streamingOnly) {
    if (!streamingOnly) {
        snowProjectionSeconds = 0;
        if (!snowId) return;
        collectSnowChanges();
    }
    if (!snowId || snowDirtyColumns.empty()) return;

    snow::ChunkColumns selected;
    const int cx = int(camera.position.x) / Chunk::SIZE_X;
    const int cz = int(camera.position.z) / Chunk::SIZE_Z;
    const int radius = terrainLod->settings().chunkRadius - 1;
    std::optional<snow::ChunkColumn> nearest;
    int best = 2 * radius * radius + 1;
    const auto findNearest = [&](const snow::ChunkColumns& columns) {
        for (const auto& column : columns) {
            if (!snowResident.contains(column)) continue;
            const int dx = int(world->wrap().deltaChunk(column.first, cx));
            const int dz = int(column.second) - cz;
            if (std::abs(dx) > radius || std::abs(dz) > radius) continue;
            const int distance = dx * dx + dz * dz;
            if (distance < best) {
                best = distance;
                nearest = column;
            }
        }
    };
    findNearest(snowFullProjection);
    if (!nearest && !streamingOnly) findNearest(snowSeasonProjection);
    if (nearest)
        selected.insert(snowDirtyColumns.extract(*nearest));
    else if (streamingOnly)
        return;

    while (selected.size() < maxColumns && !snowDirtyColumns.empty()) {
        auto next = snowDirtyColumns.upper_bound(lastSnowProjection);
        if (next == snowDirtyColumns.end()) next = snowDirtyColumns.begin();
        lastSnowProjection = *next;
        selected.insert(snowDirtyColumns.extract(next));
    }
    publishSnow(selected);
}

void GameplayState::publishSnow(const snow::ChunkColumns& selected) {
    if (measureSnow) snowWork.projectedColumns += selected.size();
    snowProjectionSeconds = 0;
    snowTerrain->resetQueries();
    // Geometry and ledger are read on the owner thread between chunk
    // publications. setEnvironment versions the immutable result for meshes.
    for (const auto& column : selected)
        if ((snowFullProjection.erase(column) |
             snowSeasonProjection.erase(column)) != 0)
            snowDirtyFootprints.erase(column);
    snowCover->rebuild(*snowSettlement, *snowTerrain, snowId, &snowResident,
                       &selected, &snowDirtyFootprints);
    snowCover->updateLooseExterior(*snowSettlement, selected, snowResident,
                                   snowId, &snowDirtyFootprints);
    for (const auto& column : selected) snowDirtyFootprints.erase(column);
    refreshSnowEnvironment();
}

void GameplayState::refreshSnowEnvironment() {
    const auto& changed = snowCover->changedChunks();
    if (changed.empty()) return;
    chunkManager->setEnvironment(snowCover->environment());
    snow::ChunkColumns columns;
    for (const auto& coord : changed) {
        columns.emplace(coord.x, coord.z);
        sunLight->markEnvironmentChanged(coord);
        blockLight->markChunkChanged(coord);
        floraRenderer->snowChanged(coord);
    }
    skyMap->setSnowEnvironment(*snowCover->environment(), columns, snowId);
}

void GameplayState::skipWeatherTime(double days) {
    clock.set(clock.days() + days);
    if (!weatherTravelHold) advanceSnow(true);
}

void GameplayState::clearSnow() {
    snowSettlement->clear();
    snowSettlement->resetWeather(clock.days());
    snowEditPending.reset();
    if (depositionFixture)
        snowSettlement->setWeatherForce(clock.days(), weatherForce);
    snowDirtyColumns.clear();
    snowFullProjection.clear();
    snowSeasonProjection.clear();
    snowDirtyFootprints.clear();
    snowPackChanges.clear();
    snowSettlement->takePackChanges();
    snowSettlement->takeFootprintChanges();
    snowSettlement->takeProjectionChanges();
    publishSnowSurface(true);
    snowCover->clear();
    refreshSnowEnvironment();
    lastBuildAction = _("Snow cleared");
}

bool GameplayState::reconcileSnowAt(int x, int z, double target) {
    x = snowTerrain->wrapX(x);
    if (!snowTerrain->inBounds(x, z)) return true;
    // Only the receiving pads emit snowfall inputs in the comparison scene.
    // This keeps the playcheck independent of the surrounding terrain.
    if (weatherTravelSites &&
        std::ranges::none_of(*weatherTravelSites, [&](const TilePos& site) {
            return z == site.z && (x == site.x - 2 || x == site.x + 2);
        }))
        return true;
    const double reached = precipitation::reconcile(
        *snowSettlement, *snowTerrain, x, z, target, !world->regions.empty(),
        [&](int64_t interval) {
            if (measureSnow) ++snowWork.weatherSamples;
            return snowHistory
                .at(*world, x, z, interval,
                    snowSettlement->modelForWeatherInterval(interval))
                .weather;
        },
        [&](double height) -> std::optional<bool> {
            const int y = int(std::floor(height + .0001));
            if (y >= WORLD_HEIGHT_Y) return false;
            const auto* tile = chunkManager->tileAt(x, y, z);
            return tile ? std::optional(tile->isLiquid()) : std::nullopt;
        },
        [&](double height) {
            // Same measured local °C as the tile panel and the room model:
            // an indoor receiver uses its connected room air, an outdoor one
            // still sees nearby sources. Historical replay uses the current
            // room state (the bounded approximation documented in the
            // handoff).
            if (!thermalQuery) return 0.;
            const auto local = thermalQuery->at(
                {double(x) + .5, height + .5, double(z) + .5}, clock.days());
            return double(local.temperatureC - local.ambientC);
        },
        2 * weather_history::IntervalsPerDay);
    if (measureSnow) ++snowWork.batches;
    return reached >= target;
}

bool GameplayState::prepareSnowEdit(TilePos pos) {
    if (!snowId || !weatherTravelSites) return true;
    snowTerrain->resetQueries();
    if (reconcileSnowAt(pos.x, pos.z, clock.days())) return true;
    snowEditPending = pos;
    snowCatchingUp = true;
    lastBuildAction = _("Weather catch-up pending at this tile");
    return false;
}

void GameplayState::trackSnowEdit(TilePos pos) {
    if (!snowId || depositionFixture || !snow::polar::supported(*world)) return;
    snowTerrain->resetQueries();
    const int x = snowTerrain->wrapX(pos.x);
    snowSettlement->trackLocalWeather(x, pos.z, clock.days());
    const snow::ChunkColumn column{unsigned(x / Chunk::SIZE_X),
                                   unsigned(pos.z / Chunk::SIZE_Z)};
    if (!snowResident.contains(column) ||
        !snowSettlement->hasSnowInChunkColumn(column))
        return;
    // An edit that changes a covered surface should show its snow now, not
    // on the next periodic publication.
    collectSnowChanges();
    snowDirtyColumns.erase(column);
    publishSnow({column});
}

void GameplayState::advanceSnow(bool throughNow, double maxDays) {
    snowUpdateSeconds = 0;
    if (!snowId) return;
    const bool seasonal = !depositionFixture && snow::polar::supported(*world);
    // Untouched terrain follows the dated seasonal stock, so it needs no
    // replay at any speed. Local history stays bounded by the two-day cap and
    // still advances during a fast clock, which keeps a roof or tunnel alive.
    if (seasonal && !snowSettlement->hasLocalWeather() &&
        !snowSettlement->pendingCount())
        return;
    snowTerrain->resetQueries();
    snowSettlement->settle(512);

    // Both resident and returning sites replay the same dated intervals.
    // Edits and debug jumps can also commit a partial current interval.
    const double completedInterval = weather_history::intervalStart(
        weather_history::intervalAt(clock.days()));
    const double target = throughNow || depositionFixture
                              ? clock.days()
                              : std::min(clock.days(), completedInterval);
    const int radius = seasonal ? 16 : 4;
    const int side = 2 * radius + 1, footprints = side * side;
    const int centreX = int(std::floor(camera.position.x));
    const int centreZ = int(std::floor(camera.position.z));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    int applied = 0;
    snowCatchingUp = false;
    if (snowEditPending && !weatherTravelHold) {
        const auto [x, y, z] = *snowEditPending;
        const double before = snowSettlement->weatherDay(x, z);
        if (reconcileSnowAt(x, z, clock.days())) snowEditPending.reset();
        applied = snowSettlement->weatherDay(x, z) > before;
        snowCatchingUp = snowEditPending && applied;
        if (std::chrono::steady_clock::now() >= deadline) {
            snowCatchingUp |= applied > 0;
            return;
        }
    }
    for (int i = 0; i < footprints; ++i) {
        const int index = int(snowNextFootprint++ % footprints);
        const int x = snowTerrain->wrapX(centreX + index % side - radius);
        const int z = centreZ + index / side - radius;
        if (!snowTerrain->inBounds(x, z)) continue;
        if (seasonal && !snowSettlement->tracksLocalWeather(x, z)) continue;
        // A settled local surface catches recent weather. Old seasons are
        // represented by the regional stock, without an unbounded edit queue.
        if (seasonal && snowSettlement->weatherDay(x, z) < target - 2.)
            snowSettlement->markWeatherApplied(x, z, target - 2.);
        const double start = snowSettlement->weatherDay(x, z);
        if (target <= start) continue;
        reconcileSnowAt(x, z, std::min(target, start + maxDays));
        const double reached = snowSettlement->weatherDay(x, z);
        applied += reached > start;
        snowCatchingUp |= reached > start && reached < target;
        if (applied == (seasonal ? 2 : 8) ||
            std::chrono::steady_clock::now() >= deadline) {
            snowCatchingUp |= applied > 0;
            break;
        }
    }
    if (applied) rebuildSnow(2);
}

void GameplayState::addSnow(const TileRaycastHit& hit) {
    snowTerrain->resetQueries();
    auto outcome =
        snowSettlement->deposit(hit.tx, hit.tz, hit.ty + 1, snowAddVolume);
    if (outcome == snow::DepositOutcome::Pending) {
        snowSettlement->settle();
        outcome = snowSettlement->pendingCount()
                      ? snow::DepositOutcome::Pending
                      : snow::DepositOutcome::Settled;
    }
    trackSnowEdit({hit.tx, hit.ty, hit.tz});
    rebuildSnow(4);
    lastBuildAction = fmt::format(
        fmt::runtime(_("Snow +{} cm3 at [{}, {}, {}]: {}")), snowAddVolume,
        hit.tx, hit.ty, hit.tz,
        outcome == snow::DepositOutcome::Settled ? _("settled")
        : outcome == snow::DepositOutcome::Sink  ? _("accounted sink")
                                                 : _("pending"));
}

GameplayState::~GameplayState() {
    // The previous camera borrows the weather texture from this state.
    uiContext->renderer->setCamera(Camera{});
    // Chunk systems outlive the light renderer these listeners use.
    chunkManager->meshUploaded.disconnect(meshConnection);
    chunkManager->removed.disconnect(removedConnection);
    chunkManager->becameReady.disconnect(skyReadyConnection);
    chunkManager->columnReleased.disconnect(snowColumnReleased);
    // The editor is destroyed before the heightmap.
    skyMap->unwatch();
    editor->blockChanged.disconnect(snowEditConnection);
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
        saveThermalSources(true);
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
    snowSettlement->checkpointWeather(slot.meta.worldDays);
    try {
        if (savedSnowRevision != snowSettlement->revision()) {
            save_catalog::writeSnow(slot, *snowSettlement);
            savedSnowRevision = snowSettlement->revision();
        }
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
            saveThermalSources(false);
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
        saveThermalSources(false);
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
    auto hit = pickTile(mouseX, mouseY);
    if (!hit) {
        lastBuildAction = _("No loaded piece in reach");
        return;
    }
    const snow::ChunkColumn column{unsigned(hit->tx / Chunk::SIZE_X),
                                   unsigned(hit->tz / Chunk::SIZE_Z)};
    if (snowSeasonProjection.contains(column) ||
        snowFullProjection.contains(column)) {
        // Project this column before editing. Otherwise the ray can pass
        // through snow that the still-pending projection would have added
        // and remove the substrate the player cannot see.
        collectSnowChanges();
        snowDirtyColumns.erase(column);
        publishSnow({column});
        const auto current = pickTile(mouseX, mouseY);
        if (!current || current->tx != hit->tx || current->ty != hit->ty ||
            current->tz != hit->tz || current->pieceId != hit->pieceId) {
            lastBuildAction = _("Snow cover changed. Select the piece again.");
            return;
        }
        hit = current;
    }
    if (tool == EditTool::SnowAdd) {
        if (prepareSnowEdit({hit->tx, hit->ty, hit->tz})) addSnow(*hit);
        return;
    }
    if (tool == EditTool::SnowClear) {
        clearSnow();
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
        if (!prepareSnowEdit({hit->tx, hit->ty, hit->tz})) return;
        const auto result = editor->removePiece({hit->tx, hit->ty, hit->tz},
                                                hit->revision, hit->pieceId);
        if (result == CellPieceEdit::Applied)
            trackSnowEdit({hit->tx, hit->ty, hit->tz});
        lastBuildAction = result == CellPieceEdit::Applied
                              ? _("Piece removed")
                              : pieceEditLabel(result);
        return;
    }
    if (tool == EditTool::PlantTree) {
        const auto& species = chunkManager->treeSpecies().species();
        if (species.empty()) return;
        const auto& rule = species[plantSpecies % species.size()];
        if (!prepareSnowEdit({hit->tx, hit->ty, hit->tz})) return;
        lastTreeAction =
            treeSim->plant({hit->tx, hit->ty, hit->tz}, rule.id, clock.days())
                ? "planted " + rule.species.name
                : "cannot plant here (solid ground, open air above)";
        return;
    }
    if (tool == EditTool::Place) {
        if (const auto placement = placementAt(*hit)) {
            if (placement->status == CellPieceEdit::Applied &&
                !prepareSnowEdit(placement->pos))
                return;
            const auto result =
                placement->status == CellPieceEdit::Applied
                    ? editor->placePiece(placement->pos, placement->revision,
                                         placement->piece)
                    : placement->status;
            if (result == CellPieceEdit::Applied) trackSnowEdit(placement->pos);
            lastBuildAction = result == CellPieceEdit::Applied
                                  ? _("Piece placed")
                                  : pieceEditLabel(result);
        }
        return;
    }
    if (testLightId && hit->placementOffset != TilePos{}) {
        const auto [dx, dy, dz] = hit->placementOffset;
        const TilePos pos{hit->tx + dx, hit->ty + dy, hit->tz + dz};
        if (prepareSnowEdit(pos) &&
            editor->placeBlock(pos, *testLightId) == EditStatus::Applied)
            trackSnowEdit(pos);
    }
}

void GameplayState::update(Game& game, float dt) {
    // Before any region can unload this frame.
    editTracker->update();
    chunkManager->commitWaterEdits();
    if (probe) updateWalk(dt);
    updateThermalItem();
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
    auto subsystem = std::chrono::steady_clock::now();
    const auto account = [&](double& total) {
        const auto now = std::chrono::steady_clock::now();
        total +=
            std::chrono::duration<double, std::milli>(now - subsystem).count();
        subsystem = now;
    };
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
    snowCover->appendLooseExterior(landmarks, chunkManager->wrap(),
                                   camera.eye(), camera.viewDistance);
    terrainLod->setStructures(std::move(landmarks));
    terrainLod->update(camera, uiContext->getWindowSize(),
                       lightRenderer->shadows().casterDetail());
    if (measureSnow) account(snowWork.lodMs);
    chunkManager->setFocus(camera.position);
    sunLight->setFocus(camera.position);
    blockLight->setFocus(camera.position);
    requestChunksAroundCamera();
    chunkManager->update(editor.get());
    if (measureSnow) account(snowWork.chunkMs);
    skyMap->setFocus(camera.position);
    skyMap->update();
    snowTerrain->resetQueries();
    auto skyLandmarks =
        treeRenderer->landmarkVoxels(chunkManager->woodDay(), false);
    for (auto& source : skyLandmarks)
        source = edits->maskStructure(source, chunkManager->wrap(), true);
    skyLandmarks.insert(skyLandmarks.end(), generatedLandmarks.begin(),
                        generatedLandmarks.end());
    snowCover->appendLooseExterior(skyLandmarks, chunkManager->wrap(),
                                   camera.eye(), SkyOcclusionMap::Extent / 2);
    skyMap->updateOcclusion(camera.eye(), std::move(skyLandmarks), *world,
                            *uiContext->assets);
    sunLight->update();
    blockLight->update();
    clock.advance(dt);
    // Rooms advance on their own cadence rather than every frame. Thermostat
    // commands and wall stores therefore change at a stable rate while the
    // camera can still move and stream chunks freely.
    roomUpdateSeconds += dt;
    if (roomAir && roomUpdateSeconds >= .25) {
        roomAir->update({double(camera.position.x), double(camera.position.y),
                         double(camera.position.z)},
                        clock.days());
        roomUpdateSeconds = 0;
    }
    snowUpdateSeconds += dt;
    // Wall time keeps an accelerated clock or a replay backlog from
    // continuously displacing frame work and in-flight mesh completions.
    auto startedSnow = std::chrono::steady_clock::time_point{};
    if (measureSnow) startedSnow = std::chrono::steady_clock::now();
    const double snowPeriod =
        snowCatchingUp ? (clock.speed() > 1 ? .1 : .5) : 1.;
    if (!weatherTravelHold && snowUpdateSeconds >= snowPeriod) advanceSnow();
    if (measureSnow) {
        const auto now = std::chrono::steady_clock::now();
        snowWork.replayMs +=
            std::chrono::duration<double, std::milli>(now - startedSnow)
                .count();
        startedSnow = now;
    }
    snowProjectionSeconds += dt;
    if (snowProjectionSeconds >= .125)
        rebuildSnow();
    else if (snowProjectionSeconds >= .04 && !snowFullProjection.empty())
        rebuildSnow(1, true);
    snowSurfaceSeconds += dt;
    if (snowSurfaceSeconds >= 1.) {
        updateSnowSeason();
        publishSnowSurface();
        snowSurfaceSeconds = 0;
    }
    if (measureSnow) account(snowWork.seasonMs);
    if (measureSnow)
        snowWork.projectionMs +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startedSnow)
                .count();
    const double visualDays = double(dt) * std::min(clock.speed(), 5.0) /
                              WorldClock::DayLengthSeconds;
    weatherVisualDays += visualDays;
    if (visualDays > 0.0) {
        const auto wind = weatherAt(camera.position);
        const double gameSeconds = visualDays * 86400.0;
        const double gameHours = gameSeconds / 3600.0;
        cloudDriftX += wind.windX * gameHours * .20;
        cloudDriftZ += wind.windZ * gameHours * .20;
        windTravelX += wind.windX * gameSeconds;
        windTravelZ += wind.windZ * gameSeconds;
    }
    chunkManager->advanceWater(uint64_t(clock.days() * 86400));
    if (measureSnow) account(snowWork.waterMs);
    floraSim->update(clock.days());
    if (measureSnow) account(snowWork.floraMs);
    treeSim->update(clock.days());
    if (measureSnow) account(snowWork.treeMs);
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
        if (polarPackSite) {
            const auto [x, y, z] = *polarPackSite;
            controls += fmt::format("Polar pack border [{}, {}, {}] to Y {}\n",
                                    x, y, z, y + snow::MaximumDepth);
        }
        if (thermalFixtureSite) {
            const auto [x, y, z] = *thermalFixtureSite;
            controls += fmt::format("Thermal site [{}, {}, {}]\n", x, y, z);
        }
        if (weatherTravelSites) {
            controls += fmt::format("Travel fixture {} · target day {:.4f}\n",
                                    weatherTravelHold ? "held" : "automatic",
                                    clock.days());
            for (size_t i = 0; i < weatherTravelSites->size(); ++i) {
                const auto [x, floor, z] = (*weatherTravelSites)[i];
                const auto padVolume = [&](int px) {
                    const auto* column =
                        snowSettlement->columnAt(px, z, floor + 1);
                    return column && column->baseY == floor + 1 ? column->volume
                                                                : 0;
                };
                controls += fmt::format(
                    "{} [{}, {}, {}] open {} / covered {} cm3\n"
                    "  through days {:.4f} / {:.4f}\n",
                    'A' + char(i), x, floor, z, padVolume(x - 2),
                    padVolume(x + 2), snowSettlement->weatherDay(x - 2, z),
                    snowSettlement->weatherDay(x + 2, z));
            }
        }
        if (distantWeatherFixture) {
            const auto eye = camera.eye();
            const auto regions = weatherRenderer->regionCount();
            controls += fmt::format(
                "Weather eye [{:.1f}, {:.1f}, {:.1f}] day {:.5f}\n"
                "Curtains {} regions / {} draws · {} summary rebuilds\n"
                "Near {} particles · weather CPU {:.3f} ms · frame {:.2f} ms\n",
                eye.x, eye.y, eye.z, clock.days(), regions, int(regions != 0),
                weatherRenderer->summaryRebuilds(),
                weatherRenderer->particleCount(), weatherFrameMs, dt * 1000.);
        }
        if (constructionSite || snowFixtureSite || snowDropSite ||
            thermalFixtureSite) {
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
            const auto weather = weatherAt(camera.position);
            const std::string climateLine = fmt::format(
                "D{}/M{}/Y{} climate {:.1f} C (baseline {:.1f}, weather "
                "{:+.1f})\n",
                date.day, date.month, date.year, weather.temperatureC,
                climate.temperatureC, weather.temperatureOffsetC);
            const std::string weatherLine = fmt::format(
                "weather {} cloud {:.0f}% precip {:.1f} mm/h visual {:.2f}, "
                "wind {:.1f} m/s, offset {:+.1f} C{}\n",
                _(world_weather::conditionName(weather.condition)),
                weather.cloudCover * 100.f, weather.precipitationRate,
                weather.visualDensity, weather.windSpeed,
                weather.temperatureOffsetC, weatherForce ? _(" (forced)") : "");
            std::string localLine;
            if (thermalQuery) {
                const auto local = thermalQuery->at(
                    {double(camera.position.x), double(camera.position.y),
                     double(camera.position.z)},
                    clock.days());
                localLine = fmt::format(
                    "local temperature {:.1f} C (ambient {:.1f}, sources "
                    "{:+.1f}, regional {:+.1f}, {} nearby)\n",
                    local.temperatureC, local.ambientC, local.detailedC,
                    local.regionalC, local.detailedSources);
                if (local.indoors) {
                    localLine += fmt::format(
                        "room air {:.1f} C, wall {:.1f} C, {} m3, leak {} "
                        "faces{}, {} sources, command {:+.1f} C{}\n",
                        local.roomC, local.wallC, local.roomVolume,
                        local.roomLeakFaces,
                        local.roomApproximate ? _(" (bounded)") : "",
                        local.roomSources, local.roomCommandC,
                        local.roomSaturated ? _(" saturated") : "");
                } else if (roomAir) {
                    const auto near = roomAir->nearest(
                        {double(camera.position.x), double(camera.position.y),
                         double(camera.position.z)},
                        24.);
                    if (near.valid)
                        localLine += fmt::format(
                            "nearby room air {:.1f} C, wall {:.1f} C, {} m3, "
                            "leak {} faces{}, command {:+.1f} C{}\n",
                            near.airC, near.wallC, near.volume, near.leakFaces,
                            near.approximateFaces ? _(" (bounded)") : "",
                            near.commandC,
                            near.saturated ? _(" saturated") : "");
                }
            }
            tileDebugText->setText(
                controls + climateLine + weatherLine + localLine +
                _("No tile") + "\n" + streaming + "LOD: " +
                std::to_string(terrainLod->patchCount()) + " patches, " +
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
                const auto pack = snowSettlement->surfacePack(hit->tx, hit->tz);
                const auto surface = precipitation::firstSurface(
                    *snowTerrain, hit->position.x, hit->position.z, pack);
                ss << "Precipitation: ";
                if (!surface)
                    ss << "geometry pending";
                else if (surface->height &&
                         *surface->height > hit->position.y + .001)
                    ss << fmt::format("covered at {:.2f} m", *surface->height);
                else
                    ss << "exposed";
                ss << "\n";
                const auto* column =
                    snowSettlement->columnAt(hit->tx, hit->tz, hit->ty + 1);
                const auto volume = column ? column->volume : 0;
                ss << fmt::format("volume: {} cm3, SWE {:.3f} mm", volume,
                                  volume / snow::SnowPerWater /
                                      precipitation::Cm3PerMillimetre);
                if (column) {
                    if (const auto level = snowSettlement->level(*column)) {
                        const double base = column->baseY + column->baseLocal;
                        ss << fmt::format(", level {:.3f} m, depth {:.3f} m",
                                          *level, *level - base);
                    }
                }
                if (pack)
                    ss << fmt::format(
                        "\nPack: {:.3f} m top, {:.3f} m upper run",
                        double(pack->top) / snow::UnitsPerCubicMetre,
                        double(pack->volume()) / snow::UnitsPerCubicMetre);
                ss << fmt::format(
                    "\nWeather through day {:.4f}, world day {:.4f}\n"
                    "Meltwater awaiting routing {:.1f} cm3\n",
                    snowSettlement->weatherDay(hit->tx, hit->tz), clock.days(),
                    snowSettlement->meltwaterCm3());
            }

            ss << "\n--- Climate ---\n";
            {
                const auto date = world_calendar::fromDays(clock.days());
                const auto climate = world_climate::at(
                    *world, double(hit->tx) + .5, double(hit->tz) + .5,
                    clock.days(), double(hit->ty) + .5);
                auto weather = world_weather::at(
                    *world, double(hit->tx) + .5, double(hit->tz) + .5,
                    clock.days(), double(hit->ty) + .5,
                    snowSettlement->modelForWeatherInterval(
                        weather_history::intervalAt(clock.days())));
                if (depositionFixture) applyWeatherOverride(weather);
                ss << fmt::format("date: D{} M{} Y{}\n", date.day, date.month,
                                  date.year);
                ss << fmt::format(
                    "baseline {:.1f} C (mean {:.1f}, seasonal {:+.1f}, "
                    "daily {:+.1f}, elevation {:+.1f})\n",
                    climate.temperatureC, climate.meanC, climate.seasonalC,
                    climate.dailyC, climate.elevationC);
                if (weatherForce && depositionFixture)
                    ss << _("physical weather forced by fixture\n");
                else if (weatherForce)
                    ss << _("presentation forced (sample unchanged)\n");
                ss << fmt::format("weather offset {:+.1f} C\n",
                                  weather.temperatureOffsetC);
                ss << fmt::format("local {:.1f} C\n", weather.temperatureC);
            }

            ss << "\n--- Weather ---\n";
            {
                auto weather =
                    weatherAt({float(hit->tx) + .5f, float(hit->ty) + .5f,
                               float(hit->tz) + .5f});
                ss << fmt::format(
                    "{} cloud {:.0f}% precip {:.1f} mm/h visual {:.2f}{}\n",
                    _(world_weather::conditionName(weather.condition)),
                    weather.cloudCover * 100.f, weather.precipitationRate,
                    weather.visualDensity, weatherForce ? _(" (forced)") : "");
                ss << fmt::format(
                    "wind {:.1f} m/s, snow fraction {:.2f}, sample v{}\n",
                    weather.windSpeed, weather.snowFraction,
                    world_weather::ModelVersion);
            }

            ss << "\n--- Temperature ---\n";
            if (thermalQuery) {
                const auto& [ox, oy, oz] = hit->placementOffset;
                auto local = thermalQuery->at(
                    {double(hit->tx) + .5 + ox, double(hit->ty) + .5 + oy,
                     double(hit->tz) + .5 + oz},
                    clock.days());
                // A picked solid tile is not itself room air. Prefer a
                // connected room in any of its six neighbours, so hovering a
                // wall, floor, roof or heater shows the room it bounds.
                if (!local.indoors) {
                    static constexpr int Neighbours[6][3] = {
                        {1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                        {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
                    for (const auto& d : Neighbours) {
                        const auto candidate =
                            thermalQuery->at({double(hit->tx) + .5 + d[0],
                                              double(hit->ty) + .5 + d[1],
                                              double(hit->tz) + .5 + d[2]},
                                             clock.days());
                        if (candidate.indoors) {
                            local = candidate;
                            break;
                        }
                    }
                }
                ss << fmt::format(
                    "ambient {:.1f} C\nsources {:+.1f} C ({} nearby)\n"
                    "regional {:+.1f} C ({} dated)\nlocal {:.1f} C\n",
                    local.ambientC, local.detailedC, local.detailedSources,
                    local.regionalC, local.regionalSources, local.temperatureC);
                if (local.indoors)
                    ss << fmt::format(
                        "room air {:.1f} C (wall {:.1f} C, ground {:.1f} C)\n"
                        "volume {} m3, wall {} faces, leak {} faces{}\n"
                        "thermostat command {:+.1f} C{}, {} sources\n",
                        local.roomC, local.wallC, local.roomGroundC,
                        local.roomVolume, local.roomWallFaces,
                        local.roomLeakFaces,
                        local.roomApproximate ? _(" (bounded)") : "",
                        local.roomCommandC,
                        local.roomSaturated ? _(" saturated") : "",
                        local.roomSources);
                else if (roomAir) {
                    const auto near = roomAir->nearest(
                        {double(hit->tx) + .5, double(hit->ty) + .5,
                         double(hit->tz) + .5},
                        16.);
                    if (near.valid)
                        ss << fmt::format(
                            "nearby room air {:.1f} C (wall {:.1f} C, ground "
                            "{:.1f} C)\nvolume {} m3, wall {} faces, leak {} "
                            "faces{}, command {:+.1f} C{}, {} sources\n",
                            near.airC, near.wallC, near.groundC, near.volume,
                            near.wallFaces, near.leakFaces,
                            near.approximateFaces ? _(" (bounded)") : "",
                            near.commandC,
                            near.saturated ? _(" saturated") : "",
                            near.sources);
                }
            }
            if (const auto* source =
                    thermalSources->blockThermal({hit->tx, hit->ty, hit->tz})) {
                ss << fmt::format(
                    "source {} command {:+.1f} C, target {}, deadband {}, "
                    "radius {:.1f} m{}\n",
                    source->emission.thermostat() ? _("target") : _("fixed"),
                    source->commandDeltaC, source->emission.targetC,
                    source->emission.deadbandC, source->emission.radius,
                    source->thermostatSaturated ? _(" saturated") : "");
            }
            if (carriedThermalItem) {
                if (const auto* source =
                        thermalSources->find(*carriedThermalItem))
                    ss << fmt::format(
                        "{} item at [{:.2f}, {:.2f}, {:.2f}]\n",
                        thermalItemCarried ? _("Carried") : _("Dropped"),
                        source->position[0], source->position[1],
                        source->position[2]);
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
    const auto weather = weatherAt(camera.eye());
    camera.sky.weather = {weather.cloudCover, weather.visualDensity,
                          weather.snowFraction,
                          float(std::fmod(weatherVisualDays * 24.0, 100000.0))};
    camera.sky.cloudOffset = {float(cloudDriftX), float(cloudDriftZ), 0, 0};
    lightRenderer->setSky(camera.sky);
    lightRenderer->update(*lightSources, camera, world->wrap(), now);
    lightRenderer->setSkyOcclusion(skyMap->skyOcclusion());
    terrainLod->render(target, *context.renderer, *chunkManager, camera,
                       context.getWindowSize(), lightVolumes.get(),
                       &snowFullProjection);

    camera.underwater = terrainLod->eyeUnderwater();
    snowTerrain->resetQueries();
    const auto weatherStarted = std::chrono::steady_clock::now();
    const float weatherRange = float(2 * terrainLod->settings().chunkRadius *
                                     weather_presentation::RegionSize);
    const auto viewport = context.getWindowSize();
    weatherRenderer->render(
        target, camera, *world, weather,
        {clock.days(), weatherVisualDays, windTravelX, windTravelZ,
         std::min(weatherRange, terrainLod->visibleDistance()),
         std::max(1.f, viewport.x) / std::max(1.f, viewport.y), weatherForce,
         distantWeatherFixture, true,
         snowSettlement->firstCurrentWeatherInterval()},
        *snowTerrain, *snowSettlement);
    if (distantWeatherFixture)
        weatherFrameMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - weatherStarted)
                             .count();
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
                                              &worldEye, &water, &snowSoft};
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
