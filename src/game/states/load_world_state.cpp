#include "load_world_state.hpp"

#include <fmt/format.h>

#include <iostream>

#include "create_or_load_state.hpp"
#include "gameplay_state.hpp"
#include "gettext_wrapper.hpp"

namespace {
constexpr float RowHeight = 56.f;
constexpr float RowGap = 6.f;
constexpr Color RowColor{224, 224, 224, 255};
constexpr Color SelectedColor{255, 210, 120, 255};

std::string displayTime(std::string iso) {
    std::replace(iso.begin(), iso.end(), 'T', ' ');
    return iso;
}
}  // namespace

LoadWorldState::LoadWorldState(Game& game, UIContext& context)
    : context(context) {
    ui_manager = std::make_unique<UIManager>(context);
    slots = save_catalog::list();

    auto root = std::make_unique<Panel>();
    root->setBackgroundColor({0, 0, 0, 255});
    root->setAnchor({0, 0, 1, 1});
    root->setGrid({24, 16});

    auto title = ui_manager->createWidget(std::make_unique<Text>(), root.get());
    title->setGridPlacement({1, 0, 22, 1});
    title->setText(_("LOAD WORLD"));

    auto list = ui_manager->createWidget(std::make_unique<ScrollContainer>(),
                                         root.get());
    list->setGridPlacement({1, 1, 21, 12});
    list->setBackgroundColor({32, 32, 32, 224});

    auto scroll = ui_manager->createWidget(
        std::make_unique<ScrollBar>(context, "ui.black.track_vertical",
                                    "ui.black.thumb_rect_vertical"),
        root.get());
    scroll->setGridPlacement({22, 1, 1, 12});
    scroll->setTarget(list);

    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& meta = slots[i].meta;
        auto row = ui_manager->createWidget(
            std::make_unique<TextButton>(context, "ui.black.9slice",
                                         "mouse_click"),
            list);
        const float top = RowGap + float(i) * (RowHeight + RowGap);
        row->setAnchor({0, 0, 1, 0});
        row->setOffset({RowGap, top, RowGap, -(top + RowHeight)});
        row->setTextAlignment(TextAlign::Left, VerticalAlign::Middle);
        row->setTextColor(RowColor);
        row->setText(fmt::format(
            fmt::runtime(_("Seed {}  |  {}x{}  |  created {}  |  played {}")),
            meta.seed, meta.width, meta.height, displayTime(meta.createdAt),
            displayTime(meta.lastPlayed)));
        row->onClick.connect([this, i]() { select(i); });
        rows.push_back(row);
    }

    status = ui_manager->createWidget(std::make_unique<Text>(), root.get());
    status->setGridPlacement({1, 14, 12, 1});
    status->setText(slots.empty() ? _("No saved worlds") : "");

    auto load_btn = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        root.get());
    load_btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    load_btn->setText(_("LOAD"));
    load_btn->setFontRole(FontRole::Bold);
    load_btn->setGridPlacement({16, 14, 3, 1});
    load_btn->onClick.connect([this]() { startLoading(); });

    auto return_btn = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        root.get());
    return_btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    return_btn->setText(_("RETURN"));
    return_btn->setFontRole(FontRole::Bold);
    return_btn->setGridPlacement({20, 14, 3, 1});
    return_btn->onClick.connect([this, &game, &context]() {
        if (!loading.valid())
            game.changeState<CreateOrLoadState>(game, context);
    });

    ui_manager->setRoot(std::move(root));
}

LoadWorldState::~LoadWorldState() {
    // The worker owns only the slot copy; wait so it never outlives us.
    if (loading.valid()) loading.wait();
}

void LoadWorldState::select(size_t index) {
    if (loading.valid()) return;
    if (selected && *selected < rows.size())
        rows[*selected]->setTextColor(RowColor);
    selected = index;
    rows[index]->setTextColor(SelectedColor);
}

void LoadWorldState::startLoading() {
    if (loading.valid() || !selected) return;
    status->setText(_("Loading..."));
    loading = std::async(std::launch::async, [slot = slots[*selected]]() {
        return save_catalog::readWorldMap(slot);
    });
}

void LoadWorldState::handleInput(Game& game, const InputEvent& e,
                                 UIContext& context, float dt) {
    ui_manager->handleInput(e, context);
}

void LoadWorldState::update(Game& game, float dt) {
    using namespace std::chrono_literals;
    if (!loading.valid() || loading.wait_for(0s) != std::future_status::ready)
        return;
    try {
        auto world = loading.get();
        game.changeState<GameplayState>(game, context, std::move(world),
                                        slots[*selected]);
    } catch (const std::exception& e) {
        std::cerr << "Loading world failed: " << e.what() << '\n';
        status->setText(_("This world could not be loaded"));
    }
}

void LoadWorldState::render(RenderTarget& target, UIContext& context) {
    ui_manager->render(target, context);
}
