#include "generated_hydrology.hpp"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <climits>
#include <deque>
#include <queue>
#include <stdexcept>

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
    const World& world, const std::function<bool()>& cancelled) {
    if (world.width <= 0 || world.height <= 0 ||
        world.regions.size() != size_t(world.width) * world.height)
        throw std::invalid_argument("Invalid hydrology world");

    auto result = std::make_shared<GeneratedHydrology>();
    result->width = world.width;
    result->height = world.height;
    result->heightSeed = world.genSettings.height_seed;
    result->featureSeed = world.genSettings.feature_seed;
    result->ground.reserve(world.regions.size());
    result->regions.resize(world.regions.size());
    for (const auto& region : world.regions)
        result->ground.push_back(
            {std::clamp(region.land_height_m, 0, WORLD_HEIGHT_Y - 1),
             region.land, region.features});
    if (!result->buildOcean(cancelled) || !result->buildBasins(cancelled) ||
        !result->buildRiverBeds(world, cancelled))
        return {};
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
        basins.emplace(id, Basin{id, seed, outlet, spill[i]});
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
            bed.push_back({{x, z}, groundAt(data, {x, z}), catchment(i)});
        }
    }
    return true;
}

GeneratedHydrology::Footprint GeneratedHydrology::refineLake(
    uint32_t body) const {
    const auto& basin = basins.at(body);
    Footprint result;
    result.lake.sillY = basin.sillY;
    struct SearchRegion {
        explicit SearchRegion(TerrainNeighborhood data)
            : data(std::move(data)) {
            levels.fill(-1);
        }
        TerrainNeighborhood data;
        std::array<int16_t, Area> levels;
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
    const auto seed = center(basin.seed);
    auto& start =
        search.try_emplace(basin.seed, terrain(basin.seed)).first->second;
    const int minimum = groundAt(start.data, seed) + 2;
    if (minimum > basin.sillY) return {};
    start.levels[index(seed)] = int16_t(minimum);
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
                const int threshold =
                    std::max(level, groundAt(it->second.data, next) + 2);
                visited = int16_t(threshold);
                if (threshold <= searchLimit)
                    frontier[threshold].push_back(next);
            }
        }
    }
    for (const auto& [i, cells] : search)
        for (int j = 0; j < Area; ++j)
            if (cells.levels[j] >= minimum && cells.levels[j] <= searchLimit)
                result.masks[i].set(j);
    if (!result.masks.empty()) result.lake.surfaceY = searchLimit;
    return result;
}

GeneratedHydrology::Footprint GeneratedHydrology::refineOcean(
    int region) const {
    const auto c = center(region);
    const int period = width * Side;
    std::unordered_map<int, TerrainNeighborhood> neighborhoods;
    std::unordered_map<int, std::bitset<Area>> seen;
    std::vector<Vec2i> wet;
    Footprint result;
    Mask& mask = result.masks[region];
    int admitted = 0;
    const auto admit = [&](Vec2i tile, int i) {
        if (regions[i].body > Sea) return;
        auto& checked = seen[i];
        const int index = tile.y % Side * Side + tile.x % Side;
        if (checked.test(index)) return;
        checked.set(index);
        if (!regions[i].deep) {
            auto it = neighborhoods.find(i);
            if (it == neighborhoods.end())
                it = neighborhoods.emplace(i, terrain(i)).first;
            if (!TerrainSampler(it->second).belowSea(tile.x, tile.y)) return;
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
            if (regions[i].oceanSource) admit(center(i), i);
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

std::shared_ptr<const GeneratedHydrology::Footprint>
GeneratedHydrology::footprint(uint64_t key) const {
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
                        : refineOcean(int(key >> 1))));
        } catch (...) {
            producer->set_exception(std::current_exception());
        }
    }
    return value.get();
}

uint32_t GeneratedHydrology::candidateBody(int region) const {
    return regions[region].body;
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
    int top = r.nearOcean ? SEA_LEVEL_Y : -1;
    if (r.shore == SharedShore) {
        for (int i : shoreNeighborhood(region))
            if (i >= 0)
                if (const auto* b = basin(regions[i].body))
                    top = std::max(top, b->sillY - 1);
    } else if (r.shore >= 0) {
        top = std::max(top, basins.at(uint32_t(r.shore) + 2).sillY - 1);
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
                (r.nearOcean && r.body <= Sea && !(r.deep && r.oceanSource)))
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

WaterSurface GeneratedHydrology::waterAt(int x, int z, int groundY) const {
    const int i = regionAt(x, z);
    if (i < 0) return {};
    const auto& r = regions[i];
    if (r.deep && r.oceanSource) return {SeaY, Sea};
    const int index = z % Side * Side + int(WorldWrap::mod(x, Side));
    if (r.nearOcean && r.body <= Sea && groundY < SEA_LEVEL_Y &&
        footprint(uint64_t(i) << 1)->masks.at(i).test(index))
        return {SeaY, Sea};
    if (const int seed = lakeSeedAt(i, {x, z}); seed >= 0) {
        const uint32_t id = uint32_t(seed) + 2;
        if (groundY + 1 >= basins.at(id).sillY) return {};
        const auto wet = footprint((uint64_t(id) << 1) | 1);
        if (const auto it = wet->masks.find(i);
            it != wet->masks.end() && it->second.test(index))
            return {wet->lake.surfaceY, id};
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
                if (r.nearOcean && r.body <= Sea) {
                    const auto sea = footprint(uint64_t(i) << 1);
                    if (!matches(sea->masks.at(i), expectedBody == Sea))
                        return false;
                } else if (expectedBody == Sea) {
                    return false;
                }
                if (expectedBody != Sea) {
                    const auto lakeMatches = [&](uint32_t id) {
                        const auto lake = footprint((uint64_t(id) << 1) | 1);
                        const auto mask = lake->masks.find(i);
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
