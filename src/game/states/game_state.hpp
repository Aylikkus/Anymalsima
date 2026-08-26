#pragma once
#include "core/input_system.hpp"
#include <render/render_target.hpp>
#include "render/ui_context.hpp"

class Game;

class IGameState {
public:
    virtual ~IGameState() = default;

    virtual void handleInput(Game& game, const InputEvent& e, UIContext& context, float dt) = 0;
    virtual void update(Game& game, float dt) = 0;
    virtual void render(RenderTarget& target, UIContext& context) = 0;
};