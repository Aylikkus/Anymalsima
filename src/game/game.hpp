#pragma once
#include <core/input_system.hpp>
#include <memory>
#include "render/render_target.hpp"
#include "render/ui_context.hpp"
#include "ui_manager.hpp"
#include "widgets/text.hpp"

class IGameState;

class Game {
public:
    Game(UIContext& context);
    ~Game();
    void handleInput(const InputEvent& e, UIContext& context, float dt);
    void update(float dt, UIContext& context);
    template<typename T, typename... Args>
    void changeState(Args&&... args);
    void render(RenderTarget& target, UIContext& context);
    void close();
    void frameEnd();
    
private:
    std::unique_ptr<IGameState> currentState;
    std::unique_ptr<IGameState> nextState;
    std::unique_ptr<UIManager> overlay;
    bool switchState = false;
    Text* fpsText;
};

template <typename T, typename... Args>
void Game::changeState(Args &&...args)
{
    if (currentState)
    {
        nextState = std::make_unique<T>(std::forward<Args>(args)...);
        switchState = true;
    }
    else {
        currentState = std::make_unique<T>(std::forward<Args>(args)...);
    }
}
