#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "items/ground_cover.hpp"
#include "soil_generator.hpp"

// Immutable, resolved ground cover of one region (like SoilProfile): safe to
// copy into generation jobs.
struct GroundCoverProfile {
    uint8_t cover_id = 0;
    int scale_m = 24;
    float threshold = 0.35f;
    float contrast = 1.5f;
    float max_slope = 0.8f;
    uint32_t seed = 0;
};

// Chooses the ground cover of a region and the cover density of its columns
// (docs/handoff/flora.md, section 2). Pure functions of their inputs.
class GroundCoverGenerator {
   public:
    // soilId: the soil the region selected (SoilGenerator::select). Covers
    // grow on topsoil, so a region without topsoil (no vegetation, or a soil
    // without a topsoil variant) gets none.
    static GroundCoverProfile select(const Region& r, uint8_t soilId,
                                     const IAssets& assets, uint32_t seed) {
        GroundCoverProfile result;
        if (!soilId || !assets.hasStorage<GroundCover>() ||
            !assets.hasStorage<Soil>())
            return result;
        const auto& soil = assets.getStorage<Soil>().get(soilId);
        if (!soil.topsoil_block_name) return result;
        const auto& properties = soil.propertiesFor(true);
        // Name order makes selection independent of file ordering.
        std::vector<const GroundCover*> ordered;
        for (const auto& c : assets.getStorage<GroundCover>().items)
            if (c.acceptsSoil(soil.name, properties) &&
                c.acceptsVegetation(r.vegetation))
                ordered.push_back(&c);
        std::sort(ordered.begin(), ordered.end(),
                  [](auto a, auto b) { return a->name < b->name; });
        int priority = std::numeric_limits<int>::lowest();
        double total = 0;
        for (const auto* c : ordered)
            for (const auto& p : c->placement)
                if (SoilGenerator::allowed(p, r)) {
                    if (p.priority > priority) {
                        priority = p.priority;
                        total = 0;
                    }
                    if (p.priority == priority) total += p.weight;
                }
        if (total <= 0) return result;
        double choice =
            SoilGenerator::unit(r.coord.x, r.coord.y, seed ^ 0x6c0f3u) * total;
        const GroundCover* selected = nullptr;
        for (const auto* c : ordered) {
            for (const auto& p : c->placement) {
                if (p.priority != priority || !SoilGenerator::allowed(p, r))
                    continue;
                selected = c;
                choice -= p.weight;
                if (choice < 0) break;
            }
            if (choice < 0) break;
        }
        if (!selected) return result;
        result.cover_id = selected->id;
        result.scale_m = selected->density_noise.scale_m;
        result.threshold = selected->density_noise.threshold;
        result.contrast = selected->density_noise.contrast;
        result.max_slope = selected->max_slope;
        // Keyed by name, not id, so the pattern survives new cover files.
        result.seed = seed ^ nameHash(selected->name);
        return result;
    }

    // Cover density 0-15 of the column at world (x, z). periodX: world width
    // in tiles when X wraps (0: no wrapping).
    static uint8_t density(const GroundCoverProfile& p, int64_t x, int64_t z,
                           double slope, int periodX = 0) {
        if (!p.cover_id) return 0;
        const double noise = SoilGenerator::field(double(x), double(z),
                                                  p.scale_m, p.seed, periodX);
        double value = std::clamp(
            (noise - p.threshold) * p.contrast / (1.0 - p.threshold), 0.0, 1.0);
        const double steep = slope / p.max_slope;
        value *= std::clamp(1.0 - steep * steep, 0.0, 1.0);
        return uint8_t(std::lround(value * 15));
    }

    static uint32_t nameHash(std::string_view name) {
        uint32_t h = 2166136261u;  // FNV-1a
        for (const char c : name) {
            h ^= uint8_t(c);
            h *= 16777619u;
        }
        return h;
    }
};
