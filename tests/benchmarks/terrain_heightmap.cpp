// Dev tool: renders the generated surface of a seeded world window to a PPM
// image (water by depth, land by height, hillshaded) so coastline shapes and
// mountain relief can be inspected without a GPU.
//   terrain_heightmap [seed] [regionX regionZ] [regions] [out.ppm]
// Without a region, the window is centred on the region with the most
// land/water neighbours mixed with high ground.
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#include "generation/perlin_noise.hpp"
#include "test_assets.hpp"
#include "world/height_profile.hpp"
#include "world/terrain_sampler.hpp"

namespace {
// Mirrors WorldGenerator::generateHeight without the asset-dependent stages.
World heightWorld(uint64_t seed) {
    World world{};
    world.genSettings = WorldGenSettings::fromSeed(seed);
    world.width = world.genSettings.width;
    world.height = world.genSettings.height;
    world.regions.resize(size_t(world.width) * world.height);
    PerlinNoise noise(world.genSettings.height_seed);
    for (int z = 0; z < world.height; ++z)
        for (int x = 0; x < world.width; ++x) {
            auto& r = world.regions[z * world.width + x];
            r = test::regionAt(0);
            r.coord = {unsigned(x), unsigned(z)};
            float h = noise.getHeight(x, z, world.width, world.height);
            h = h < .5f ? h / .5f * .495f : .505f + (h - .5f) / .5f * .495f;
            r.height = h;
            r.land = h < .45f   ? LandType::WATER
                     : h < .5f  ? LandType::SHORE
                     : h < .65f ? LandType::PLAINS
                     : h < .7f  ? LandType::HILLS
                     : h < .85f ? LandType::MOUNTAINS
                                : LandType::PEAK;
            r.land_height_m = HeightProfile::heightToY(h);
            r.vegetation = VegetationType::GRASS;
        }
    return world;
}
}  // namespace

int main(int argc, char** argv) {
    const uint64_t seed = argc > 1 ? std::stoull(argv[1]) : 42;
    const auto world = heightWorld(seed);
    int cx = -1, cz = -1;
    if (argc > 3) {
        cx = std::stoi(argv[2]);
        cz = std::stoi(argv[3]);
    }
    const int span = argc > 4 ? std::stoi(argv[4]) : 6;
    const std::string out = argc > 5 ? argv[5] : "terrain_heightmap.ppm";
    if (cx < 0) {
        int best = -1;
        for (int z = 2; z < world.height - 2; ++z)
            for (int x = 2; x < world.width - 2; ++x) {
                int wet = 0, high = 0;
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int y =
                            world.regions[(z + dz) * world.width + x + dx]
                                .land_height_m;
                        wet += y < SEA_LEVEL_Y;
                        high = std::max(high, y);
                    }
                const int score = std::min(wet, 25 - wet) * 40 + high / 4;
                if (score > best) best = score, cx = x, cz = z;
            }
    }
    int peak = 0;
    for (const auto& r : world.regions) peak = std::max(peak, r.land_height_m);
    std::cout << "seed " << seed << " window centre region " << cx << ' ' << cz
              << " span " << span << " highest ceiling " << peak << '\n';

    test::Assets assets;
    std::map<std::pair<int, int>, TerrainNeighborhood> cache;
    const int x0 = (cx - span / 2) * 128, z0 = (cz - span / 2) * 128;
    const int size = span * 128;
    std::vector<double> heights(size_t(size) * size);
    int low = INT_MAX, high = INT_MIN;
    for (int z = 0; z < size; ++z)
        for (int x = 0; x < size; ++x) {
            const int wx = std::clamp(x0 + x, 0, world.width * 128 - 1);
            const int wz = std::clamp(z0 + z, 0, world.height * 128 - 1);
            const auto key = std::pair{wx / 128, wz / 128};
            auto it = cache.find(key);
            if (it == cache.end())
                it = cache
                         .emplace(key, TerrainNeighborhood::capture(
                                           world,
                                           {unsigned(key.first),
                                            unsigned(key.second)},
                                           assets))
                         .first;
            const double y = TerrainSampler(it->second).surface(wx, wz);
            heights[size_t(z) * size + x] = y;
            low = std::min(low, int(y));
            high = std::max(high, int(y));
        }
    std::cout << "surface range " << low << ".." << high << '\n';
    FILE* file = std::fopen(out.c_str(), "wb");
    if (!file) return 1;
    std::fprintf(file, "P6\n%d %d\n255\n", size, size);
    for (int z = 0; z < size; ++z)
        for (int x = 0; x < size; ++x) {
            const double y = heights[size_t(z) * size + x];
            const double west = heights[size_t(z) * size + std::max(0, x - 1)];
            const double north = heights[size_t(std::max(0, z - 1)) * size + x];
            const double shade =
                std::clamp(1 + .06 * ((y - west) + (y - north)), .5, 1.3);
            double r, g, b;
            if (y < SEA_LEVEL_Y) {
                const double t = std::clamp((SEA_LEVEL_Y - y) / 40, 0.0, 1.0);
                r = 40 * (1 - t) + 10 * t;
                g = 150 * (1 - t) + 40 * t;
                b = 220 * (1 - t) + 110 * t;
            } else if (y < SEA_LEVEL_Y + 4) {
                r = 225, g = 205, b = 140;  // beach band
            } else {
                const double t =
                    std::clamp((y - SEA_LEVEL_Y) /
                                   double(HeightProfile::PeakY - SEA_LEVEL_Y),
                               0.0, 1.0);
                // Continuous ramp: lowland green, rock brown, snow white.
                const double stops[4][4] = {{0, 80, 150, 60},
                                            {.25, 150, 130, 80},
                                            {.6, 110, 95, 85},
                                            {.8, 235, 235, 240}};
                int i = 0;
                while (i < 2 && t > stops[i + 1][0]) ++i;
                const double k = std::clamp(
                    (t - stops[i][0]) / (stops[i + 1][0] - stops[i][0]), 0.0,
                    1.0);
                r = std::lerp(stops[i][1], stops[i + 1][1], k);
                g = std::lerp(stops[i][2], stops[i + 1][2], k);
                b = std::lerp(stops[i][3], stops[i + 1][3], k);
                r *= shade, g *= shade, b *= shade;
            }
            if ((x0 + x) % 128 == 0 || (z0 + z) % 128 == 0)
                r *= .7, g *= .7, b *= .7;  // region grid
            const unsigned char px[3] = {
                (unsigned char)std::clamp(r, 0.0, 255.0),
                (unsigned char)std::clamp(g, 0.0, 255.0),
                (unsigned char)std::clamp(b, 0.0, 255.0)};
            std::fwrite(px, 1, 3, file);
        }
    std::fclose(file);
    std::cout << "wrote " << out << '\n';
}
