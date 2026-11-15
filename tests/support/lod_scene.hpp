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
