#pragma once

#include <random>

#include "generation/perlin_noise.hpp"
#include "region.hpp"
#include "world.hpp"

#include "items/block.hpp"

class LayersGenerator {
   public:
    explicit LayersGenerator(const WorldGenSettings &settings, IAssets *assets)
        : settings(settings),
          assets(assets),
          rockNoise(settings.feature_seed + 1337) {}

    std::vector<RegionLayer> get(const Region &region, int y_limit = 0) {
        /* ROCKS */

        const auto &rocks = assets->getStorage<Rock>();
        const auto &blocks = assets->getStorage<Block>();

        std::vector<RegionLayer> result;

        // 1. collect candidates
        std::vector<const Rock *> candidates;
        rocks.forEach([&](const Rock &rock) {
            if (isRockAllowed(rock, region)) candidates.push_back(&rock);
        });

        if (candidates.empty())
            throw std::runtime_error(
                "Can't place rocks on region. Please add more rocks!");

        // 2. pick base rock (bottom)
        std::vector<const Rock *> baseCandidates;

        for (auto rock : candidates) {
            if (region.crust == CrustType::CONTINENTAL &&
                (rock->category == RockType::IGNEOUS_INTRUSIVE ||
                 rock->category == RockType::SEDIMENTARY))
                baseCandidates.push_back(rock);

            else if (region.crust == CrustType::OCEANIC &&
                     rock->category == RockType::IGNEOUS_EXTRUSIVE)
                baseCandidates.push_back(rock);

            else if (region.crust == CrustType::TRANSITIONAL &&
                     rock->category == RockType::SEDIMENTARY)
                baseCandidates.push_back(rock);
        }

        float nx = float(region.coord.x) / settings.width;
        float ny = float(region.coord.y) / settings.height;

        // large-scale blobs
        float field = rockNoise.fbmTileable(
            region.coord.x * 0.01f, region.coord.y * 0.01f,
            settings.width * 0.01f, settings.height * 0.01f);

        const Rock *base = pickRock(baseCandidates, field, dist(rng));
        if (!base)
            throw std::runtime_error(
                "Can't choose base rock on region! Please add more rocks!");

        int neededDepth;
        if (y_limit != 0) {
            neededDepth = std::min(region.land_height_m, y_limit);
        } else {
            neededDepth = region.land_height_m;
        }
        int depth = 0;

        // base layer
        {
            int thickness;
            if (base->body_size_m.has_value())
                thickness = base->body_size_m->first +
                            dist(rng) * (base->body_size_m->second -
                                         base->body_size_m->first);
            else {
                thickness = 10 + dist(rng) * 20;  // 10 - 30 m fallback
            }

            if (depth + thickness > neededDepth)
                thickness = neededDepth - depth;

            int depth_from_surface = neededDepth - (depth + thickness);

            result.push_back(RegionLayer(RegionLayerType::Rock, blocks.get(base->block_name).id,
                                         thickness, depth_from_surface, 0, base->id));

            depth += thickness;
        }

        // 3. add upper layers
        const Rock *prevRock = base;
        while (depth < neededDepth) {
            const Rock *rock = pickRock(candidates, field, dist(rng));

            if (!canFollow(prevRock, rock)) continue;

            int thickness;

            if (rock->body_size_m.has_value()) {
                thickness = rock->body_size_m->first +
                            dist(rng) * (rock->body_size_m->second -
                                         rock->body_size_m->first);
            } else {
                thickness = dist(rng) * 5;
            }

            if (thickness < 1) {
                continue;
            }

            if (depth + thickness > neededDepth)
                thickness = neededDepth - depth;

            if (thickness <= 0) break;

            int depth_from_surface = neededDepth - (depth + thickness);
            result.push_back(RegionLayer(RegionLayerType::Rock, blocks.get(rock->block_name).id,
                                         thickness, depth_from_surface, 0, rock->id));
            prevRock = rock;

            depth += thickness;
        }

        // 4. apply transforms (metamorphism)
        for (auto &layer : result) {
            int depth_m = layer.depth_from_surface_m;

            bool transform_end = false;

            while (!transform_end) {
                bool transformed = false;
                const Rock &r = rocks.get(layer.data_id);
                const auto &transforms = r.transforms;
                for (const auto &tr : transforms) {
                    if (tr.min_depth_m && depth_m < tr.min_depth_m) {
                        continue;
                    }

                    const Rock &newRock = rocks.get(tr.result);

                    if (!isRockAllowed(newRock, region, true)) {
                        continue;
                    }

                    layer.block_id = blocks.get(newRock.block_name).id;
                    layer.data_id = newRock.id;
                    transformed = true;
                }

                if (!transformed) transform_end = true;
            }
        }

        std::reverse(result.begin(), result.end());

        /* AQUIFERS */

        float waterInfluence = std::exp(-region.water_distance / 50.0f);

        for (auto &layer : result) {
            if (layer.type != RegionLayerType::Rock) continue;

            const Rock &r = rocks.get(layer.data_id);

            const auto &p = r.properties;

            if (p.porosity < 0.02f && p.permeability < 0.005f) continue;

            float depth = layer.depth_from_surface_m;

            float depthFactor = std::clamp(depth / 500.0f, 0.0f, 1.0f);

            float porosityFactor = std::clamp(p.porosity / 0.3f, 0.0f, 1.0f);
            float permeabilityFactor =
                std::clamp(p.permeability / 0.1f, 0.0f, 1.0f);

            float flowFactor = std::sqrt(permeabilityFactor);
            float storageFactor = porosityFactor;
            float pressureFactor = 0.3f + 0.7f * depthFactor;

            float saturation =
                waterInfluence * flowFactor * storageFactor * pressureFactor;

            if (saturation < 0.1f) continue;

            layer.aquifer_saturation = saturation;
        }

        return std::move(result);
    }

