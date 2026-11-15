#pragma once

#include <future>
#include <optional>
#include <vector>

#include "composite/text_button.hpp"
#include "game.hpp"
#include "game_state.hpp"
#include "ui_manager.hpp"
#include "widgets/scroll_bar.hpp"
#include "widgets/scroll_container.hpp"
#include "widgets/text.hpp"
#include "world/save/save_catalog.hpp"

// Lists worlds in saves/ and continues the selected one. The world map is
// decoded on a background thread; update() switches to gameplay when done.
class LoadWorldState : public IGameState {
   public:
    LoadWorldState(Game& game, UIContext& context);
    ~LoadWorldState() override;

    void handleInput(Game& game, const InputEvent& e, UIContext& context,
                     float dt) override;
    void update(Game& game, float dt) override;
    void render(RenderTarget& target, UIContext& context) override;

   private:
    void select(size_t index);
    void startLoading();

    UIContext& context;
    std::unique_ptr<UIManager> ui_manager;
    std::vector<SaveSlot> slots;
    std::vector<TextButton*> rows;
    std::optional<size_t> selected;
    Text* status = nullptr;
    std::future<std::unique_ptr<World>> loading;
};
