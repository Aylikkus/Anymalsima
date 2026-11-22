#include "generated_hydrology.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <climits>
#include <deque>
#include <queue>
#include <stdexcept>
#include <thread>

#include "terrain_sampler.hpp"

namespace {
constexpr int SeaY = WaterSurface::SeaBoundary;
constexpr std::array<Vec2i, 4> Directions{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

int groundAt(const TerrainNeighborhood& terrain, Vec2i tile) {
    return int(std::floor(TerrainSampler(terrain).surface(tile.x, tile.y)));
}

bool waterLand(LandType land) {
    return land == LandType::DEEP_WATER || land == LandType::WATER ||
           land == LandType::SHORE;
}

bool stopped(const std::function<bool()>& cancelled) {
    return cancelled && cancelled();
}
}  // namespace

std::shared_ptr<const GeneratedHydrology> GeneratedHydrology::build(
    const World& world, const std::function<bool()>& cancelled,
    bool carveRivers, bool naturalRivers, bool boundRivers, bool riverSediment,
    bool connectLakeOutlets, bool connectCoastalBasins,
    bool oceanAcrossBasins) {
    if (world.width <= 0 || world.height <= 0 ||
        world.regions.size() != size_t(world.width) * world.height)
        throw std::invalid_argument("Invalid hydrology world");

    auto result = std::make_shared<GeneratedHydrology>();
    result->width = world.width;
    result->height = world.height;
    result->naturalProfiles = naturalRivers;
    result->boundedBanks = boundRivers;
    result->riverSediment = riverSediment;
    result->connectCoastalBasins = connectCoastalBasins;
    result->oceanAcrossBasins = oceanAcrossBasins;
    result->heightSeed = world.genSettings.height_seed;
    result->featureSeed = world.genSettings.feature_seed;
    result->ground.reserve(world.regions.size());
    result->regions.resize(world.regions.size());
    for (const auto& region : world.regions)
        result->ground.push_back(
            {std::clamp(region.land_height_m, 0, WORLD_HEIGHT_Y - 1),
             region.land, region.features});
    if (!result->buildOcean(cancelled) || !result->buildBasins(cancelled) ||
        !result->buildRiverBeds(world, cancelled) ||
        (carveRivers &&
         !result->buildReaches(cancelled, naturalRivers, connectLakeOutlets)))
        return {};
    result->reachLakeMasks.clear();
    result->reachLakeMasks.rehash(0);
    result->reachLakeMaskBytes = 0;
    return result;
}

int GeneratedHydrology::regionAt(int x, int z) const {
    if (z < 0 || z >= height * Side) return -1;
    return z / Side * width + int(WorldWrap::mod(x, width * Side)) / Side;
}

Vec2i GeneratedHydrology::center(int region) const {
    return {region % width * Side + Side / 2, region / width * Side + Side / 2};
}

std::array<int, 9> GeneratedHydrology::shoreNeighborhood(int region) const {
    const auto c = center(region);
    std::array<int, 9> result;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            result[(dz + 1) * 3 + dx + 1] =
                regionAt(c.x + dx * Side, c.y + dz * Side);
    return result;
}

int GeneratedHydrology::lakeSeedAt(int region, Vec2i tile) const {
    const int shore = regions[region].shore;
    if (shore != SharedShore) return shore;
    // A shore region can border several basins. Partition it by distance
    // to their core regions, with a stable tie break. Water still needs a
    // surveyed path from its seed on its own side of the partition.
    const int x = int(WorldWrap::mod(tile.x, Side)), z = tile.y % Side;
    const std::array<int, 3> dx{x + 1, 0, Side - x};
    const std::array<int, 3> dz{z + 1, 0, Side - z};
    const auto adjacent = shoreNeighborhood(region);
    auto nearest = std::pair{INT_MAX, -1};
    for (size_t k = 0; k < adjacent.size(); ++k) {
        const int i = adjacent[k];
        if (i < 0 || regions[i].body <= Sea) continue;
        const int distance = dx[k % 3] * dx[k % 3] + dz[k / 3] * dz[k / 3];
        nearest =
            std::min(nearest, std::pair{distance, int(regions[i].body) - 2});
    }
    return nearest.second;
}

std::array<int, 4> GeneratedHydrology::neighbors(int region) const {
    const int x = region % width, row = region / width * width;
    return {row + (x + width - 1) % width, row + (x + 1) % width,
            row > 0 ? region - width : -1,
            row < (height - 1) * width ? region + width : -1};
}

TerrainNeighborhood GeneratedHydrology::terrain(int region) const {
    TerrainNeighborhood data;
    data.center = {unsigned(region % width), unsigned(region / width)};
    data.width = width;
    data.height = height;
    data.height_seed = heightSeed;
    data.feature_seed = featureSeed;
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            const auto& source =
                ground[std::clamp(int(data.center.y) + dz, 0, height - 1) *
                           width +
                       WorldWrap::mod(int(data.center.x) + dx, width)];
            auto& target = data.regions[(dz + 2) * 5 + dx + 2].region;
            target.land_height_m = source.y;
            target.features = source.features;
            target.land = source.land;
        }
    return data;
}

