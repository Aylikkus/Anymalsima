#pragma once
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
#include "widgets/slider.hpp"
#include "widgets/text.hpp"
#include "widgets/text_box.hpp"
#include "world/block_light_propagator.hpp"
#include "world/chunk_manager.hpp"
#include "world/flora_renderer.hpp"
#include "world/flora_simulation.hpp"
#include "world/light_renderer.hpp"
#include "world/light_sources.hpp"
#include "world/light_volume_manager.hpp"
#include "world/location_generator.hpp"
#include "world/location_manager.hpp"
#include "world/save/region_store.hpp"
#include "world/save/save_catalog.hpp"
#include "world/sunlight_propagator.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_lod_manager.hpp"
#include "world/tile_raycast.hpp"
#include "world/world_clock.hpp"
#include "world/world_editor.hpp"
#include "world/world_generation_task.hpp"

class GameplayState : public IGameState {
   public:
    // The camera starts from slot.meta.camera, or above slot.meta.startRegion
    // for a world that has never been played.
    GameplayState(Game& game, UIContext& context,
                  std::unique_ptr<World>&& world, SaveSlot slot);
    // Saves every resident region and world.json before workers stop.
    ~GameplayState() override;

    virtual void handleInput(Game& game, const InputEvent& e,
                             UIContext& context, float dt) override;
    virtual void update(Game& game, float dt) override;
    virtual void render(RenderTarget& target, UIContext& context) override;

    void requestChunksAroundCamera();

   private:
    std::unique_ptr<UIManager> ui_manager;
    std::unique_ptr<World> world;
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
    std::unique_ptr<LocationManager> locationMgr;
    std::unique_ptr<ChunkMesher> mesher;
    std::unique_ptr<SunLightPropagator> sunLight;
    std::unique_ptr<WorldEditor> editor;
    // Artificial light. Listens to chunkManager, editor and lightSources;
    // declared after them so it is destroyed first.
    std::unique_ptr<BlockLightPropagator> blockLight;
    // Listens to chunkManager signals and reads blockLight; declared after
    // both so it is destroyed first.
    std::unique_ptr<LightVolumeManager> lightVolumes;
    // Edits shown at distance (terrain_edits.dat). Declared before the LOD
    // manager and tracker that hold references to it.
    std::unique_ptr<TerrainEdits> edits;
    SectionContainer editsExtra;
    std::future<void> editsWrite;
    // Listens to chunkManager and editor; destroyed before both.
    std::unique_ptr<TerrainEditTracker> editTracker;

    std::unique_ptr<TerrainLodManager> terrainLod;
    std::unique_ptr<LightRenderer> lightRenderer;
    // Declared after chunkManager and editor: they disconnect from their
    // signals.
    std::unique_ptr<FloraRenderer> floraRenderer;
    std::unique_ptr<FloraSimulation> floraSim;
    // Game time (saved in world.json); the TIME button cycles debug speeds.
    WorldClock clock;
    TextButton* timeButton = nullptr;
    TextButton* harvestButton = nullptr;
    std::string lastHarvest;
    Signal<const ChunkCoord&>::Connection meshConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    uint64_t shadowLodVersion = 0;
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    // Shadow caster passes of the far lights due this frame.
    void renderShadowCasters(RenderTarget& target, double now);
    UIContext* uiContext = nullptr;
    std::optional<Vec2u> focusRegion;
    ShaderProperty u_texture_array;
    ShaderProperty u_normal_array;
    ShaderProperty worldEye;

    std::string blockShader = "block";

    Text* tileDebugText = nullptr;

    // HUD block editing: a tool is armed by its button and applied with LMB.
    enum class EditTool { None, Place, Remove, PlaceLight, Harvest };
    EditTool tool = EditTool::None;
    TextButton* placeButton = nullptr;
    TextButton* removeButton = nullptr;
    // Debug light source (assets/data/blocks/test_light.json).
    TextButton* lightButton = nullptr;
    static constexpr const char* TestLightBlock = "blocks.test_light";
    std::optional<uint8_t> testLightId;
    // Debug sky dimmer (not saved): 0 = day, 1 = no sky light.
    Text* skyText = nullptr;
    Slider* skySlider = nullptr;
    void setTool(EditTool value);
    void useTool(int mouseX, int mouseY);
    // First solid block under the cursor.
    std::optional<TileRaycastHit> pickTile(int mouseX, int mouseY);

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