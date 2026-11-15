#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "consts.hpp"
#include "core/assets_api.hpp"
#include "items/block.hpp"
#include "items/soil.hpp"
#include "region.hpp"

// An immutable, resolved profile. Safe to copy into generation jobs; no asset
// pointers or mutable random engines are accessed by column workers.
struct SoilProfile {
    uint8_t soil_id = 0;
    uint8_t block_id = 0;
    uint8_t topsoil_block_id = 0;
    std::array<int, 2> depth_m{0, 0};
    std::array<int, 2> topsoil_depth_m{0, 0};
    uint32_t seed = 0;
};

struct SoilColumn {
    std::array<RegionLayer, 2> layers{};
    int count = 0;
    int depth_m = 0;
};

class SoilGenerator {
   public:
    static bool allowed(const SoilPlacement& p, const Region& r) {
        const auto contains = [](const auto& values, auto value) {
            return values.empty() || std::find(values.begin(), values.end(),
                                               value) != values.end();
        };
        if (!p.allow_underwater && r.land_height_m < SEA_LEVEL_Y) return false;
        if (!contains(p.allowed_land, r.land) ||
            !contains(p.allowed_biomes, r.biome))
            return false;
        if (r.humidity < p.humidity_range[0] ||
            r.humidity > p.humidity_range[1] ||
            r.temperature_c < p.temperature_range_c[0] ||
            r.temperature_c > p.temperature_range_c[1])
            return false;
        for (auto f : p.required_features)
            if (!r.hasFeature(f)) return false;
        for (auto f : p.forbidden_features)
            if (r.hasFeature(f)) return false;
        return true;
    }

    static SoilProfile select(const Region& r, const IAssets& assets,
                              uint32_t seed) {
        SoilProfile result;
        result.seed = seed;
        if (!assets.hasStorage<Soil>()) return result;
        const auto& soils = assets.getStorage<Soil>();
        // Name order makes selection independent of manifest/file ordering.
        std::vector<const Soil*> ordered;
        for (const auto& s : soils.items) ordered.push_back(&s);
        std::sort(ordered.begin(), ordered.end(),
                  [](auto a, auto b) { return a->name < b->name; });
        int priority = std::numeric_limits<int>::lowest();
        double total = 0;
        for (const auto* s : ordered)
            for (const auto& p : s->placement)
                if (allowed(p, r)) {
                    if (p.priority > priority) {
                        priority = p.priority;
                        total = 0;
                    }
                    if (p.priority == priority) total += p.weight;
                }
        if (total <= 0) return result;
        double choice = unit(r.coord.x, r.coord.y, seed) * total;
        const Soil* selected = nullptr;
        for (const auto* s : ordered) {
            for (const auto& p : s->placement) {
                if (p.priority != priority || !allowed(p, r)) continue;
                selected = s;
                choice -= p.weight;
                if (choice < 0) break;
            }
            if (choice < 0) break;
        }
        if (!selected) return result;
        const auto& blocks = assets.getStorage<Block>();
        result.soil_id = selected->id;
        result.block_id = blocks.get(selected->block_name).id;
        result.depth_m = selected->depth_m;
        result.topsoil_depth_m = selected->topsoil_depth_m;
        if (selected->topsoil_block_name && r.land_height_m >= SEA_LEVEL_Y &&
            r.vegetation != VegetationType::NONE)
            result.topsoil_block_id =
                blocks.get(*selected->topsoil_block_name).id;
        return result;
    }

    static SoilColumn sample(const SoilProfile& p, const Region& r, int64_t x,
                             int64_t z) {
        SoilColumn out;
        if (!p.block_id) return out;
        const double exposure = field(x, z, 16, p.seed ^ 0x93a70u);
        double threshold = 0.04;
        if (r.land == LandType::MOUNTAINS) threshold = 0.55;
        if (r.land == LandType::PEAK) threshold = 0.75;
        if (r.hasFeature(FeatureMask::CLIFF) ||
            r.hasFeature(FeatureMask::ROCKY_SHORE))
            threshold = 0.82;
        if (r.hasFeature(FeatureMask::DESERT_DUNES)) threshold = 0;
        if (exposure < threshold) return out;
        const double variation = field(x, z, 48, p.seed ^ 0x471b9u);
        const auto interpolate = [](const std::array<int, 2>& range, double t) {
            return static_cast<int>(std::min<double>(
                range[1],
                range[0] + std::floor(t * (double(range[1]) - range[0] + 1))));
        };
        int depth = interpolate(p.depth_m, variation);
        if (r.land == LandType::MOUNTAINS || r.land == LandType::PEAK)
            depth = std::min(depth, 2);
        // Never replace beyond the finite geology represented by this world.
        depth = std::min(depth, std::max(0, r.land_height_m));
        if (depth <= 0) return out;
        const int top =
            p.topsoil_block_id
                ? std::min(depth, interpolate(p.topsoil_depth_m, variation))
                : 0;
        if (top > 0)
            out.layers[out.count++] = {RegionLayerType::Soil,
                                       p.topsoil_block_id,
                                       top,
                                       0,
                                       0,
                                       p.soil_id,
                                       true};
        if (depth > top)
            out.layers[out.count++] = {RegionLayerType::Soil,
                                       p.block_id,
                                       depth - top,
                                       top,
                                       0,
                                       p.soil_id,
                                       false};
        out.depth_m = depth;
        return out;
    }

   public:
    // Stable world-coordinate fields shared with terrain/profile blending.
    static double unit(int64_t x, int64_t z, uint32_t seed) {
        uint64_t h = static_cast<uint64_t>(x) * 0x9e3779b97f4a7c15ULL;
        h ^= static_cast<uint64_t>(z) * 0xbf58476d1ce4e5b9ULL;
        h ^= seed;
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
        return (h >> 11) * 0x1.0p-53;
    }
    static double field(double x, double z, int scale, uint32_t seed) {
        const auto gx = static_cast<int64_t>(std::floor(double(x) / scale));
        const auto gz = static_cast<int64_t>(std::floor(double(z) / scale));
        double tx = double(x) / scale - gx;
        double tz = double(z) / scale - gz;
        tx = tx * tx * (3 - 2 * tx);
        tz = tz * tz * (3 - 2 * tz);
        return std::lerp(
            std::lerp(unit(gx, gz, seed), unit(gx + 1, gz, seed), tx),
            std::lerp(unit(gx, gz + 1, seed), unit(gx + 1, gz + 1, seed), tx),
            tz);
    }
};
