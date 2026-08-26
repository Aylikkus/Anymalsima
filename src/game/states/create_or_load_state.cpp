#include "create_or_load_state.hpp"
#include "gettext_wrapper.hpp"

#include "create_world_state.hpp"

CreateOrLoadState::CreateOrLoadState(Game& game, UIContext& context)
{
    ui_manager = std::make_unique<UIManager>(context);

    auto root = std::make_unique<Panel>();
    auto* root_ptr = root.get();
    root->setBackgroundColor({0, 0, 0, 256});
    root->setAnchor({0, 0, 1, 1});
    root->setGrid({24, 16});
    root->setGridHighlight(true);

    auto create_button = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), root.get());
    create_button->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    create_button->setText(_("CREATE WORLD"));
    create_button->setFontRole(FontRole::Bold);
    create_button->setFontSizeRole(FontSizeRole::Title);
    create_button->setGridPlacement({1,1, 10, 12});
    create_button->onClick.connect([&game, &context](){
        game.changeState<CreateWorldState>(game, context);
    });

    auto load_button = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), root.get());
    load_button->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    load_button->setText(_("LOAD WORLD"));
    load_button->setFontRole(FontRole::Bold);
    load_button->setFontSizeRole(FontSizeRole::Title);
    load_button->setGridPlacement({13, 1, 10, 12});
    load_button->onClick.connect([&game, &context](){
        // game.changeState<MainMenuState>(game, context);
    });

    auto return_btn = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), root.get());
    return_btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    return_btn->setText(_("RETURN"));
    return_btn->setFontRole(FontRole::Bold);
    return_btn->setGridPlacement({20,14, 3, 1});
    return_btn->onClick.connect([&game, &context](){
        game.changeState<MainMenuState>(game, context);
    });

    ui_manager->setRoot(std::move(root));
}

void CreateOrLoadState::handleInput(Game &game, const InputEvent &e, UIContext &context, float dt)
{
    ui_manager->handleInput(e, context);
}

void CreateOrLoadState::update(Game &game, float dt)
{
}

void CreateOrLoadState::render(RenderTarget &target, UIContext &context)
{
    ui_manager->render(target, context);
}