bool GeneratedHydrology::buildOcean(const std::function<bool()>& cancelled) {
    const auto candidate = [&](int i) {
        return ground[i].y < SEA_LEVEL_Y && waterLand(ground[i].land);
    };
    std::deque<int> pending;
    for (int row : {0, height - 1})
        for (int x = 0; x < width; ++x) {
            const int i = row * width + x;
            if (candidate(i) && !regions[i].ocean) {
                regions[i].ocean = true;
                pending.push_back(i);
            }
        }
    while (!pending.empty()) {
        if (stopped(cancelled)) return false;
        const int i = pending.front();
        pending.pop_front();
        for (int next : neighbors(i)) {
            if (next < 0 || regions[next].ocean || !candidate(next)) continue;
            regions[next].ocean = true;
            pending.push_back(next);
        }
    }

    // An all-water 3x3 neighborhood stays submerged under the terrain
    // sampler's bounded coast warp. Every other center link needs an actual
    // face-connected path. Check the centerline as a conservative witness.
    for (int i = 0; i < int(regions.size()); ++i) {
        if (!regions[i].ocean) continue;
        const auto c = center(i);
        bool deep = true;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int j =
                    regionAt(c.x + dx * Side,
                             std::clamp(c.y + dz * Side, 0, height * Side - 1));
                deep &= ground[j].y < SEA_LEVEL_Y;
            }
        regions[i].deep = deep;
    }
    const auto line = [&](int i, Vec2i a, Vec2i dir, int steps) {
        if (regions[i].deep) return true;
        const auto data = terrain(i);
        for (int n = 0; n <= steps; ++n) {
            if (groundAt(data, {a.x + n * dir.x, a.y + n * dir.y}) >=
                SEA_LEVEL_Y)
                return false;
        }
        return true;
    };
    const auto certify = [&](int i) {
        regions[i].oceanSource = true;
        pending.push_back(i);
    };
    for (int row : {0, height - 1})
        for (int x = 0; x < width; ++x) {
            if (stopped(cancelled)) return false;
            const int i = row * width + x;
            if (regions[i].ocean && !regions[i].oceanSource &&
                line(i, center(i), {0, row == 0 ? -1 : 1},
                     row == 0 ? Side / 2 : Side / 2 - 1))
                certify(i);
        }
    while (!pending.empty()) {
        if (stopped(cancelled)) return false;
        const int i = pending.front();
        pending.pop_front();
        const auto adjacent = neighbors(i);
        for (size_t d = 0; d < adjacent.size(); ++d) {
            const int next = adjacent[d];
            if (next < 0 || !regions[next].ocean || regions[next].oceanSource)
                continue;
            const auto dir = Directions[d];
            const bool positive = dir.x + dir.y > 0;
            if (line(i, center(i), dir, Side / 2 - positive) &&
                line(next, center(next), {-dir.x, -dir.y},
                     Side / 2 - !positive))
                certify(next);
        }
    }
    for (int i = 0; i < int(regions.size()); ++i) {
        if (!regions[i].ocean) continue;
        const auto c = center(i);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int j = regionAt(c.x + dx * Side, c.y + dz * Side);
                if (j >= 0) regions[j].nearOcean = true;
            }
    }
    return true;
}

bool GeneratedHydrology::buildBasins(const std::function<bool()>& cancelled) {
    // Priority flood finds the minimum regional escape height. Stable queue
    // ordering also fixes the parent used for tied outlet saddles.
    const int count = int(regions.size());
    std::vector<int16_t> spill(count, -1);
    std::vector<int> parent(count, -1);
    std::vector<int> order;
    order.reserve(count);
    using Node = std::pair<int, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> frontier;
    const auto insert = [&](int i, int y, int from) {
        spill[i] = int16_t(y);
        parent[i] = from;
        frontier.emplace(y, i);
    };
    for (int i = 0; i < count; ++i) {
        if (regions[i].oceanSource) {
            regions[i].body = Sea;
            insert(i, SeaY, -1);
        } else if (i / width == 0 || i / width == height - 1) {
            insert(i, ground[i].y + 1, -1);
        }
    }
    while (!frontier.empty()) {
        if (stopped(cancelled)) return false;
        const auto [y, i] = frontier.top();
        frontier.pop();
        order.push_back(i);
        for (int next : neighbors(i))
            if (next >= 0 && spill[next] < 0)
                insert(next, std::max(y, ground[next].y + 1), i);
    }

    std::vector<bool> seen(count);
    std::vector<int> component;
    const auto depressed = [&](int i) {
        return !regions[i].body && ground[i].y + 1 < spill[i];
    };
    for (int i = 0; i < count; ++i) {
        if (stopped(cancelled)) return false;
        if (seen[i] || !depressed(i)) continue;
        component.clear();
        component.push_back(i);
        seen[i] = true;
        int seed = -1;
        int outlet = -1;
        for (size_t k = 0; k < component.size(); ++k) {
            const int j = component[k];
            // Sub-sea dry map terrain supplies no standing-water source.
            if ((ground[j].y >= SEA_LEVEL_Y || waterLand(ground[j].land)) &&
                (seed < 0 || Node{ground[j].y, j} < Node{ground[seed].y, seed}))
                seed = j;
            const int from = parent[j];
            if (from >= 0 && (!depressed(from) || spill[from] != spill[i]) &&
                (outlet < 0 || from < outlet))
                outlet = from;
            for (int next : neighbors(j)) {
                if (next < 0 || seen[next] || spill[next] != spill[i] ||
                    !depressed(next))
                    continue;
                seen[next] = true;
                component.push_back(next);
            }
        }
        if (seed < 0) continue;
        const auto id = uint32_t(seed) + 2;
        const bool coastal = connectCoastalBasins &&
                             ground[seed].y < SEA_LEVEL_Y &&
                             std::any_of(
                                 component.begin(), component.end(),
                                 [&](int j) { return regions[j].nearOcean; });
        basins.emplace(id, Basin{id, seed, outlet, spill[i], coastal});
        for (int j : component) {
            regions[j].body = id;
            regions[j].shore = seed;
        }
    }
    // The height sampler shares corner heights with diagonal neighbors.
    // Include them in the search area, but admit tiles through faces only.
    for (int i = 0; i < count; ++i) {
        if (regions[i].body <= Sea) continue;
        const int seed = regions[i].shore;
        for (int next : shoreNeighborhood(i)) {
            if (next < 0 || regions[next].ocean || regions[next].body) continue;
            auto& shore = regions[next].shore;
            if (shore == -1)
                shore = seed;
            else if (shore != seed)
                shore = SharedShore;
        }
    }

    // Follow downhill into a body where possible, otherwise follow the
    // priority-flood outlet. The rank provides deterministic flat drainage.
    std::vector<int> rank(count);
    for (size_t i = 0; i < order.size(); ++i) rank[order[i]] = int(i);
    for (int i : order) {
        auto& region = regions[i];
        if (region.body) {
            region.catchment = region.body;
            continue;
        }
        int receiver = parent[i];
        for (int next : neighbors(i)) {
            if (next < 0 || ground[next].y >= ground[i].y ||
                (!regions[next].body && rank[next] >= rank[i]))
                continue;
            if (receiver < 0 ||
                Node{ground[next].y, next} < Node{ground[receiver].y, receiver})
                receiver = next;
        }
        if (receiver >= 0)
            region.catchment = regions[receiver].body
                                   ? regions[receiver].body
                                   : regions[receiver].catchment;
    }
    return true;
}

