#pragma once
#include "world/world.hpp"

// Deterministic landscape with mountain silhouettes, mixed coastlines and
// cliffs. Shared by the CPU budget benchmark and optional Vulkan capture tool.
inline World lodScene(int side = 96) {
    World world{};
    world.width = world.height = side;
    world.genSettings.width = world.genSettings.height = side;
    world.genSettings.height_seed = 1234;
    world.genSettings.feature_seed = 5678;
    world.regions.resize(side * side);
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            auto& r = world.regions[z * side + x];
            r.coord = {unsigned(x), unsigned(z)};
            const double waves = std::sin(x * .24) * std::cos(z * .19) * 42;
            const double ridge =
                85 * std::exp(-std::pow((x - side * .5) / 3, 2));
            r.land_height_m = std::clamp(int(365 + waves + ridge), 280, 530);
            r.height = (r.land_height_m - 256) / 256.f;
            r.land =
                r.land_height_m > 440 ? LandType::MOUNTAINS : LandType::HILLS;
            r.crust = CrustType::CONTINENTAL;
            r.biome = BiomeType::TEMPERATE;
            r.humidity = .5f;
            r.temperature_c = 15;
            r.vegetation = VegetationType::GRASS;
            if (x % 11 == 0) r.addFeature(FeatureMask::CLIFF);
        }
    return world;
}

// A narrow westbound river crosses a regional slope and the X seam. The
// tributary shares the route to the open sea; a duplicate tests deduplication.
inline World riverLodScene(int width = 128) {
    World world{};
    world.width = width;
    world.height = 7;
    world.genSettings = WorldGenSettings::fromSeed(2024);
    world.genSettings.width = width;
    world.genSettings.height = world.height;
    world.regions.resize(width * world.height);
    for (int z = 0; z < world.height; ++z)
        for (int x = 0; x < width; ++x) {
            auto& r = world.regions[z * width + x];
            r.coord = {unsigned(x), unsigned(z)};
            r.land_height_m = x == width - 2   ? 350
                              : x == width - 1 ? 391
                              : x == 0         ? 435
                                               : 500;
            if (x == 0 && z < 3) r.land_height_m += 32;
            r.height = (r.land_height_m - 256) / 256.f;
            r.land = x == width - 2 ? LandType::WATER : LandType::HILLS;
            r.crust = CrustType::CONTINENTAL;
            r.biome = BiomeType::TEMPERATE;
            r.humidity = .5f;
            r.temperature_c = 15;
            r.vegetation = VegetationType::GRASS;
        }
    RiverPolyline main;
    main.path = {{{1, 3}, 512},
                 {{0, 3}, 512},
                 {{float(width - 1), 3}, 512},
                 {{float(width - 2), 3}, 512}};
    RiverPolyline branch;
    branch.path = {{{0, 1}, 1},
                   {{0, 3}, 1},
                   {{float(width - 1), 3}, 512},
                   {{float(width - 2), 3}, 512}};
    world.rivers = {std::move(main), std::move(branch)};
    world.rivers.push_back(world.rivers.front());
    return world;
}
