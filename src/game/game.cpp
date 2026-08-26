#include "game.hpp"

#include "states/game_state.hpp"
#include "states/main_menu_state.hpp"
#include <assert.h>
#include <fmt/xchar.h>

Game::Game(UIContext& context)
{
    changeState<MainMenuState>(*this, context);

    overlay = std::make_unique<UIManager>(context);

    auto root = std::make_unique<Panel>();
    auto* root_ptr = root.get();
    root->setBackgroundColor({0, 0, 0, 0});
    root->setAnchor({0, 0, 1, 1});
    root->setGrid({24, 16});


    fpsText = overlay->createWidget(std::make_unique<Text>(), root.get());
    fpsText->setGridPlacement({21, 1, 2, 1});
    fpsText->setFontSizeRole(FontSizeRole::Medium);
    fpsText->setVerticalAlign(VerticalAlign::Middle);

    overlay->setRoot(std::move(root));

}

void Game::handleInput(const InputEvent &e, UIContext& context, float dt)
{
    currentState->handleInput(*this, e, context, dt);
}

void Game::update(float dt, UIContext& context)
{
    fpsText->setText(fmt::format("FPS: {:.0f}", context.platform->getFps()));
    currentState->update(*this, dt);
}

void Game::render(RenderTarget &target, UIContext& context)
{
    currentState->render(target, context);
    overlay->render(target, context);
}

void Game::frameEnd()
{
    if (switchState)
    {
        currentState = std::move(nextState);
        switchState = false;
    }
}

void Game::close() {
    currentState.reset();
    nextState.reset();
    overlay.reset();
}

Game::~Game() {
    close();
}