bool GeneratedHydrology::buildRiverBeds(
    const World& world, const std::function<bool()>& cancelled) {
    beds.reserve(world.rivers.size());
    int last = -1;
    TerrainNeighborhood data;
    for (const auto& river : world.rivers) {
        if (stopped(cancelled)) return false;
        auto& bed = beds.emplace_back();
        bed.reserve(river.path.size());
        for (const auto& [point, strength] : river.path) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
            // Preview integers represent region centers.
            const int x = int(
                WorldWrap::modf((double(point.x) + .5) * Side, width * Side));
            const int z = int(std::clamp((double(point.y) + .5) * Side, 0.,
                                         double(height * Side - 1)));
            const int i = regionAt(x, z);
            if (i != last) {
                data = terrain(i);
                last = i;
            }
            bed.push_back(
                {{x, z}, groundAt(data, {x, z}), catchment(i), strength});
        }
    }
    return true;
}

GeneratedHydrology::Footprint GeneratedHydrology::refineLake(
    uint32_t body) const {
    const auto& basin = basins.at(body);
    if (basin.coastal) {
        auto coast = refineCoastalBasin(basin);
        if (coast.ocean) return coast;
    }
    Footprint result;
    result.lake.sillY = basin.sillY;
    struct SearchRegion {
        explicit SearchRegion(TerrainNeighborhood data)
            : data(std::move(data)) {
            levels.fill(-1);
        }
        TerrainNeighborhood data;
        std::array<int16_t, Area> levels;
        std::array<int16_t, Area> floors;
    };
    std::unordered_map<int, SearchRegion> search;
    std::unordered_map<int, TerrainNeighborhood> exterior;
    const auto outsideGround = [&](int region, Vec2i tile) {
        auto it = exterior.find(region);
        if (it == exterior.end())
            it = exterior.emplace(region, terrain(region)).first;
        return groundAt(it->second, tile);
    };
    std::array<std::vector<Vec2i>, WORLD_HEIGHT_Y + 1> frontier;
    const auto index = [](Vec2i p) { return p.y % Side * Side + p.x % Side; };
    std::unordered_map<int, std::shared_ptr<const Footprint>> ocean;
    const auto reachesSea = [&](int region, Vec2i tile) {
        if (!oceanAcrossBasins || !canSurveyOcean(regions[region]))
            return false;
        auto& survey = ocean[region];
        if (!survey) survey = footprint(uint64_t(region) << 1);
        return survey->masks.at(region).test(index(tile));
    };
    const auto seed = center(basin.seed);
    auto& start =
        search.try_emplace(basin.seed, terrain(basin.seed)).first->second;
    const int minimum = groundAt(start.data, seed) + 2;
    if (minimum > basin.sillY ||
        (minimum <= SeaY && reachesSea(basin.seed, seed)))
        return {};
    start.levels[index(seed)] = int16_t(minimum);
    start.floors[index(seed)] = int16_t(minimum - 1);
    frontier[minimum].push_back(seed);
    const int period = width * Side;
    int searchLimit = basin.sillY;
    for (int level = minimum; level <= searchLimit; ++level) {
        auto& bucket = frontier[level];
        while (!bucket.empty() && level <= searchLimit) {
            const auto tile = bucket.back();
            bucket.pop_back();
            for (const auto dir : Directions) {
                const Vec2i next{int(WorldWrap::mod(tile.x + dir.x, period)),
                                 tile.y + dir.y};
                const int i = regionAt(next.x, next.y);
                if (i < 0) {
                    searchLimit = level - 1;
                    result.lake.sillY = searchLimit;
                    result.lake.outlet = tile;
                    break;
                }
                if (lakeSeedAt(i, next) != basin.seed ||
                    regions[i].oceanSource) {
                    // A cell can spill only through a face with an occupied
                    // tile on both sides. Cap at the boundary below that tile.
                    const int escape =
                        std::max(level, outsideGround(i, next) + 2);
                    if (escape <= searchLimit) {
                        searchLimit = escape - 1;
                        result.lake.sillY = searchLimit;
                        result.lake.outlet = next;
                    }
                    continue;
                }
                auto it = search.find(i);
                if (it == search.end()) {
                    if (search.size() >= MaxLakeRegions) {
                        // The work limit leaves this connection unknown.
                        // Keep only the fully surveyed lower levels.
                        const int threshold =
                            std::max(level, outsideGround(i, next) + 2);
                        searchLimit = std::min(searchLimit, threshold - 1);
                        continue;
                    }
                    it = search.try_emplace(i, terrain(i)).first;
                }
                auto& visited = it->second.levels[index(next)];
                if (visited >= 0) continue;
                const int bottom = groundAt(it->second.data, next) + 2;
                const int threshold = std::max(level, bottom);
                visited = int16_t(threshold);
                it->second.floors[index(next)] = int16_t(bottom - 1);
                if (threshold > searchLimit) continue;
                // One coarse region can hold both a lake and a separate
                // piece of coast. The surveyed sea is also an open spill.
                if (bottom <= SeaY && reachesSea(i, next)) {
                    searchLimit = threshold - 1;
                    result.lake.sillY = searchLimit;
                    result.lake.outlet = next;
                } else {
                    frontier[threshold].push_back(next);
                }
            }
        }
    }
    StorageCurve::Levels area{}, initial{};
    for (const auto& [i, cells] : search) {
        for (int j = 0; j < Area; ++j) {
            const int level = cells.levels[j];
            if (level < minimum || level > searchLimit) continue;
            result.masks[i].set(j);
            auto& thresholds = result.thresholds[i];
            if (thresholds.empty()) thresholds.resize(Area, -1);
            thresholds[j] = int16_t(level);
            const int floor = cells.floors[j];
            ++area[level];
            initial[level] += level - floor - 1;
        }
    }
    WaterVolume wetArea = 0;
    for (int y = 1; y <= WORLD_HEIGHT_Y; ++y) {
        result.storage[y] = result.storage[y - 1];
        if (y > searchLimit) continue;
        wetArea += area[y];
        result.storage[y] += (wetArea + initial[y]) * WaterUnitsPerBlock;
    }
    if (!result.masks.empty()) result.lake.surfaceY = searchLimit;
    return result;
}

