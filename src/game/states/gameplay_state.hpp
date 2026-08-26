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
#include "world/location_generator.hpp"
#include "world/location_manager.hpp"
#include "world/sunlight_propagator.hpp"
#include "world/tile_raycast.hpp"
#include "world/world_generation_task.hpp"

class GameplayState : public IGameState {
   public:
    GameplayState(Game& game, UIContext& context,
                  std::unique_ptr<World>&& world, const Region& initial);

    virtual void handleInput(Game& game, const InputEvent& e,
                             UIContext& context, float dt) override;
    virtual void update(Game& game, float dt) override;
    virtual void render(RenderTarget& target, UIContext& context) override;

    void requestChunksAroundCamera();

   private:
    std::unique_ptr<UIManager> ui_manager;
    std::unique_ptr<World> world;

    std::unique_ptr<LocationGenerator> generator;
    std::unique_ptr<ChunkManager> chunkManager;
    std::unique_ptr<LocationManager> locationMgr;
    std::unique_ptr<ChunkMesher> mesher;
    std::unique_ptr<SunLightPropagator> sunLight;

    ShaderProperty u_texture_array;

    std::string blockShader = "block";

    Text* tileDebugText = nullptr;

    Camera camera;
    bool rotating = false;
    int viewZ = 0;
    int lastMouseX = 0;
    int lastMouseY = 0;
};