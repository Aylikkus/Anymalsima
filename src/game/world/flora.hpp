#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <memory>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "items/flora.hpp"
#include "soil_generator.hpp"

// One plant (docs/handoff/flora.md, section 4). Plants are not blocks: they
// live in a per-chunk FloraLayer owned by ChunkManager, in the chunk of
// their anchor (the solid tile they grow on).
struct FloraInstance {
    static constexpr uint8_t Harvested = 1 << 0;
    static constexpr uint8_t Damaged = 1 << 1;

    uint8_t species = 0;  // Flora id
    uint8_t flags = 0;
    uint16_t seed = 0;  // drives every per-instance range and variation
    // Chunk-local base position in 1/16 tile (the footprint centre of large
    // plants); the anchor tile is (x / 16, y, z / 16).
    uint8_t x = 0, z = 0;
    uint8_t y = 0;           // chunk-local anchor tile Y
    uint8_t rotation = 0;    // yaw in 1/256 turns
    int32_t birthDay = 0;    // game day of germination (negative: before day 0)
    int32_t harvestDay = 0;  // day of the last harvest (with Harvested)

    int tileX() const { return x / 16; }
    int tileZ() const { return z / 16; }
    float localX() const { return (x + 0.5f) / 16.f; }
    float localZ() const { return (z + 0.5f) / 16.f; }
    float yaw() const { return rotation * (6.2831853f / 256.f); }
    // Non-uniform scale factor from the seed, in [0.85, 1.15].
    float scale() const { return 0.85f + 0.3f * float(seed >> 8) / 255.f; }
    bool operator==(const FloraInstance&) const = default;
};
static_assert(sizeof(FloraInstance) == 16, "FloraInstance is saved");

struct FloraLayer {
    std::vector<FloraInstance> instances;
    // Last game day the layer was advanced to (FloraSimulation): spreading
    // counts seeding cycles from here.
    int32_t resolvedDay = 0;
    bool empty() const { return instances.empty(); }
    bool operator==(const FloraLayer&) const = default;
};
using FloraLayerPtr = std::shared_ptr<const FloraLayer>;

// Per chunk: the game day its tree wood was last brought up to date and the
// tree generator version that made it (docs/handoff/trees.md). Saved in the
// header of the chunk's FLOR blob. Never: no tree wood placed yet (chunks
// saved before trees existed get whole trees once).
struct TreeStamp {
    static constexpr int32_t Never = INT32_MIN;
    int32_t day = Never;
    uint16_t version = 0;
    bool placed() const { return day != Never; }
    bool operator==(const TreeStamp&) const = default;
};

// Analytic growth: a plant's stage is a pure function of its age, so
// unloaded areas need no ticking (docs/handoff/flora.md, section 4).
struct FloraStageState {
    FloraStage stage = FloraStage::Mature;
    float progress = 0;  // 0..1 inside the stage
    bool alive = true;
};