GeneratedHydrology::Footprint GeneratedHydrology::refineCoastalBasin(
    const Basin& basin) const {
    // Follow the basin's original submerged terrain to a surveyed ocean
    // tile. Using uncarved ground keeps nearby river cuts from flooding it.
    struct Cells {
        TerrainNeighborhood terrain;
        Mask seen;
    };
    struct Coast {
        TerrainNeighborhood terrain;
        std::shared_ptr<const Footprint> survey;
    };
    Footprint result;
    const int period = width * Side;
    std::unordered_map<int, Cells> surveyed;
    std::unordered_map<int, Coast> coast;
    std::vector<Vec2i> wet;
    bool exceededBudget = false;
    const auto admit = [&](Vec2i tile) {
        const int i = regionAt(tile.x, tile.y);
        if (i < 0) return;
        const auto& region = regions[i];
        const int index = tile.y % Side * Side + tile.x % Side;
        if (!result.ocean && canSurveyOcean(region)) {
            if (region.deep && region.oceanSource) {
                result.ocean = true;
            } else {
                auto it = coast.find(i);
                if (it == coast.end())
                    it = coast.emplace(i, Coast{terrain(i), {}}).first;
                auto& candidate = it->second;
                // A dry boundary cannot justify a full ocean survey.
                if (!candidate.survey &&
                    TerrainSampler(candidate.terrain).belowSea(tile.x, tile.y))
                    candidate.survey = footprint(uint64_t(i) << 1);
                if (candidate.survey)
                    result.ocean = candidate.survey->masks.at(i).test(index);
            }
        }
        if (region.oceanSource || lakeSeedAt(i, tile) != basin.seed) return;
        auto it = surveyed.find(i);
        if (it == surveyed.end()) {
            if (surveyed.size() == MaxLakeRegions) {
                exceededBudget = true;
                return;
            }
            it = surveyed.emplace(i, Cells{terrain(i), {}}).first;
        }
        auto& cells = it->second;
        if (cells.seen.test(index)) return;
        cells.seen.set(index);
        if (!TerrainSampler(cells.terrain).belowSea(tile.x, tile.y)) return;
        result.masks[i].set(index);
        wet.push_back(tile);
    };
    admit(center(basin.seed));
    for (size_t k = 0; k < wet.size() && !exceededBudget; ++k) {
        const auto tile = wet[k];
        for (const auto dir : Directions)
            admit(
                {int(WorldWrap::mod(tile.x + dir.x, period)), tile.y + dir.y});
    }
    if (!result.ocean || exceededBudget) return {};
    result.lake = {SeaY, SeaY, {}};
    return result;
}

GeneratedHydrology::Footprint GeneratedHydrology::refineOcean(int region,
                                                              int level) const {
    const auto c = center(region);
    const int period = width * Side;
    std::unordered_map<int, TerrainNeighborhood> neighborhoods;
    std::unordered_map<int, std::bitset<Area>> seen;
    std::vector<Vec2i> wet;
    Footprint result;
    Mask& mask = result.masks[region];
    int admitted = 0;
    const bool lowered = level < SeaY;
    const auto* thresholds = lowered ? &seaThresholds() : nullptr;
    const auto admit = [&](Vec2i tile, int i) {
        if (!canCrossOcean(regions[i])) return;
        auto& checked = seen[i];
        const int index = tile.y % Side * Side + tile.x % Side;
        if (checked.test(index)) return;
        checked.set(index);
        if (lowered ? (*thresholds)[i] > level : !regions[i].deep) {
            auto it = neighborhoods.find(i);
            if (it == neighborhoods.end())
                it = neighborhoods.emplace(i, terrain(i)).first;
            if (lowered ? groundAt(it->second, tile) + 2 > level
                        : !TerrainSampler(it->second).belowSea(tile.x, tile.y))
                return;
        }
        wet.push_back(tile);
        if (i == region) {
            mask.set(index);
            ++admitted;
        }
    };
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const int i = regionAt(c.x + dx * Side, c.y + dz * Side);
            if (i < 0 || !regions[i].ocean) continue;
            if (lowered ? (*thresholds)[i] <= level : regions[i].oceanSource)
                admit(center(i), i);
            const int row = i / width;
            if (row == 0 || row == height - 1)
                for (int x = i % width * Side; x < (i % width + 1) * Side; ++x)
                    admit({x, row == 0 ? 0 : height * Side - 1}, i);
        }
    for (size_t k = 0; k < wet.size() && admitted < Area; ++k) {
        const auto tile = wet[k];
        for (const auto dir : Directions) {
            const Vec2i next{int(WorldWrap::mod(tile.x + dir.x, period)),
                             tile.y + dir.y};
            const int i = regionAt(next.x, next.y);
            if (i >= 0 &&
                std::abs(WorldWrap::delta(i % width, region % width, width)) <=
                    1 &&
                std::abs(i / width - region / width) <= 1)
                admit(next, i);
        }
    }
    return result;
}

