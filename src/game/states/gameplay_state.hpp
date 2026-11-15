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
#include "world/chunk_manager.hpp"
#include "world/light_volume_manager.hpp"
#include "world/location_generator.hpp"
#include "world/location_manager.hpp"
#include "world/save/region_store.hpp"
#include "world/save/save_catalog.hpp"
#include "world/sunlight_propagator.hpp"
#include "world/terrain_edit_tracker.hpp"
#include "world/terrain_lod_manager.hpp"
#include "world/tile_raycast.hpp"
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

    std::unique_ptr<LocationGenerator> generator;
    std::unique_ptr<ChunkManager> chunkManager;
    std::unique_ptr<LocationManager> locationMgr;
    std::unique_ptr<ChunkMesher> mesher;
    std::unique_ptr<SunLightPropagator> sunLight;
    // Listens to chunkManager signals; declared after it so it is destroyed
    // first.
    std::unique_ptr<LightVolumeManager> lightVolumes;
    std::unique_ptr<WorldEditor> editor;
    // Edits shown at distance (terrain_edits.dat). Declared before the LOD
    // manager and tracker that hold references to it.
    std::unique_ptr<TerrainEdits> edits;
    SectionContainer editsExtra;
    std::future<void> editsWrite;
    // Listens to chunkManager and editor; destroyed before both.
    std::unique_ptr<TerrainEditTracker> editTracker;

    std::unique_ptr<TerrainLodManager> terrainLod;
    UIContext* uiContext = nullptr;
    std::optional<Vec2u> focusRegion;
    ShaderProperty u_texture_array;
    ShaderProperty u_normal_array;
    ShaderProperty worldEye;

    std::string blockShader = "block";

    Text* tileDebugText = nullptr;

    // HUD block editing: a tool is armed by its button and applied with LMB.
    enum class EditTool { None, Place, Remove };
    EditTool tool = EditTool::None;
    TextButton* placeButton = nullptr;
    TextButton* removeButton = nullptr;
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
    void writeMeta();
};