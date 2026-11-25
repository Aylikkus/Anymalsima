#pragma once
#include <optional>

#include "composite/dropdown.hpp"
#include "composite/text_button.hpp"
#include "game.hpp"
#include "game_state.hpp"
#include "main_menu_state.hpp"
#include "render/renderer_api.hpp"
#include "ui_manager.hpp"
#include "widgets/base_widget.hpp"
#include "widgets/button.hpp"
#include "widgets/scroll_bar.hpp"
#include "widgets/scroll_container.hpp"
#include "widgets/text.hpp"
#include "widgets/text_box.hpp"
#include "world/block_light_propagator.hpp"
#include "world/chunk_manager.hpp"
#include "world/flora_renderer.hpp"
#include "world/flora_simulation.hpp"
#include "world/grounded_probe.hpp"
#include "world/light_renderer.hpp"
#include "world/light_sources.hpp"
#include "world/light_volume_manager.hpp"
#include "world/location_generator.hpp"
#include "world/location_manager.hpp"
#include "world/save/region_store.hpp"
#include "world/save/save_catalog.hpp"
#include "world/sky_height_map.hpp"
#include "world/snow_cover.hpp"
#include "world/sunlight_propagator.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_lod_manager.hpp"
#include "world/tile_raycast.hpp"
#include "world/tree_renderer.hpp"
#include "world/tree_simulation.hpp"
#include "world/world_clock.hpp"
#include "world/world_editor.hpp"
#include "world/world_generation_task.hpp"

class GameplayState : public IGameState {
   public:
    // The camera starts from slot.meta.camera, or above slot.meta.startRegion
    // for a world that has never been played.
    GameplayState(
        Game& game, UIContext& context, std::shared_ptr<World> world,
        SaveSlot slot,
        std::optional<std::vector<TreeLandmark>> landmarks = std::nullopt);
    // Saves every resident region and world.json before workers stop.
    ~GameplayState() override;

    virtual void handleInput(Game& game, const InputEvent& e,
                             UIContext& context, float dt) override;
    virtual void update(Game& game, float dt) override;
    virtual void render(RenderTarget& target, UIContext& context) override;

    void requestChunksAroundCamera();

   private:
    std::unique_ptr<UIManager> ui_manager;
    std::shared_ptr<World> world;
    SaveSlot slot;
    // Declared before chunk users so it outlives them during destruction.
    std::unique_ptr<RegionStore> regionStore;

    // Far block lights (light_sources.dat) and future entity lights.
    // Declared before every system that holds a reference to it.
    std::unique_ptr<LightSourceRegistry> lightSources;
    SectionContainer lightsExtra;
    std::future<void> lightsWrite;

    std::unique_ptr<LocationGenerator> generator;
    std::unique_ptr<ChunkManager> chunkManager;
    // Snow simulation and its non-persistent environmental projection.
    // snowTerrain reads raw chunk solids, so settlement never sees its own
    // projection. Declared before the settlement that references it.
    std::unique_ptr<snow::SnowTerrain> snowTerrain;
    std::unique_ptr<snow::SnowSettlement> snowSettlement;
    std::unique_ptr<snow::SnowCover> snowCover;
    uint8_t snowId = 0;
    // cm3: the step 06 fixture amount for a 5 cm landing plus a 5 cm toe.
    int64_t snowAddVolume = 51250;
    void rebuildSnow();
    void addSnow(const TileRaycastHit& hit);
    std::unique_ptr<LocationManager> locationMgr;
    std::unique_ptr<ChunkMesher> mesher;
    // Edits shown at distance (terrain_edits.dat). Declared before the LOD
    // manager, tracker and sky heightmap that hold references to it.
    std::unique_ptr<TerrainEdits> edits;
    SectionContainer editsExtra;
    std::future<void> editsWrite;
    // Sky heightmap (docs/handoff/sky-light.md): listens to chunkManager and
    // edits (declared after them); sunLight listens to it (declared after
    // it). The editor it watches is released in ~GameplayState (unwatch).
    // Its overrides are sky.dat.
    std::unique_ptr<SkyHeightMap> skyMap;
    SectionContainer skyExtra;
    std::future<void> skyWrite;
    Signal<const ChunkCoord&>::Connection skyReadyConnection;
    // Terrain edits exposing or sealing aquifer rock register and close
    // runtime spring outlets (see docs/handoff/water.md, step 5).
    void saveSky(bool wait);
    std::unique_ptr<SunLightPropagator> sunLight;
    std::unique_ptr<WorldEditor> editor;
    // Artificial light. Listens to chunkManager, editor and lightSources;
    // declared after them so it is destroyed first.
    std::unique_ptr<BlockLightPropagator> blockLight;
    // Listens to chunkManager signals and reads blockLight; declared after
    // both so it is destroyed first.
    std::unique_ptr<LightVolumeManager> lightVolumes;
    // Listens to chunkManager and editor; destroyed before both.
    std::unique_ptr<TerrainEditTracker> editTracker;