bool GeneratedHydrology::oceanConnectedAt(int region, Vec2i tile) const {
    // River derivation runs before the hydrology snapshot is published.
    // Reuse a full survey if an earlier outlet already needed one.
    const auto key = uint64_t(region) << 1;
    const int index = tile.y % Side * Side + tile.x % Side;
    const auto surveyed = [&] {
        return footprint(key)->masks.at(region).test(index);
    };
    if (cache.contains(key)) return surveyed();

    // Search from the mouth toward the same sources used by refineOcean.
    // A matching path proves membership without filling the whole window.
    const auto adjacent = shoreNeighborhood(region);
    const int period = width * Side;
    const int rx = region % width, rz = region / width;
    struct Source {
        Vec2i center;
        int openZ;
        bool certified;
    };
    std::vector<Source> sources;
    for (const int i : adjacent) {
        if (i < 0 || !regions[i].ocean || !canCrossOcean(regions[i])) continue;
        const int row = i / width;
        const int openZ = row == 0            ? 0
                          : row == height - 1 ? height * Side - 1
                                              : -1;
        if (regions[i].oceanSource || openZ >= 0)
            sources.push_back({center(i), openZ, regions[i].oceanSource});
    }
    if (sources.empty()) return false;

    std::array<std::bitset<Area>, 9> seen;
    std::unordered_map<int, TerrainNeighborhood> neighborhoods;
    struct Visit {
        int distance;
        Vec2i tile;
        bool operator>(const Visit& other) const {
            return distance > other.distance;
        }
    };
    std::priority_queue<Visit, std::vector<Visit>, std::greater<>> pending;
    const auto enqueue = [&](Vec2i p) {
        const int i = regionAt(p.x, p.y);
        if (i < 0 || !canCrossOcean(regions[i])) return;
        const int dx = int(WorldWrap::delta(i % width, rx, width));
        const int dz = i / width - rz;
        if (std::abs(dx) > 1 || std::abs(dz) > 1) return;
        auto& checked = seen[(dz + 1) * 3 + dx + 1];
        const int local = p.y % Side * Side + p.x % Side;
        if (checked.test(local)) return;
        checked.set(local);
        int distance = INT_MAX;
        for (const auto& source : sources) {
            const int x =
                int(std::abs(WorldWrap::delta(p.x, source.center.x, period)));
            if (source.certified)
                distance =
                    std::min(distance, x + std::abs(p.y - source.center.y));
            if (source.openZ >= 0)
                distance = std::min(distance, std::max(0, x - Side / 2) +
                                                  std::abs(p.y - source.openZ));
        }
        pending.push({distance, p});
    };
    enqueue(tile);
    constexpr int ProbeBudget = 1024;
    for (int count = 0; count < ProbeBudget && !pending.empty(); ++count) {
        const auto p = pending.top().tile;
        pending.pop();
        const int i = regionAt(p.x, p.y);
        const auto& r = regions[i];
        if (!r.deep && p != tile) {
            auto it = neighborhoods.find(i);
            if (it == neighborhoods.end())
                it = neighborhoods.emplace(i, terrain(i)).first;
            if (!TerrainSampler(it->second).belowSea(p.x, p.y)) continue;
        }
        const int row = i / width;
        if ((r.oceanSource && (r.deep || p == center(i))) ||
            (r.ocean &&
             (row == 0 ? p.y == 0
                       : row == height - 1 && p.y == height * Side - 1)))
            return true;
        for (const auto dir : Directions)
            enqueue({int(WorldWrap::mod(p.x + dir.x, period)), p.y + dir.y});
    }
    return !pending.empty() && surveyed();
}

void GeneratedHydrology::retainOutletLake(uint32_t body) {
    if (outletLakes.contains(body) || outletLakeBytes >= OutletLakeBudget)
        return;
    const auto lake = footprint((uint64_t(body) << 1) | 1);
    const size_t cost = sizeof(Footprint) + 128 +
                        lake->masks.size() * (sizeof(Mask) + 128) +
                        lake->thresholds.size() * Area * sizeof(int16_t);
    if (outletLakeBytes + cost > OutletLakeBudget) return;
    outletLakes.emplace(body, lake);
    outletLakeBytes += cost;
}

std::shared_ptr<const GeneratedHydrology::Footprint>
GeneratedHydrology::footprint(uint64_t key) const {
    if (key & 1)
        if (const auto it = outletLakes.find(uint32_t(key >> 1));
            it != outletLakes.end())
            return it->second;
    std::optional<std::promise<std::shared_ptr<const Footprint>>> producer;
    Cached value;
    {
        std::lock_guard lock(cacheMutex);
        const auto it = cache.find(key);
        const auto now = ++cacheClock;
        if (it != cache.end()) {
            it->second.lastUsed = now;
            value = it->second.value;
        } else {
            if (cache.size() >= CacheSize) {
                auto oldest = cache.end();
                for (auto entry = cache.begin(); entry != cache.end(); ++entry)
                    if ((oldest == cache.end() ||
                         entry->second.lastUsed < oldest->second.lastUsed) &&
                        entry->second.value.wait_for(std::chrono::seconds(0)) ==
                            std::future_status::ready)
                        oldest = entry;
                if (oldest != cache.end()) cache.erase(oldest);
            }
            producer.emplace();
            value = producer->get_future().share();
            if (cache.size() < CacheSize)
                cache.emplace(key, CacheEntry{value, now});
        }
    }
    if (producer) {
        try {
            producer->set_value(std::make_shared<const Footprint>(
                key & 1 ? refineLake(uint32_t(key >> 1))
                        : refineOcean(int(uint32_t(key) >> 1),
                                      key >> 32 ? int(key >> 32) : SeaY)));
        } catch (...) {
            producer->set_exception(std::current_exception());
        }
    }
    return value.get();
}

uint32_t GeneratedHydrology::candidateBody(int region) const {
    return regions[region].body;
}

