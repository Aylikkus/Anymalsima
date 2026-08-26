#pragma once
#include "game_state.hpp"
#include "game.hpp"
#include "ui_manager.hpp"
#include "widgets/base_widget.hpp"
#include "widgets/button.hpp"
#include "widgets/text.hpp"
#include "widgets/scroll_container.hpp"
#include "widgets/scroll_bar.hpp"
#include "widgets/text_box.hpp"
#include "widgets/slider.hpp"
#include "composite/text_button.hpp"
#include "composite/dropdown.hpp"

#include "world/world_generation_task.hpp"

#include "main_menu_state.hpp"

class CreateWorldState : public IGameState {
public:
    CreateWorldState(Game& game, UIContext& context);

    virtual void handleInput(Game& game, const InputEvent& e, UIContext& context, float dt) override;
    virtual void update(Game& game, float dt) override;
    virtual void render(RenderTarget& target, UIContext& context) override;
private:
    std::unique_ptr<UIManager> ui_manager;
    std::unique_ptr<World> world;
    WorldGenTask world_gen;

    bool generating = false;
};