#include "create_world_state.hpp"

#include <fmt/xchar.h>

#include <iostream>

#include "gameplay_state.hpp"
#include "gettext_wrapper.hpp"
#include "utils/enum_parser.hpp"
#include "utils/region_enum_maps.hpp"
#include "widgets/world_preview.hpp"
#include "world/save/save_catalog.hpp"

CreateWorldState::CreateWorldState(Game &game, UIContext &context) {
    ui_manager = std::make_unique<UIManager>(context);

    auto root = std::make_unique<Panel>();
    auto *root_ptr = root.get();
    root->setBackgroundColor({0, 0, 0, 255});
    root->setAnchor({0, 0, 1, 1});
    root->setGrid({24, 16});
    root->setGridHighlight(true);

    auto settings_panel =
        ui_manager->createWidget(std::make_unique<Panel>(), root.get());
    settings_panel->setGridPlacement({0, 0, 9, 16});
    settings_panel->setBackgroundColor({64, 64, 64, 224});
    settings_panel->setGrid({9, 16});

    auto return_btn = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        settings_panel);
    return_btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    return_btn->setText(_("RETURN"));
    return_btn->setFontRole(FontRole::Bold);
    return_btn->setGridPlacement({1, 14, 3, 1});
    return_btn->onClick.connect([&game, &context]() {
        game.changeState<CreateOrLoadState>(game, context);
    });

    auto generate_btn = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        settings_panel);
    generate_btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    generate_btn->setText(_("GENERATE"));
    generate_btn->setFontRole(FontRole::Bold);
    generate_btn->setGridPlacement({5, 14, 3, 1});
    generate_btn->onClick.connect([&game, &context, this]() {
        generating = true;
        std::random_device rd;
        std::mt19937_64 gen((uint64_t(rd()) << 32) ^ rd());
        auto gen_settigs = WorldGenSettings::fromSeed(gen());
        // X wraps: 512 regions (65536 tiles) is a whole number of distant
        // width here can be changed freely, it doesn't incur any errors
        // terrain roots (16 regions), so LOD wraps exactly too.
        gen_settigs.width = 512;
        gen_settigs.height = 300;
        world_gen.start(gen_settigs, context.assets);
    });

    auto choose_btn = ui_manager->createWidget(
        std::make_unique<TextButton>(context, "ui.black.9slice", "mouse_click"),
        settings_panel);
    choose_btn->setTextAlignment(TextAlign::Center, VerticalAlign::Middle);
    choose_btn->setText(_("CHOOSE"));
    choose_btn->setFontRole(FontRole::Bold);
    choose_btn->setGridPlacement({5, 12, 3, 1});

    auto cell_info_container = ui_manager->createWidget(
        std::make_unique<ScrollContainer>(), root.get());
    cell_info_container->setGridPlacement({19, 5, 4, 10});
    cell_info_container->setBackgroundColor({64, 64, 64, 224});

    auto cell_info_text =
        ui_manager->createWidget(std::make_unique<Text>(), cell_info_container);
    cell_info_text->setAnchor({0, 0, 1, 1});

    auto preview = ui_manager->createWidget(
        std::make_unique<WorldPreview>(context), root.get());
    preview->setAnchor({0, 0, 1, 1});
    preview->setBackgroundColor({0, 0, 0, 0});

    preview->onRegionHovered.connect([cell_info_text](const Region &r) {
        std::stringstream ss;
        ss << _("Height") << " " << fmt::format("{}", r.land_height_sea_m)
           << "\n";
        ss << _("Temperature C") << " "
           << fmt::format("{:.2f}", r.temperature_c) << "\n";
        ss << _("Humidity") << " " << fmt::format("{:.2f}", r.humidity) << "\n";

        ss << _("Biome") << " "
           << EnumParser<BiomeType>::toStringLocal(
                  RegionEnumMaps::biomeReverseLocalized(), r.biome)
           << "\n";
        ss << _("Land") << " "
           << EnumParser<LandType>::toStringLocal(
                  RegionEnumMaps::landReverseLocalized(), r.land)
           << "\n";
        ss << _("Vegetation") << " "
           << EnumParser<VegetationType>::toStringLocal(
                  RegionEnumMaps::vegetationReverseLocalized(), r.vegetation)
           << "\n\n";

        if (r.river.flow != RiverFlowDirection::NO) {
            ss << _("River") << "\n"
               << _("Direction") << " "
               << EnumParser<RiverFlowDirection>::toStringLocal(
                      RegionEnumMaps::flowDirectionReverseLocalized(),
                      r.river.flow)
               << "\n"
               << _("Flow Strength") << " "
               << fmt::format("{:.2f}", r.river.strength) << "\n\n";
        }

        ss << _("Features") << "\n";

        bool anyFeature = false;
        for (int i = 0; i < 32; ++i) {
            FeatureMask f = static_cast<FeatureMask>(1 << i);
            if ((r.features & f) != FeatureMask::None) {
                ss << " - "
                   << EnumParser<FeatureMask>::toStringLocal(
                          RegionEnumMaps::featuresReverseLocalized(), f)
                   << "\n";
                anyFeature = true;
            }
        }

        if (!anyFeature) ss << " - " << _("None") << "\n";

        // for (int i = 0; i < std::min((size_t)10, r.layers.size()); i++) {
        //     const auto &l = r.layers[i];
        //     switch (l.type) {
        //         case RegionLayerType::Rock: {
        //             const auto &rock = std::get<RockLayer>(l.layer).rock;
        //             ss << fmt::format("{} {} {} {}", _(rock->id),
        //                               l.depth_from_surface_m, l.thickness_m,
        //                               l.aquifer_saturation)
        //                << "\n";
        //         }
        //     }
        // }

        cell_info_text->setText(ss.str());
    });

    world_gen.onFinished.connect([preview, this]() {
        world = world_gen.takeResult();
        preview->setWorld(world.get());
    });

    choose_btn->onClick.connect([&game, &context, this, preview]() {
        auto r = preview->getSelectedRegion();
        if (!r || !world) return;
        try {
            auto slot = save_catalog::create(*world, r.value()->coord);
            game.changeState<GameplayState>(game, context, std::move(world),
                                            std::move(slot));
        } catch (const std::exception &e) {
            std::cerr << "Creating world save failed: " << e.what() << '\n';
        }
    });

    ui_manager->setRoot(std::move(root));
}

void CreateWorldState::handleInput(Game &game, const InputEvent &e,
                                   UIContext &context, float dt) {
    ui_manager->handleInput(e, context);
}

void CreateWorldState::update(Game &game, float dt) {
    if (generating) world_gen.update();
}

void CreateWorldState::render(RenderTarget &target, UIContext &context) {
    ui_manager->render(target, context);
}