std::vector<uint16_t> GeneratedHydrology::refinedLakeCoverage(
    const std::function<bool()>& cancelled) const {
    std::vector<uint32_t> ids;
    ids.reserve(basins.size());
    for (const auto& [id, basin] : basins) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::vector<std::atomic<uint32_t>> counts(regions.size());
    std::atomic<size_t> next = 0;
    std::atomic_bool stop = false;
    const auto work = [&] {
        for (size_t k; !stop && (k = next++) < ids.size();) {
            if (stopped(cancelled)) {
                stop = true;
                break;
            }
            const auto lake = footprint((uint64_t(ids[k]) << 1) | 1);
            for (const auto& [region, mask] : lake->masks) {
                uint32_t wet = 0;
                for (const auto word : mask.bits) wet += std::popcount(word);
                counts[region] += wet;
            }
        }
    };
    const unsigned threads =
        std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
    work();
    for (auto& thread : pool) thread.join();
    if (stop) return {};
    std::vector<uint16_t> result(regions.size());
    for (size_t i = 0; i < counts.size(); ++i)
        result[i] = uint16_t(std::min<uint32_t>(counts[i], Area));
    return result;
}

uint32_t GeneratedHydrology::catchment(int region) const {
    return regions[region].catchment;
}

const GeneratedHydrology::Basin* GeneratedHydrology::basin(uint32_t id) const {
    const auto it = basins.find(id);
    return it == basins.end() ? nullptr : &it->second;
}

GeneratedHydrology::Lake GeneratedHydrology::lake(uint32_t id) const {
    return basin(id) ? footprint((uint64_t(id) << 1) | 1)->lake : Lake{};
}

int GeneratedHydrology::ceiling(int region) const {
    const auto& r = regions[region];
    const auto channel = riverCeilings.find(region);
    int top = channel != riverCeilings.end() ? channel->second : -1;
    if (r.nearOcean) top = std::max(top, SEA_LEVEL_Y);
    const auto include = [&](const Basin& basin) {
        top = std::max(top, basin.sillY - 1);
        if (basin.coastal) top = std::max(top, SEA_LEVEL_Y);
    };
    if (r.shore == SharedShore) {
        for (int i : shoreNeighborhood(region))
            if (i >= 0)
                if (const auto* b = basin(regions[i].body)) include(*b);
    } else if (r.shore >= 0) {
        include(basins.at(uint32_t(r.shore) + 2));
    }
    return top;
}

bool GeneratedHydrology::needsRefinement(int x0, int z0, int x1, int z1) const {
    for (int z = z0; z < z1; z += Side - z % Side)
        for (int x = x0; x < x1; x += Side - WorldWrap::mod(x, Side)) {
            const int i = regionAt(x, z);
            if (i < 0) continue;
            const auto& r = regions[i];
            if (r.shore != -1 ||
                (canSurveyOcean(r) && !(r.deep && r.oceanSource)))
                return true;
        }
    return false;
}

int GeneratedHydrology::waterCeilingAt(int x, int z) const {
    const int i = regionAt(x, z);
    return i < 0 ? -1 : ceiling(i) + 1;
}

WaterSurface GeneratedHydrology::certifiedSeaAt(int x, int z) const {
    const int i = regionAt(x, z);
    if (i >= 0 && regions[i].deep && regions[i].oceanSource) return {SeaY, Sea};
    return {};
}

bool GeneratedHydrology::potentialSeaAt(int x, int z) const {
    const int i = regionAt(x, z);
    return i >= 0 && canSurveyOcean(regions[i]) &&
           !(regions[i].deep && regions[i].oceanSource);
}