   private:
    const WorldGenSettings &settings;
    IAssets *assets;
    std::mt19937 rng;
    std::uniform_real_distribution<float> dist{0.0f, 1.0f};
    PerlinNoise rockNoise;

    bool isRockAllowed(const Rock &rock, const Region &r,
                       bool metamorphic = false) {
        if (!metamorphic && rock.category == RockType::METAMORPHIC)
            return false;

        if (!rock.placement.allow_underwater && r.height < 0.5f) return false;

        if (!rock.placement.allowed_land.empty()) {
            if (std::find(rock.placement.allowed_land.begin(),
                          rock.placement.allowed_land.end(),
                          r.land) == rock.placement.allowed_land.end())
                return false;
        }

        if (!rock.placement.allowed_crust.empty()) {
            if (std::find(rock.placement.allowed_crust.begin(),
                          rock.placement.allowed_crust.end(),
                          r.crust) == rock.placement.allowed_crust.end())
                return false;
        }

        for (auto f : rock.placement.required_features)
            if (!r.hasFeature(f)) return false;

        for (auto f : rock.placement.forbidden_features)
            if (r.hasFeature(f)) return false;

        return true;
    }

    const Rock *pickRock(const std::vector<const Rock *> &candidates,
                         float field, float rnd) {
        auto computeWeight = [](const Rock *rock, float field) {
            float base = rock->placement.weight;

            float rockBias =
                std::hash<std::string>{}(rock->name) % 1000 / 1000.0f;

            float d = std::abs(field - rockBias);

            // gaussian-like falloff
            float spatial = std::exp(-d * 8.0f);

            return base * spatial;
        };
        float total = 0.0f;

        for (auto r : candidates) total += computeWeight(r, field);

        float acc = 0.0f;

        for (auto r : candidates) {
            acc += computeWeight(r, field);
            if (rnd <= acc / total) return r;
        }

        return candidates.empty() ? nullptr : candidates.back();
    }

    bool canFollow(const Rock *prev, const Rock *next) {
        if (!prev) return true;

        // intrusive should not appear above sedimentary
        if (prev->category == RockType::SEDIMENTARY &&
            next->category == RockType::IGNEOUS_INTRUSIVE)
            return false;

        return true;
    }
};