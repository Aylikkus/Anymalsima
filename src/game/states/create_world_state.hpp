#pragma once
#include <future>

#include "composite/dropdown.hpp"
#include "composite/text_button.hpp"
#include "game.hpp"
#include "game_state.hpp"
#include "main_menu_state.hpp"
#include "ui_manager.hpp"
#include "widgets/base_widget.hpp"
#include "widgets/button.hpp"
#include "widgets/scroll_bar.hpp"
#include "widgets/scroll_container.hpp"
#include "widgets/slider.hpp"
#include "widgets/text.hpp"
#include "widgets/text_box.hpp"
#include "world/save/save_catalog.hpp"
#include "world/tree_generator.hpp"
#include "world/world_generation_task.hpp"

class WorldPreview;

class CreateWorldState : public IGameState {
   public:
    CreateWorldState(Game& game, UIContext& context);

    virtual void handleInput(Game& game, const InputEvent& e,
                             UIContext& context, float dt) override;
    virtual void update(Game& game, float dt) override;
    virtual void render(RenderTarget& target, UIContext& context) override;

   private:
    struct PreparedWorld {
        SaveSlot slot;
        std::vector<TreeLandmark> landmarks;
    };
    UIContext& context;
    TextButton* chooseButton = nullptr;
    WorldPreview* worldPreview = nullptr;
    std::unique_ptr<UIManager> ui_manager;
    Text* status = nullptr;
    std::shared_ptr<World> world;
    WorldGenTask world_gen;

    bool generating = false;
    // Destroy the future before the preview world it borrows.
    std::future<PreparedWorld> entering;
};