    std::unique_ptr<TerrainLodManager> terrainLod;
    std::unique_ptr<LightRenderer> lightRenderer;
    // Declared after chunkManager and editor: they disconnect from their
    // signals.
    std::unique_ptr<FloraRenderer> floraRenderer;
    std::unique_ptr<TreeRenderer> treeRenderer;
    std::unique_ptr<FloraSimulation> floraSim;
    // Tree growth, cuts and planting; listens to editor (declared after it).
    std::unique_ptr<TreeSimulation> treeSim;
    SectionContainer structuresExtra;
    uint64_t savedStructureOverrideRevision = 0;
    bool importLegacyStructures = false;
    std::chrono::steady_clock::time_point nextStructureSave{};
    void saveReservoirs();
    uint64_t savedWaterRevision = 0;
    void saveStructures(bool force = false);
    TextButton* plantButton = nullptr;
    size_t plantSpecies = 0;  // index into treeSpecies().species()
    std::string lastTreeAction;
    void updatePlantLabel();
    // Game time (saved in world.json); the TIME button cycles debug speeds.
    WorldClock clock;
    TextButton* timeButton = nullptr;
    TextButton* harvestButton = nullptr;
    std::string lastHarvest;
    Signal<const ChunkCoord&>::Connection meshConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    uint64_t shadowLodVersion = 0;
    // Camera chunk of the last request pass (withdraws stale requests when
    // it changes).
    std::optional<ChunkCoord> requestCentre;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    // Shadow caster passes of the far lights due this frame.
    void renderShadowCasters(RenderTarget& target, double now);
    // Whether render() draws this chunk's blocks: inside the detail chunk
    // box around the camera target and in a detailed column (frustum and
    // mesh aside). Shared by the chunk pass, shadow casters and trees.
    UIContext* uiContext = nullptr;
    std::optional<Vec2u> focusRegion;
    ShaderProperty u_texture_array;
    ShaderProperty u_normal_array;
    ShaderProperty worldEye;

    std::string blockShader = "block";

    Text* tileDebugText = nullptr;

    // HUD block editing: a tool is armed by its button and applied with LMB.
    enum class EditTool {
        None,
        Place,
        Remove,
        PlaceLight,
        Harvest,
        PlantTree,
        SnowAdd,
        SnowClear
    };
    EditTool tool = EditTool::None;
    TextButton* placeButton = nullptr;
    TextButton* removeButton = nullptr;
    TextButton* snow5Button = nullptr;
    TextButton* snow50Button = nullptr;
    TextButton* snow5mButton = nullptr;
    TextButton* snowClearButton = nullptr;
    // Debug light source (assets/data/blocks/test_light.json).
    TextButton* lightButton = nullptr;
    static constexpr const char* TestLightBlock = "blocks.test_light";
    std::optional<uint8_t> testLightId;
    void setTool(EditTool value);
    void useTool(int mouseX, int mouseY);
    std::optional<TileRaycastHit> pickTile(int mouseX, int mouseY,
                                           bool liquids = false);

    enum class ShapeKind { Cube, Slab, Ramp, Wall, Layer, RampInfill };
    ShapeKind shapeKind = ShapeKind::Cube;
    unsigned facing = 0, thickness = 0, rampForm = 0;
    unsigned rampRun = 1, rampSegment = 1;
    TextButton *shapeButton = nullptr, *facingButton = nullptr,
               *thicknessButton = nullptr, *formButton = nullptr,
               *runButton = nullptr, *segmentButton = nullptr;
    TextButton *materialButton = nullptr, *targetButton = nullptr,
               *elevationButton = nullptr;
    unsigned layerThickness = 0, elevation = 0, infillHeight = 20;
    bool timber = false, sameTile = false;
    uint8_t stoneId = 0, timberId = 0;
    TileShape placementShape() const;
    struct PiecePlacement {
        TilePos pos;
        tile_geometry::TilePiece piece;
        SolidCellRevision revision;
        CellPieceEdit status;
    };
    std::optional<PiecePlacement> placementPreview;
    std::optional<PiecePlacement> placementAt(const TileRaycastHit& hit);
    std::string lastBuildAction;
    MeshHandle previewMesh;
    ShaderProperty previewColor;
    std::optional<tile_geometry::TilePiece> previewShape;
    void renderPlacementPreview(RenderTarget& target, IRenderer& renderer);
    std::optional<TilePos> constructionSite;
    void setupConstructionFixture();
    std::optional<TilePos> snowFixtureSite;
    std::optional<TilePos> snowDropSite;
    void setupSnowFixture();
    void updateShapeControls();

    std::optional<GroundedProbe> probe;
    Vec2 walkInput{};
    bool walkFast = false;
    float orbitDistance = 10;
    TextButton* walkButton = nullptr;
    void toggleWalk();
    void updateWalk(float dt);
    std::optional<Vec2i> cursor;

    Camera camera;
    bool rotating = false;
    int viewZ = 0;
    int lastMouseX = 0;
    int lastMouseY = 0;

    static constexpr float AutosaveInterval = 300.f;
    float autosaveTimer = 0;
    std::vector<Vec2u> autosaveQueue;
    void updateAutosave(float dt);
    // Writes terrain_edits.dat when it changed; wait=false runs on the pool.
    void saveTerrainEdits(bool wait);
    // Writes light_sources.dat when far block lights changed.
    void saveLightSources(bool wait);
    void loadLightSources(const DataStorage<Block>& blocks);
    void writeMeta();
};