namespace flora_growth {

inline double unit(uint16_t seed, int salt) {
    return SoilGenerator::unit(seed, salt, 0x6a09e667u);
}

// Duration in days of each stage the species has, for this plant.
inline std::array<double, FloraStageCount> durations(const Flora& f,
                                                     uint16_t seed) {
    std::array<double, FloraStageCount> out{};
    for (int s = 0; s < FloraStageCount; ++s)
        if (const auto& range = f.growth.stages[s])
            out[s] = std::max(0.0, double(range->sample(unit(seed, s))));
    return out;
}

inline double lifespan(const Flora& f, uint16_t seed) {
    return std::max(1.0, double(f.growth.lifespan_days.sample(unit(seed, 17))));
}

// Stage at `age` days. Annuals pass every stage once and die after
// withering. Perennials grow up once, then cycle through mature, flowering
// and fruiting, and wither at the end of their lifespan. A negative age is a
// seed that has not germinated yet (not alive, nothing to draw).
inline FloraStageState at(const Flora& f, uint16_t seed, double age) {
    const auto days = durations(f, seed);
    const auto stage = [](int s) { return FloraStage(s); };
    if (age < 0) return {FloraStage::Seedling, 0, false};
    double total = 0;
    for (double d : days) total += d;
    if (total <= 0) return {FloraStage::Mature, 0, true};
    const int seedling = int(FloraStage::Seedling);
    const int young = int(FloraStage::Young);
    const int withering = int(FloraStage::Withering);
    if (!f.growth.perennial) {
        double t = age;
        for (int s = 0; s < FloraStageCount; ++s) {
            if (days[s] <= 0) continue;
            if (t < days[s]) return {stage(s), float(t / days[s]), true};
            t -= days[s];
        }
        return {FloraStage::Withering, 1, false};
    }
    const double life = lifespan(f, seed);
    if (age >= life) {
        const double wither = days[withering] > 0 ? days[withering] : 1;
        return {FloraStage::Withering,
                float(std::min(1.0, (age - life) / wither)),
                age < life + wither};
    }
    double t = age;
    for (int s : {seedling, young}) {
        if (days[s] <= 0) continue;
        if (t < days[s]) return {stage(s), float(t / days[s]), true};
        t -= days[s];
    }
    double cycle = 0;
    for (int s = int(FloraStage::Mature); s < withering; ++s) cycle += days[s];
    if (cycle <= 0) return {FloraStage::Mature, 0, true};
    t = std::fmod(t, cycle);
    for (int s = int(FloraStage::Mature); s < withering; ++s) {
        if (days[s] <= 0) continue;
        if (t < days[s]) return {stage(s), float(t / days[s]), true};
        t -= days[s];
    }
    return {FloraStage::Mature, 0, true};
}

// Days in one seeding cycle: an annual's whole life, a perennial's
// mature..fruiting loop. 0 when the species has no stages.
inline double cycle(const Flora& f, uint16_t seed) {
    const auto days = durations(f, seed);
    double total = 0;
    if (!f.growth.perennial) {
        for (double d : days) total += d;
        return total;
    }
    for (int s = int(FloraStage::Mature); s < int(FloraStage::Withering); ++s)
        total += days[s];
    return total;
}

// Age when the young plant first reaches maturity (start of its cycles).
inline double juvenile(const Flora& f, uint16_t seed) {
    const auto days = durations(f, seed);
    return f.growth.perennial
               ? days[int(FloraStage::Seedling)] + days[int(FloraStage::Young)]
               : 0;
}

// Age at which the plant is gone (after withering).
inline double deathAge(const Flora& f, uint16_t seed) {
    const auto days = durations(f, seed);
    if (!f.growth.perennial) {
        double total = 0;
        for (double d : days) total += d;
        return std::max(1.0, total);
    }
    const double wither = days[int(FloraStage::Withering)] > 0
                              ? days[int(FloraStage::Withering)]
                              : 1;
    return lifespan(f, seed) + wither;
}

inline uint16_t nextSeed(uint16_t seed, uint32_t salt = 0) {
    return uint16_t(unit(seed, int(0x5bd1e995u ^ salt)) * 65536.0);
}

// Days from the start of a seeding cycle (birth for annuals, maturity for
// perennials) to the moment seeds drop: the end of spread_stage.
inline double seedDrop(const Flora& f, uint16_t seed) {
    const auto days = durations(f, seed);
    int stage = -1;
    if (f.growth.spread_stage) {
        stage = int(*f.growth.spread_stage);
    } else {
        for (const auto s : {FloraStage::Fruiting, FloraStage::Flowering})
            if (days[int(s)] > 0) {
                stage = int(s);
                break;
            }
        // Otherwise the last growing stage.
        for (int s = int(FloraStage::Withering) - 1; stage < 0 && s >= 0; --s)
            if (days[s] > 0) stage = s;
    }
    if (stage < 0) return 0;
    const int start = f.growth.perennial ? int(FloraStage::Mature) : 0;
    double offset = 0;
    for (int s = start; s <= stage; ++s) offset += days[s];
    return offset;
}

// Expected seedlings one plant leaves over its life where nothing crowds
// it (the reproduction number). Above 1 a population grows until crowding
// (FloraSimulation) holds it at the species' density; below 1 it dies out.
inline double replacementRatio(const Flora& f, uint16_t seed = 0) {
    const auto& g = f.growth;
    if (g.spread_radius_m <= 0 || g.spread_chance_per_cycle <= 0) return 0;
    const double length = cycle(f, seed);
    if (length <= 0) return 0;
    double cycles = 1;
    if (g.perennial)
        cycles =
            std::max(0.0, (lifespan(f, seed) - juvenile(f, seed)) / length);
    const double seeds = (g.spread_seeds.min + g.spread_seeds.max) / 2.0;
    return cycles * g.spread_chance_per_cycle * seeds * g.spread_germination;
}

}  // namespace flora_growth
