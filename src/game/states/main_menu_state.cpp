#include "main_menu_state.hpp"
#include "game_state.hpp"
#include <fmt/xchar.h>
#include <thread>
#include "gettext_wrapper.hpp"

void MainMenuState::handleInput(Game &game, const InputEvent &e, UIContext &context, float dt)
{
    ui_manager->handleInput(e, context);
}

void MainMenuState::update(Game &game, float dt)
{
    animator.update(dt);
}

MainMenuState::MainMenuState(Game& game, UIContext& context)
{
    ui_manager = std::make_unique<UIManager>(context);
    if (!context.audioSystem->isMusicPlaying())
        context.audioSystem->playMusic("reflection", true);

    auto root = std::make_unique<Panel>();
    auto* root_ptr = root.get();
    root->setBackgroundColor({0, 0, 0, 255});
    root->setAnchor({0, 0, 1, 1});
    root->setGrid({24, 16});

    settings_panel = ui_manager->createWidget(std::make_unique<Panel>(), root.get());
    settings_panel->setVisible(false);
    settings_panel->setGrid({12, 12});
    settings_panel->setGridPlacement({-16, -16, 12, 12});
    settings_panel->setBackgroundColor({32,32,32,192});

    auto mv_text = ui_manager->createWidget(std::make_unique<Text>(), settings_panel);
    mv_text->setText(_F("Master Volume {0}", (int)context.audioSystem->getMasterVolume()));
    mv_text->setGridPlacement({1, 1, 10, 1});
    mv_text->setFontSizeRole(FontSizeRole::Title);
    mv_text->setFontRole(FontRole::Bold);
    mv_text->setVerticalAlign(VerticalAlign::Middle);

    auto mv_slider = ui_manager->createWidget(std::make_unique<Slider>(
        context, 
        "ui.white.track_horizontal", 
        "ui.white.thumb_circle"), settings_panel);
    mv_slider->setRange(0.0f, 100.0f);
    mv_slider->setStep(1.0f);
    mv_slider->setGridPlacement({1, 2, 10, 1});
    mv_slider->setAxis(ScrollAxis::Horizontal);
    mv_slider->setValue(context.audioSystem->getMasterVolume());
    mv_slider->onValueChanged.connect([&context, mv_text](float newV) {
        mv_text->setText(_F("Master Volume {0}", (int)newV));
        context.playerSettings->setMasterVolume(newV);
    });

    auto musv_text = ui_manager->createWidget(std::make_unique<Text>(), settings_panel);
    musv_text->setText(_F("Music Vol. {0}", (int)context.audioSystem->getMusicVolume()));
    musv_text->setGridPlacement({1, 3, 5, 1});
    musv_text->setFontSizeRole(FontSizeRole::Title);
    musv_text->setFontRole(FontRole::Bold);
    musv_text->setVerticalAlign(VerticalAlign::Middle);

    auto musv_slider = ui_manager->createWidget(std::make_unique<Slider>(
        context, 
        "ui.white.track_horizontal", 
        "ui.white.thumb_circle"), settings_panel);
    musv_slider->setRange(0.0f, 100.0f);
    musv_slider->setStep(1.0f);
    musv_slider->setGridPlacement({1, 4, 5, 1});
    musv_slider->setAxis(ScrollAxis::Horizontal);
    musv_slider->setValue(context.audioSystem->getMusicVolume());
    musv_slider->onValueChanged.connect([&context, musv_text](float newV) {
        musv_text->setText(_F("Music Vol. {0}", (int)newV));
        context.playerSettings->setMusicVolume(newV);
    });

    auto sv_text = ui_manager->createWidget(std::make_unique<Text>(), settings_panel);
    sv_text->setText(_F("Sound Vol. {0}", (int)context.audioSystem->getSoundVolume()));
    sv_text->setGridPlacement({6, 3, 5, 1});
    sv_text->setFontSizeRole(FontSizeRole::Title);
    sv_text->setFontRole(FontRole::Bold);
    sv_text->setVerticalAlign(VerticalAlign::Middle);

    auto sv_slider = ui_manager->createWidget(std::make_unique<Slider>(
        context, 
        "ui.white.track_horizontal", 
        "ui.white.thumb_circle"), settings_panel);
    sv_slider->setRange(0.0f, 100.0f);
    sv_slider->setStep(1.0f);
    sv_slider->setGridPlacement({6, 4, 5, 1});
    sv_slider->setAxis(ScrollAxis::Horizontal);
    sv_slider->setValue(context.audioSystem->getSoundVolume());
    sv_slider->onValueChanged.connect([&context, sv_text](float newV) {
        sv_text->setText(_F("Sound Vol. {0}", (int)newV));
        context.playerSettings->setSoundVolume(newV);
    });

    auto us_text = ui_manager->createWidget(std::make_unique<Text>(), settings_panel);
    us_text->setText(_F("Text Scale {:.0f}%", context.playerSettings->getUiScale()*100));
    us_text->setGridPlacement({1, 5, 10, 1});
    us_text->setFontSizeRole(FontSizeRole::Title);
    us_text->setFontRole(FontRole::Bold);
    us_text->setVerticalAlign(VerticalAlign::Middle);

    auto us_slider = ui_manager->createWidget(std::make_unique<Slider>(
        context, 
        "ui.white.track_horizontal", 
        "ui.white.thumb_circle"), settings_panel);
    us_slider->setRange(0.8f, 1.2f);
    us_slider->setStep(0.05f);
    us_slider->setGridPlacement({1, 6, 7, 1});
    us_slider->setAxis(ScrollAxis::Horizontal);
    us_slider->setValue(context.playerSettings->getUiScale());
    us_slider->onValueChanged.connect([&context, us_text](float newV) {
        us_text->setText(_F("Text Scale {:.0f}%", newV*100));
    });

    auto us_apply = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), settings_panel);
    us_apply->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    us_apply->setText(_("APPLY"));
    us_apply->setFontRole(FontRole::Bold);
    us_apply->setFontSizeRole(FontSizeRole::Large);
    us_apply->setGridPlacement({8, 6, 3, 1});
    us_apply->onClick.connect([&context, us_slider, this](){
        context.playerSettings->setUiScale(us_slider->getValue());
    });

    auto language_dd = ui_manager->createWidget(std::make_unique<Dropdown<std::string>>(
        context,
        "ui.black.9slice", 
        "ui.black.9slice_thin",
        "mouse_click"), settings_panel);
    language_dd->setGridPlacement({1, 8, 3, 1});
    language_dd->setHeaderFontSize(FontSizeRole::Large);
    language_dd->setFontStyle(FontRole::Bold);
    language_dd->addItem(context, "English", "en_US");
    language_dd->addItem(context, "Русский", "ru_RU");
    language_dd->addItem(context, "日本語", "ja_JP");
    std::string current_lang = context.playerSettings->getLanguage();
    if (current_lang == "en_US") {
        language_dd->setSelectedIndex(0);
    }
    else if (current_lang == "ru_RU") {
        language_dd->setSelectedIndex(1);
    }
    else if (current_lang == "ja_JP") {
        language_dd->setSelectedIndex(2);
    }

    auto save_settings = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), settings_panel);
    save_settings->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    save_settings->setText(_("SAVE"));
    save_settings->setFontRole(FontRole::Bold);
    save_settings->setFontSizeRole(FontSizeRole::Large);
    save_settings->setGridPlacement({8, 10, 3, 1});

    save_settings->onClick.connect([this, &context, root_ptr]() {

        std::thread([settings = context.playerSettings]() {
            settings->save("settings.json");
        }).detach();

        auto popup = ui_manager->createWidget(std::make_unique<Text>(), root_ptr);
        WidgetID id = popup->getID();

        GridPlacement start = {-3, 1, 3, 1};
        GridPlacement end   = {2, 1, 3, 1};

        popup->setFontRole(FontRole::Bold);
        popup->setFontSizeRole(FontSizeRole::Large);
        popup->setColor({0, 224, 0, 255});
        popup->setVisible(true);
        popup->setGridPlacement(start);
        popup->setBackgroundColor({32,32,32,192});
        popup->setVerticalAlign(VerticalAlign::Middle);
        popup->setHorizontalAlign(TextAlign::Center);
        popup->setText(_("SAVED!"));

        Animation::animateGridPlacement(animator, popup, start, end, 0.3f);

        Animation::delay(animator, 2.0f, [=, &context]() {

            if (popup) {
                Animation::animateGridPlacement(animator, popup, end, start, 0.3f);
            }

            Animation::delay(animator, 0.35f, [=]() {
                if (popup) {
                    popup->setVisible(false);
                    ui_manager->destroyWidget(popup->getID());
                }
            });
        });
    });

    auto play = ui_manager->createWidget(std::make_unique<TextButton>(
        context,
        "ui.black.9slice", 
        "fox_scream"), root.get());
    play->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    play->setText(_("PLAY"));
    play->setFontRole(FontRole::Bold);
    play->setFontSizeRole(FontSizeRole::Large);
    play->setGridPlacement({16, 6, 3, 1});

    play->onClick.connect([&game, &context, this](){
        animator.clear();
        game.changeState<CreateOrLoadState>(game, context);
    });


    auto settings = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), root.get());
    settings->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    settings->setText(_("SETTINGS"));
    settings->setFontRole(FontRole::Bold);
    settings->setFontSizeRole(FontSizeRole::Large);
    settings->setGridPlacement({16, 8, 3, 1});
    settings->onClick.connect([this, &context](){
        bool visible = settings_panel->getVisible();
        GridPlacement start = {-16, 2, 12, 12};
        GridPlacement end   = {2, 2, 12, 12};
        
        
        if (!visible) {
            settings_panel->setGridPlacement(start);
            settings_panel->setVisible(true);
            Animation::animateGridPlacement(animator, settings_panel, start, end, 0.3f);
        } 
        else {
            settings_panel->setGridPlacement(end);
            Animation::animateGridPlacement(animator, settings_panel, end, start, 0.3f);
            Animation::delay(animator, 0.3f, [=, this, &context]() {
                settings_panel->setVisible(false);
            });
        }
    });

    auto exit = ui_manager->createWidget(std::make_unique<TextButton>(
        context, 
        "ui.black.9slice", 
        "mouse_click"), root.get());
    exit->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    exit->setText(_("EXIT"));
    exit->setFontRole(FontRole::Bold);
    exit->setFontSizeRole(FontSizeRole::Large);
    exit->setGridPlacement({16, 10, 3, 1});
    exit->onClick.connect([&context, this](){
        animator.clear();
        context.platform->stop();
    });


    language_dd->onChanged.connect([=, &context](const std::string& newl){
        context.playerSettings->setLanguage(newl);
        mv_text->setText(_F("Master Volume {0}", (int)context.audioSystem->getMasterVolume()));
        musv_text->setText(_F("Music Vol. {0}", (int)context.audioSystem->getMusicVolume()));
        sv_text->setText(_F("Sound Vol. {0}", (int)context.audioSystem->getSoundVolume()));
        us_text->setText(_F("Text Scale {:.0f}%", context.playerSettings->getUiScale()*100));
        us_apply->setText(_("APPLY"));
        save_settings->setText(_("SAVE"));
        play->setText(_("PLAY"));
        settings->setText(_("SETTINGS"));
        exit->setText(_("EXIT"));
    });

    ui_manager->setRoot(std::move(root));
}

void MainMenuState::render(RenderTarget &target, UIContext &context)
{
    ui_manager->render(target, context);
}