WaterSurface GeneratedHydrology::previewWaterAt(int x, int z,
                                                int groundY) const {
    const auto channel = riverAt(x, z);
    const int i = regionAt(x, z);
    if (i < 0) return {};
    const auto& r = regions[i];
    WaterSurface standing;
    if ((r.deep && r.oceanSource) ||
        (groundY >= 0 && groundY < SEA_LEVEL_Y && canSurveyOcean(r)))
        standing = {SeaY, Sea};
    const auto ready = [&](uint32_t body) -> std::shared_ptr<const Footprint> {
        if (const auto it = outletLakes.find(body); it != outletLakes.end())
            return it->second;
        const uint64_t key = (uint64_t(body) << 1) | 1;
        Cached cached;
        {
            std::lock_guard lock(cacheMutex);
            const auto it = cache.find(key);
            if (it == cache.end()) return {};
            cached = it->second.value;
        }
        if (cached.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
            return {};
        return cached.get();
    };
    if (r.shore != -1) {
        const int seed = lakeSeedAt(i, {x, z});
        if (seed >= 0) {
            const uint32_t id = uint32_t(seed) + 2;
            if (const auto lake = ready(id)) {
                const auto mask = lake->masks.find(i);
                const int index =
                    z % Side * Side + int(WorldWrap::mod(x, Side));
                if (mask != lake->masks.end() && mask->second.test(index))
                    standing = {lake->lake.surfaceY, lake->ocean ? Sea : id};
            }
        }
    }
    if (channel && channel->distance <= channel->radius &&
        (!boundedBanks || !standing.wet()) &&
        channel->water.surfaceY > standing.surfaceY &&
        channel->water.depth(groundY) > 0 &&
        channel->water.depth(groundY) <= MaxRiverDepth)
        return channel->water;
    return standing;
}

WaterSurface GeneratedHydrology::waterAt(int x, int z, int groundY) const {
    const auto channel = riverAt(x, z);
    if (groundY < 0 && channel) return sampleRiverColumn(x, z).second;
    const auto standing = standingWaterAt(x, z, groundY);
    if (channel && channel->distance <= channel->radius &&
        (!boundedBanks || !standing.wet()) &&
        channel->water.surfaceY > standing.surfaceY &&
        channel->water.surfaceY > groundY + 1 &&
        channel->water.depth(groundY) <= MaxRiverDepth)
        return channel->water;
    return standing;
}

WaterSurface GeneratedHydrology::standingWaterAt(int x, int z, int groundY,
                                                 bool probeOutlet) const {
    const int i = regionAt(x, z);
    if (i < 0) return {};
    const auto& r = regions[i];
    if (r.deep && r.oceanSource) return {SeaY, Sea};
    const int index = z % Side * Side + int(WorldWrap::mod(x, Side));
    if (canSurveyOcean(r) && groundY < SEA_LEVEL_Y &&
        (probeOutlet ? oceanConnectedAt(i, {x, z})
                     : footprint(uint64_t(i) << 1)->masks.at(i).test(index)))
        return {SeaY, Sea};
    if (const int seed = lakeSeedAt(i, {x, z}); seed >= 0) {
        const uint32_t id = uint32_t(seed) + 2;
        const auto& basin = basins.at(id);
        if (groundY + 1 >= basin.sillY &&
            !(basin.coastal && groundY < SEA_LEVEL_Y))
            return {};
        const ReachLakeMask* compact = nullptr;
        std::shared_ptr<const Footprint> wet;
        if (probeOutlet) {
            const auto cached = reachLakeMasks.find(id);
            if (cached != reachLakeMasks.end()) compact = &cached->second;
        }
        if (!compact) {
            wet = footprint((uint64_t(id) << 1) | 1);
            if (probeOutlet) {
                const size_t bytes = sizeof(ReachLakeMask) +
                                     wet->masks.size() * (sizeof(Mask) + 64);
                if (bytes <= ReachLakeMaskBudget - reachLakeMaskBytes) {
                    auto [it, inserted] = reachLakeMasks.try_emplace(
                        id, ReachLakeMask{wet->lake.surfaceY, wet->ocean,
                                          wet->masks});
                    if (inserted) reachLakeMaskBytes += bytes;
                    compact = &it->second;
                }
            }
        }
        const auto& masks = compact ? compact->masks : wet->masks;
        if (const auto it = masks.find(i);
            it != masks.end() && it->second.test(index))
            return {compact ? compact->surfaceY : wet->lake.surfaceY,
                    (compact ? compact->ocean : wet->ocean) ? Sea : id};
    }
    return {};
}

void GeneratedHydrology::Mask::set(int index) {
    bits[index / WordSize] |= uint64_t(1) << (index % WordSize);
}

bool GeneratedHydrology::Mask::test(int index) const {
    return (bits[index / WordSize] >> (index % WordSize)) & 1;
}

bool GeneratedHydrology::Mask::matches(int x0, int z0, int x1, int z1,
                                       bool wet) const {
    for (int x = x0; x < x1;) {
        const int word = x / WordSize;
        const int next = std::min(x1, (word + 1) * WordSize);
        const uint64_t mask = (~uint64_t(0) << (x % WordSize)) &
                              (~uint64_t(0) >> ((word + 1) * WordSize - next));
        for (int z = z0; z < z1; ++z)
            if ((bits[z * (Side / WordSize) + word] & mask) != (wet ? mask : 0))
                return false;
        x = next;
    }
    return true;
}

bool GeneratedHydrology::uniformWater(int x0, int z0, int x1, int z1,
                                      uint32_t expectedBody,
                                      bool certifiedOnly) const {
    for (int z = z0; z < z1;) {
        const int depth = std::min(z1 - z, Side - z % Side);
        for (int x = x0; x < x1;) {
            const int localX = int(WorldWrap::mod(x, Side));
            const int span = std::min(x1 - x, Side - localX);
            const int i = regionAt(x, z);
            if (i < 0) return expectedBody == 0;
            const auto& r = regions[i];
            const auto matches = [&](const Mask& mask, bool wet) {
                return mask.matches(localX, z % Side, localX + span,
                                    z % Side + depth, wet);
            };
            if (r.deep && r.oceanSource) {
                if (expectedBody != Sea) return false;
            } else if (certifiedOnly) {
                if (expectedBody) return false;
            } else {
                bool seaMatches = false;
                if (canSurveyOcean(r)) {
                    const auto sea = footprint(uint64_t(i) << 1);
                    if (expectedBody == Sea)
                        seaMatches = matches(sea->masks.at(i), true);
                    else if (!matches(sea->masks.at(i), false))
                        return false;
                }
                if (expectedBody == Sea && !seaMatches) {
                    if (r.shore < 0) return false;
                    const auto& basin = basins.at(uint32_t(r.shore) + 2);
                    if (!basin.coastal) return false;
                    const auto coast = footprint((uint64_t(basin.id) << 1) | 1);
                    const auto mask = coast->masks.find(i);
                    if (!coast->ocean || mask == coast->masks.end() ||
                        !matches(mask->second, true))
                        return false;
                } else if (expectedBody != Sea) {
                    const auto lakeMatches = [&](uint32_t id) {
                        const auto lake = footprint((uint64_t(id) << 1) | 1);
                        const auto mask = lake->masks.find(i);
                        if (expectedBody == id && lake->ocean) return false;
                        const bool wet = expectedBody == id;
                        return mask == lake->masks.end()
                                   ? !wet
                                   : matches(mask->second, wet);
                    };
                    if (expectedBody) {
                        if (!basin(expectedBody) || !lakeMatches(expectedBody))
                            return false;
                    } else if (r.shore == SharedShore) {
                        std::array<uint32_t, 9> seen{};
                        size_t count = 0;
                        for (int next : shoreNeighborhood(i)) {
                            if (next < 0) continue;
                            const uint32_t id = regions[next].body;
                            if (id <= Sea ||
                                std::find(seen.begin(), seen.begin() + count,
                                          id) != seen.begin() + count)
                                continue;
                            if (!lakeMatches(id)) return false;
                            seen[count++] = id;
                        }
                    } else if (r.shore >= 0 &&
                               !lakeMatches(uint32_t(r.shore) + 2)) {
                        return false;
                    }
                }
            }
            x += span;
        }
        z += depth;
    }
    return true;
}

const std::vector<int16_t>& GeneratedHydrology::seaThresholds() const {
    std::call_once(seaThresholdsOnce, [&] {
        // Certify a connected coarse interior without scanning every tile.
        // Each deep region is below the maximum neighboring regional height,
        // or the shelf's seabed ceiling. Mixed coasts are surveyed on demand.
        std::vector<int16_t> bounds(regions.size(), SeaY);
        std::array<std::vector<int>, SeaY> frontier;
        oceanThreshold.assign(regions.size(), SeaY);
        for (int i = 0; i < int(regions.size()); ++i) {
            if (!regions[i].deep || !regions[i].oceanSource) continue;
            int depth = 0;
            const auto c = center(i);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int j = regionAt(
                        c.x + dx * Side,
                        std::clamp(c.y + dz * Side, 0, height * Side - 1));
                    depth = std::max(depth, ground[j].y + 2);
                }
            bounds[i] = int16_t(std::min(SeaY, depth));
            if (bounds[i] == SeaY) continue;
            if (i / width == 0 || i / width == height - 1) {
                oceanThreshold[i] = bounds[i];
                frontier[bounds[i]].push_back(i);
            }
        }
        for (int level = 0; level < SeaY; ++level)
            for (size_t k = 0; k < frontier[level].size(); ++k) {
                const int i = frontier[level][k];
                for (const int j : neighbors(i)) {
                    if (j < 0 || bounds[j] == SeaY) continue;
                    const int threshold = std::max<int>(bounds[j], level);
                    if (threshold >= oceanThreshold[j]) continue;
                    oceanThreshold[j] = int16_t(threshold);
                    frontier[threshold].push_back(j);
                }
            }
    });
    return oceanThreshold;
}

