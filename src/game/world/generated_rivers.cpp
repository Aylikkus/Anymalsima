#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <unordered_set>

#include "generated_hydrology.hpp"
#include "terrain_sampler.hpp"

namespace {
constexpr float BankWidth = 3;
constexpr int ProfileWindow = 32;
constexpr unsigned SpringUnits = 8;

uint64_t bucketKey(int x, int z) {
    return (uint64_t(unsigned(z)) << 32) | unsigned(x);
}

struct Projection {
    float t, distance;
};

Projection project(double x, double z, Vec2i from, Vec2i to, int period) {
    const float dx = to.x - from.x, dz = to.y - from.y;
    double offset = x - from.x;
    if (offset < -period * .5 || offset >= period * .5)
        offset = WorldWrap::deltaf(x, from.x, period);
    const float px = offset, pz = z - from.y;
    const float t =
        std::clamp((dx * px + dz * pz) / (dx * dx + dz * dz), 0.f, 1.f);
    return {t, std::hypot(px - dx * t, pz - dz * t)};
}
}  // namespace

const std::vector<uint32_t>* GeneratedHydrology::spansAt(int x, int z) const {
    if (riverIndex.empty() || z < 0 || z >= height * Side) return nullptr;
    const auto key = bucketKey(
        int(WorldWrap::mod(x, width * Side)) / RiverBucket, z / RiverBucket);
    const auto it = riverIndex.find(key);
    return it == riverIndex.end() ? nullptr : &it->second;
}

std::optional<GeneratedHydrology::Channel> GeneratedHydrology::riverAt(
    int x, int z) const {
    const auto* candidates = spansAt(x, z);
    if (!candidates) return {};
    std::optional<Channel> result;
    float best = std::numeric_limits<float>::infinity();
    for (uint32_t i : *candidates) {
        const auto& span = spans[i];
        const auto p = project(x, z, span.from, span.to, width * Side);
        const float radius = std::lerp(span.radius0, span.radius1, p.t);
        if (p.distance > radius + BankWidth) continue;
        // Wet channel cores take precedence over another channel's bank.
        const float priority =
            p.distance <= radius ? p.distance : p.distance + 32;
        if (priority >= best) continue;
        best = priority;
        const bool fall = span.level0 - span.level1 >= MinFallDrop;
        const float t = fall ? (p.t < .5f ? 0.f : 1.f) : p.t;
        const int level =
            int(std::round(std::lerp(span.level0, span.level1, t)));
        // New channels have a shallow shelf around the wetted edge.
        const float deepRadius = naturalProfiles ? .88f * radius : radius;
        const float across = std::min(1.f, p.distance / deepRadius);
        const int depth =
            std::max(1, int(std::round(std::lerp(span.depth0, span.depth1, t) *
                                       (1 - across * across))));
        const float dx = span.to.x - span.from.x, dz = span.to.y - span.from.y;
        const float length = std::hypot(dx, dz);
        result = Channel{{level, 0, span.reach, {dx / length, dz / length}},
                         level - depth - 1,
                         p.distance,
                         radius};
    }
    return result;
}

int GeneratedHydrology::bankTop(int x, int z) const {
    int top = -1;
    for (const auto& [dx, dz] :
         std::array<Vec2i, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
        if (const auto neighbor = riverAt(x + dx, z + dz);
            neighbor && neighbor->distance <= neighbor->radius)
            top = std::max(top, neighbor->water.topTile());
    return top;
}

double GeneratedHydrology::carve(int x, int z, double originalY) const {
    const auto river = riverAt(x, z);
    double bed = originalY;
    bool core = false;
    if (river) {
        core = river->distance <= river->radius;
        bed = river->bedY;
        if (!core) {
            const double t = (river->distance - river->radius) / BankWidth;
            bed = std::lerp(double(river->water.topTile()),
                            std::max(originalY, double(river->water.topTile())),
                            t);
        }
        bed = std::clamp(bed, std::max(0., originalY - MaxRiverCut), originalY);
    }
    if (boundedBanks) {
        if (core) {
            // Fill deeply eroded ground underneath the channel. Otherwise
            // the depth cap can cut dry holes into a flowing bed.
            bed = std::max(bed,
                           double(river->water.surfaceY - MaxRiverDepth - 1));
            if (bed >= river->water.topTile())
                bed = std::max(bed, double(bankTop(x, z)));
        } else {
            const auto high = riverCeilings.find(regionAt(x, z));
            // A neighbor can still reach this bank where the channel width
            // changes, even if the current point lies outside its own span.
            if (high != riverCeilings.end() && bed < high->second &&
                (river || hasChannel(x - 1, z - 1, x + 2, z + 2)))
                bed = std::max(bed, double(bankTop(x, z)));
        }
    }
    if (bed == originalY || standingWaterAt(x, z, int(originalY)).wet())
        return originalY;
    return bed;
}

std::pair<int, WaterSurface> GeneratedHydrology::sampleRiverColumn(
    int x, int z) const {
    if (!spansAt(x, z)) return {-1, {}};
    return sampleColumn(x, z);
}

std::pair<int, WaterSurface> GeneratedHydrology::sampleColumn(int x,
                                                              int z) const {
    const int region = regionAt(x, z);
    if (region < 0) return {-1, {}};
    const auto data = terrain(region);
    const double original = TerrainSampler(data).surface(x, z);
    const int groundY = int(std::floor(carve(x, z, original)));
    return {groundY, waterAt(x, z, groundY)};
}

bool GeneratedHydrology::springRockAt(int x, int y, int z) const {
    if (springRock.empty()) return false;
    const auto it =
        springRock.find(bucketKey(int(WorldWrap::mod(x, width * Side)), z));
    return it != springRock.end() && it->second == y;
}

bool GeneratedHydrology::hasChannel(int x0, int z0, int x1, int z1) const {
    if (riverIndex.empty() || x0 >= x1 || z0 >= z1) return false;
    const double cx = (double(x0) + x1 - 1) * .5;
    const double cz = (double(z0) + z1 - 1) * .5;
    const double half = std::hypot(x1 - x0 - 1, z1 - z0 - 1) * .5;
    for (int rz = std::max(0, z0); rz < std::min(z1, height * Side);) {
        const int ez = std::min(z1, rz + Side - rz % Side);
        for (int rx = x0; rx < x1;) {
            const int ex =
                std::min(x1, rx + Side - int(WorldWrap::mod(rx, Side)));
            if (riverCeilings.contains(regionAt(rx, rz)))
                for (int z = rz; z < ez; z += RiverBucket - z % RiverBucket)
                    for (int x = rx; x < ex;
                         x += RiverBucket - WorldWrap::mod(x, RiverBucket))
                        if (const auto* candidates = spansAt(x, z))
                            for (uint32_t i : *candidates) {
                                const auto& s = spans[i];
                                if (project(cx, cz, s.from, s.to, width * Side)
                                        .distance <=
                                    std::max(s.radius0, s.radius1) + BankWidth +
                                        half)
                                    return true;
                            }
            rx = ex;
        }
        rz = ez;
    }
    return false;
}

int GeneratedHydrology::channelFloorLevel(int x, int z, int radius) const {
    int level = WORLD_HEIGHT_Y + 1;
    // Work scales with indexed segments, independent of the number of
    // terrain columns this coarse vertex represents.
    for (int rz = std::max(0, z - radius);
         rz < std::min(height * Side, z + radius);
         rz += RiverBucket - rz % RiverBucket)
        for (int rx = x - radius; rx < x + radius;
             rx += RiverBucket - WorldWrap::mod(rx, RiverBucket))
            if (const auto* entries = spansAt(rx, rz))
                for (uint32_t id : *entries) {
                    const auto& span = spans[id];
                    const auto p = project(x - .5, z - .5, span.from, span.to,
                                           width * Side);
                    if (p.distance <= std::max(span.radius0, span.radius1) +
                                          (radius - .5f) * 1.414214f)
                        level = std::min(level, span.level1);
                }
    return level <= WORLD_HEIGHT_Y ? level : -1;
}

void GeneratedHydrology::indexSpan(Span span) {
    const int pad =
        int(std::ceil(std::max(span.radius0, span.radius1) + BankWidth));
    const int x0 = std::min(span.from.x, span.to.x) - pad;
    const int z0 = std::max(0, std::min(span.from.y, span.to.y) - pad);
    const int x1 = std::max(span.from.x, span.to.x) + pad;
    const int z1 =
        std::min(height * Side - 1, std::max(span.from.y, span.to.y) + pad);
    const auto id = uint32_t(spans.size());
    spans.push_back(span);
    for (int z = z0; z <= z1; z += RiverBucket - z % RiverBucket)
        for (int x = x0; x <= x1;
             x += RiverBucket - WorldWrap::mod(x, RiverBucket)) {
            const int wx = int(WorldWrap::mod(x, width * Side));
            riverIndex[bucketKey(wx / RiverBucket, z / RiverBucket)].push_back(
                id);
            auto& top = riverCeilings[regionAt(wx, z)];
            top = std::max(top, span.level0 - 1);
        }
}

bool GeneratedHydrology::buildReaches(const std::function<bool()>& cancelled,
                                      bool naturalRivers,
                                      bool connectLakeOutlets) {
    const auto discharge = [](float strength) {
        return std::isfinite(strength) ? std::clamp(strength, 0.f, 512.f) : 0.f;
    };
    struct Station {
        Vec2i tile;
        int ground, level, depth;
        float radius;
    };
    constexpr int SteepWindow = 8;
    constexpr int SteepDrop = 18;
    const auto grade = [&](std::vector<Station>& profile, Reach& reach) {
        // A reach may keep its strongest natural drop. Spread its remaining
        // elevation changes upstream at at most one block per station.
        size_t lip = SIZE_MAX;
        int strongest = SteepDrop - 1;
        for (size_t i = 1; i + SteepWindow < profile.size(); ++i) {
            const size_t end = i + SteepWindow;
            const int high = profile[i].level, low = profile[end].level;
            const int slope = profile[i].ground - profile[end].ground;
            if (slope <= strongest || high - low < SteepDrop) continue;
            strongest = slope;
            lip = i;
        }
        if (lip != SIZE_MAX) {
            const size_t end = lip + SteepWindow;
            const int low = profile[end].level;
            const int high = std::min(profile[lip].level, low + MaxFallDrop);
            profile[lip].level = high;
            size_t landing = lip + 1;
            while (landing < end && profile[landing].ground + 1 >= high)
                ++landing;
            for (size_t i = lip + 1; i < end; ++i)
                profile[i].level = i < landing ? high : low;
            lip = landing - 1;
        }

        std::vector<int> levels(profile.size());
        const auto solve = [&] {
            size_t blocked = SIZE_MAX;
            levels[profile.size() - 1] = profile.back().level;
            for (size_t i = profile.size() - 1; i-- > 0;) {
                levels[i] =
                    std::min(profile[i].level,
                             levels[i + 1] + (i == lip ? MaxFallDrop : 1));
                if (levels[i] <
                    std::max(2, profile[i].ground - MaxRiverCut + 2))
                    blocked = i;
            }
            return blocked;
        };
        if (const size_t blocked = solve(); blocked != SIZE_MAX) {
            profile.resize(blocked + 1);
            reach.end = RiverEnd::Plateau;
            reach.receiver = 0;
            // A shorter route has a higher outlet, reducing each upstream
            // cut. The last retained station retains a feasible local head.
            profile.back().level = std::max(
                profile.back().level, profile.back().ground - MaxRiverCut + 2);
            solve();
        }
        for (size_t i = 0; i < profile.size(); ++i) {
            auto& s = profile[i];
            s.level = levels[i];
            s.depth = std::min(
                {s.depth, s.level - 1, MaxRiverCut + s.level - s.ground - 1});
        }
    };
    const int period = width * Side;
    std::vector<size_t> order(beds.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return beds[a].size() > beds[b].size();
    });

    std::unordered_set<uint32_t> lakeOutlets;
    uint32_t nextOutlet = uint32_t(beds.size()) + 1;
    const auto publish = [&](std::vector<Station>& profile, Reach& reach) {
        if (profile.size() < 2) return;
        const int sourceLevel = profile.front().level;
        if (naturalRivers) grade(profile, reach);
        if (reach.lakeSource &&
            (!reach.receiver || profile.front().level != sourceLevel))
            return;
        if (profile.size() < 2) return;
        // Legacy profiles concentrate drops once per window.
        for (size_t i = 1; !naturalRivers && i + 1 < profile.size();) {
            size_t end = i + 1;
            while (end + 1 < profile.size() && end - i < ProfileWindow &&
                   profile[i].level - profile[end + 1].level <= MaxFallDrop)
                ++end;
            const int high = profile[i].level, low = profile[end].level;
            size_t landing = i + 1;
            while (landing < end && profile[landing].ground + 1 >= high)
                ++landing;
            if (high - low >= MinFallDrop && high - low <= MaxFallDrop &&
                std::all_of(profile.begin() + landing, profile.begin() + end,
                            [&](const Station& s) {
                                return s.ground - (low - s.depth - 1) <=
                                           MaxRiverCut &&
                                       low > s.depth;
                            })) {
                for (size_t j = i + 1; j < end; ++j)
                    profile[j].level = j < landing ? high : low;
                i = end;
            } else {
                ++i;
            }
        }

        if (!reach.lakeSource) {
            const auto start = profile.front().tile;
            reach.spring = {start, profile.front().level - 2, SpringUnits};
            springRock.emplace(bucketKey(start.x, start.y), reach.spring.rockY);
        } else {
            lakeOutlets.insert(reach.lakeSource->body);
            retainOutletLake(reach.lakeSource->body);
        }
        reach.outlet = profile.back().tile;
        for (size_t i = 1; i < profile.size();) {
            const auto& a = profile[i - 1];
            const auto& b = profile[i];
            const int dx = WorldWrap::delta(b.tile.x, a.tile.x, period);
            const int dz = b.tile.y - a.tile.y;
            size_t end = i;
            // Merge flat, straight spans, bounded to one bucket's length.
            if (a.level == b.level && a.depth == b.depth &&
                a.radius == b.radius)
                while (end + 1 < profile.size() && end - i < RiverBucket - 1 &&
                       profile[end + 1].level == a.level &&
                       profile[end + 1].depth == a.depth &&
                       profile[end + 1].radius == a.radius &&
                       WorldWrap::delta(profile[end + 1].tile.x,
                                        profile[end].tile.x, period) == dx &&
                       profile[end + 1].tile.y - profile[end].tile.y == dz)
                    ++end;
            const auto& target = profile[end];
            const int tx =
                a.tile.x + WorldWrap::delta(target.tile.x, a.tile.x, period);
            indexSpan({a.tile,
                       {tx, target.tile.y},
                       a.level,
                       target.level,
                       a.depth,
                       target.depth,
                       a.radius,
                       target.radius,
                       reach.id});
            if (a.level - target.level >= MinFallDrop)
                reach.falls.push_back(
                    {a.tile, target.tile, a.level, target.level, 2 * b.radius});
            i = end + 1;
        }
        if (reach.end == RiverEnd::Lake) retainOutletLake(reach.receiver);
        reaches.push_back(std::move(reach));
    };

    TerrainNeighborhood data;
    int lastRegion = -1;
    for (size_t route : order) {
        if (cancelled && cancelled()) return false;
        const auto& candidate = beds[route];
        if (candidate.size() < 2) continue;
        const auto start = candidate.front().tile;
        if (const auto channel = riverAt(start.x, start.y);
            channel && channel->distance <= channel->radius)
            continue;

        // Path order is part of the saved generation inputs. Longer paths
        // establish the downstream profile first, with saved order as a tie.
        Reach reach{
            uint32_t(route + 1), {}, RiverEnd::ClosedBasin, start, 0, {}};
        std::vector<Station> profile;
        const auto startLakeOutlet = [&](WaterSurface lake, Vec2i tile,
                                         int ground, float radius) {
            profile.clear();
            if (lakeOutlets.contains(lake.body)) return false;
            reach = {nextOutlet++, {}, RiverEnd::ClosedBasin, tile, 0, {}};
            reach.lakeSource = LakeOutlet{lake.body, tile};
            profile.push_back({tile, ground, lake.surfaceY, 1, radius});
            return true;
        };
        bool ended = false;
        for (size_t i = 1; i < candidate.size() && !ended; ++i) {
            const auto a = candidate[i - 1].tile, b = candidate[i].tile;
            const int dx = WorldWrap::delta(b.x, a.x, period), dz = b.y - a.y;
            const int steps = std::max(std::abs(dx), std::abs(dz));
            // Saved preview routes use adjacent region centers (with short
            // fractional steps at mouths). Bound malformed route segments.
            if (steps == 0) continue;
            if (steps > 2 * Side) break;
            const float from = discharge(candidate[i - 1].strength);
            const float to = discharge(candidate[i].strength);
            const auto previousPoint = i >= 2 ? candidate[i - 2].tile : a;
            const auto nextPoint =
                i + 1 < candidate.size() ? candidate[i + 1].tile : b;
            const double tangentX0 =
                i >= 2
                    ? .5 * (dx - WorldWrap::delta(previousPoint.x, a.x, period))
                    : dx;
            const double tangentZ0 =
                i >= 2 ? .5 * (dz - (previousPoint.y - a.y)) : dz;
            const double tangentX1 =
                i + 1 < candidate.size()
                    ? .5 * (dx + WorldWrap::delta(nextPoint.x, b.x, period))
                    : dx;
            const double tangentZ1 =
                i + 1 < candidate.size() ? .5 * (dz + nextPoint.y - b.y) : dz;
            const int subdivisions = naturalRivers ? 2 * steps : steps;
            const double phase = SoilGenerator::unit(a.x, a.y, featureSeed) *
                                 2 * std::numbers::pi;
            for (int step = 0; step <= subdivisions; ++step) {
                if ((step % 32 == 0) && cancelled && cancelled()) return false;
                const double t = double(step) / subdivisions;
                const float scale = std::sqrt(std::lerp(from, to, float(t)));
                double px = dx * t, pz = dz * t;
                float widthVariation = 0.f;
                if (naturalRivers) {
                    const double t2 = t * t;
                    const double ease = t2 * (3 - 2 * t);
                    const double h0 = t * (1 - t) * (1 - t);
                    const double h1 = -t2 * (1 - t);
                    const double wave =
                        .085 * std::sin(2 * std::numbers::pi * t + phase) * 16 *
                        t2 * (1 - t) * (1 - t);
                    px =
                        ease * dx + h0 * tangentX0 + h1 * tangentX1 - dz * wave;
                    pz =
                        ease * dz + h0 * tangentZ0 + h1 * tangentZ1 + dx * wave;
                    widthVariation =
                        .55f * float(std::sin(phase + t * 9) * 4 * t * (1 - t));
                }
                const float radius = std::clamp(
                    std::round((1.25f + .24f * scale + widthVariation) * 4) / 4,
                    1.25f, 7.f);
                const Vec2i tile{
                    int(WorldWrap::mod(a.x + std::lround(px), period)),
                    int(std::clamp(a.y + std::lround(pz), 0L,
                                   long(height * Side - 1)))};
                if (!profile.empty() && tile == profile.back().tile) continue;
                const int region = regionAt(tile.x, tile.y);
                if (lastRegion != region) {
                    data = terrain(region);
                    lastRegion = region;
                }
                const int ground = int(
                    std::floor(TerrainSampler(data).surface(tile.x, tile.y)));
                const auto channel = riverAt(tile.x, tile.y);
                const auto standing =
                    standingWaterAt(tile.x, tile.y, ground, true);
                const bool lake = connectLakeOutlets && standing.body > Sea;
                // At a lake shore the standing body owns the connection,
                // including tiles shared with an already indexed inlet.
                const bool junction =
                    !lake && channel && channel->distance <= .75f;
                if (lake && reach.lakeSource &&
                    standing.body == reach.lakeSource->body) {
                    // The route can briefly leave and re-enter a lake.
                    // Keep the last wet station as the outlet's anchor.
                    reach.lakeSource->tile = tile;
                    profile.resize(1);
                    profile.front() = {tile, ground, standing.surfaceY, 1,
                                       radius};
                    continue;
                }
                if (lake && profile.empty()) {
                    if (startLakeOutlet(standing, tile, ground, radius))
                        continue;
                    ended = true;
                    break;
                }
                const int previous =
                    profile.empty() ? WORLD_HEIGHT_Y : profile.back().level;
                int level = std::min(previous, ground + 1);
                if (reach.lakeSource && profile.size() == 1) level = previous;
                int depth = std::clamp(
                    int(std::round(1 +
                                   1.5f * scale *
                                       std::min(1.f, profile.size() / 48.f))),
                    1, MaxRiverDepth);
                if (junction || standing.wet()) {
                    const auto receiver = junction ? channel->water : standing;
                    if (profile.empty()) {
                        ended = true;
                        break;
                    }
                    if (receiver.surfaceY > previous ||
                        (reach.lakeSource && (lake || junction) &&
                         receiver.surfaceY >= profile.front().level)) {
                        reach.end = RiverEnd::Plateau;
                        ended = true;
                        break;
                    }
                    level = receiver.surfaceY;
                    if (junction) depth = level - channel->bedY - 1;
                    reach.end = junction               ? RiverEnd::Confluence
                                : standing.body == Sea ? RiverEnd::Sea
                                                       : RiverEnd::Lake;
                    reach.receiver = junction ? receiver.reach : receiver.body;
                    ended = true;
                }
                depth = std::min(
                    {depth, MaxRiverCut + level - ground - 1, level - 1});
                if (depth < 1 ||
                    (previous != WORLD_HEIGHT_Y &&
                     previous - level >
                         (naturalRivers ? MaxRiverCut : MaxFallDrop))) {
                    reach.end = RiverEnd::Plateau;
                    reach.receiver = 0;
                    ended = true;
                    break;
                }
                profile.push_back({tile, ground, level, depth, radius});
                if (lake) {
                    publish(profile, reach);
                    ended = !startLakeOutlet(standing, tile, ground, radius);
                }
                if (ended && (lake || reach.lakeSource)) break;
            }
        }
        publish(profile, reach);
    }
    return true;
}