bool GeneratedHydrology::uniformLoweredSea(int x0, int z0, int x1, int z1,
                                           int level) const {
    if (level <= 0) return false;
    const auto& thresholds = seaThresholds();
    for (int z = z0; z < z1; z += Side - WorldWrap::mod(z, Side))
        for (int x = x0; x < x1; x += Side - WorldWrap::mod(x, Side)) {
            const int i = regionAt(x, z);
            if (i < 0 || thresholds[i] > level) return false;
        }
    return true;
}

StorageCurve GeneratedHydrology::storageCurve(uint32_t body) const {
    if (body != Sea) {
        if (basin(body))
            return StorageCurve(footprint((uint64_t(body) << 1) | 1)->storage);
        return StorageCurve({});
    }

    // Deep regions contribute their full area at the sampled regional depth.
    // Along the shelf, sample a regular lattice only in coastal candidates.
    // The open ocean dominates the budget. Small inland lakes use tile data.
    constexpr int stride = Side / 4;
    const auto& thresholds = seaThresholds();
    StorageCurve::Levels areas{}, initial{}, storage{};
    const auto admit = [&](int floor, int connection, WaterVolume weight) {
        const int first = std::max(floor + 1, connection);
        if (first > SeaY) return;
        areas[first] += weight;
        initial[first] += weight * (first - floor - 1);
    };
    for (int i = 0; i < int(regions.size()); ++i) {
        const auto& r = regions[i];
        if (r.deep && r.oceanSource) {
            const int floor = std::clamp(ground[i].y + 1, 1, SeaY - 1);
            admit(floor, thresholds[i], Area);
        } else if (r.oceanSource) {
            const auto local = terrain(i);
            const auto c = center(i);
            const int x0 = c.x - Side / 2, z0 = c.y - Side / 2;
            int link = SeaY;
            for (int n : neighbors(i))
                if (n >= 0) link = std::min<int>(link, thresholds[n]);
            // The coast samples approximate wetted area below the
            // region's certified connection at the original sea level.
            for (int z = stride / 2; z < Side; z += stride)
                for (int x = stride / 2; x < Side; x += stride)
                    admit(groundAt(local, {x0 + x, z0 + z}) + 1, link,
                          stride * stride);
        }
    }
    WaterVolume area = 0;
    for (int y = 1; y <= WORLD_HEIGHT_Y; ++y) {
        area += areas[y];
        storage[y] = storage[y - 1] +
                     (y <= SeaY ? (area + initial[y]) * WaterUnitsPerBlock : 0);
    }
    return StorageCurve(std::move(storage));
}

bool GeneratedHydrology::connectedAt(uint32_t body, int x, int z,
                                     int level) const {
    const int i = regionAt(x, z);
    if (body == Sea) {
        if (i < 0 || level <= 0) return false;
        if (seaThresholds()[i] <= level) return true;
        const auto survey = footprint(uint64_t(level) << 32 | uint64_t(i) << 1);
        return survey->masks.at(i).test(z % Side * Side +
                                        WorldWrap::mod(x, Side));
    }
    if (i < 0 || !basin(body)) return false;
    const auto wet = footprint((uint64_t(body) << 1) | 1);
    const auto it = wet->thresholds.find(i);
    if (it == wet->thresholds.end()) return false;
    const int threshold = it->second[z % Side * Side + WorldWrap::mod(x, Side)];
    return threshold >= 0 && threshold <= level;
}

bool GeneratedHydrology::mayContainBody(uint32_t body, int x0, int z0, int x1,
                                        int z1) const {
    for (int z = std::max(0, z0); z < z1; z += Side - z % Side)
        for (int x = x0; x < x1; x += Side - WorldWrap::mod(x, Side)) {
            const int i = regionAt(x, z);
            if (i < 0) continue;
            const auto& r = regions[i];
            if (body == Sea) {
                if (r.oceanSource || canSurveyOcean(r) ||
                    (connectCoastalBasins && r.shore != -1))
                    return true;
            } else if (r.shore == SharedShore) {
                for (const int next : shoreNeighborhood(i))
                    if (next >= 0 && regions[next].body == body) return true;
            } else if (r.shore >= 0 && uint32_t(r.shore) + 2 == body) {
                return true;
            }
        }
    return false;
}
