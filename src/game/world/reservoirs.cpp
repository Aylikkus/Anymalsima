#include "reservoirs.hpp"

#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

#include "chunk.hpp"
#include "generated_hydrology.hpp"
#include "serialization/section_container.hpp"

namespace {
constexpr auto MaxVolume = std::numeric_limits<WaterVolume>::max();
constexpr WaterVolume LakeSeepRate = 32;
constexpr uint64_t ReachFlag = uint64_t{1} << 32;

WaterVolume add(WaterVolume a, WaterVolume b) {
    if (b > 0 && a > MaxVolume - b) return MaxVolume;
    if (b < 0 && a < -MaxVolume - b) return -MaxVolume;
    return a + b;
}

WaterVolume scale(WaterVolume rate, uint64_t steps) {
    if (!rate || !steps) return 0;
    return steps > uint64_t(MaxVolume / rate) ? MaxVolume
                                              : rate * WaterVolume(steps);
}
}  // namespace

StorageCurve::StorageCurve(Levels storage) : storage(std::move(storage)) {
    if (this->storage.front() != 0 ||
        !std::is_sorted(this->storage.begin(), this->storage.end()))
        throw std::invalid_argument("Invalid reservoir storage curve");
}

StorageCurve StorageCurve::constantArea(WaterVolume area, int floor, int top) {
    if (area <= 0 || floor < 0 || floor >= top || top > WORLD_HEIGHT_Y ||
        area > MaxVolume / WaterUnitsPerBlock / (top - floor))
        throw std::invalid_argument("Invalid reservoir dimensions");
    Levels storage{};
    for (int y = floor + 1; y <= WORLD_HEIGHT_Y; ++y)
        storage[y] = area * WaterUnitsPerBlock * (std::min(y, top) - floor);
    return StorageCurve(std::move(storage));
}

int StorageCurve::levelAt(WaterVolume volume) const {
    if (volume <= 0) return -1;
    const auto cap =
        std::lower_bound(storage.begin(), storage.end(), capacity());
    return int(std::lower_bound(storage.begin(), cap, volume) -
               storage.begin());
}

uint8_t StorageCurve::fillAtSurface(WaterVolume volume) const {
    const int top = levelAt(volume);
    if (top <= 0) return 255;
    const auto lower = storage[top - 1];
    const auto band = storage[top] - lower;
    // Quantize visibility to roughly 1/16 block to bound remesh frequency.
    const auto bins = int(std::ceil(16.0 * double(volume - lower) / band));
    return uint8_t(std::clamp((bins * 255 + 15) / 16, 1, 255));
}

uint64_t ReservoirSnapshot::columnKey(int x, int z) {
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
}

void PoolFootprint::add(Vec2i tile, int period, int anchorX, int floor,
                        int sill) {
    columns.emplace(ReservoirSnapshot::columnKey(tile.x, tile.y),
                    std::pair{floor, sill});
    const int x = anchorX + int(WorldWrap::delta(tile.x, anchorX, period));
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minZ = std::min(minZ, tile.y);
    maxZ = std::max(maxZ, tile.y);
}

bool PoolFootprint::intersects(int x0, int z0, int x1, int z1,
                               int period) const {
    if (z0 > maxZ || z1 <= minZ) return false;
    const auto inside = [&](int shift) {
        return x0 <= maxX + shift && x1 > minX + shift;
    };
    return inside(0) || (period > 0 && (inside(period) || inside(-period)));
}

bool ReservoirSnapshot::poolIntersects(int x0, int z0, int x1, int z1,
                                       int period) const {
    return std::any_of(pools.begin(), pools.end(), [&](const auto& entry) {
        const auto& pool = entry.second;
        return (pool.level >= 0 || pool.parent) &&
               pool.footprint->intersects(x0, z0, x1, z1, period);
    });
}

int ReservoirSnapshot::localTop(int x, int z) const { return cells.top(x, z); }

int ReservoirSnapshot::level(uint32_t body, int baseline) const {
    const auto it = levels.find(body);
    return it == levels.end() ? baseline : it->second;
}

namespace {
constexpr uint64_t RiverStageInterval = 216;
constexpr uint64_t RiverStageDuration = 100 * RiverStageInterval;
constexpr uint64_t MaxRiverDelay = 43200;
constexpr uint64_t MaxRiverAge = RiverStageDuration + MaxRiverDelay;

uint64_t riverDelay(Vec2i start, Vec2i end, int period) {
    const auto distance = std::abs(WorldWrap::delta(end.x, start.x, period)) +
                          std::abs(int64_t(end.y) - start.y);
    return std::min<uint64_t>(distance * 4, MaxRiverDelay);
}

// Integral of a linear 0..100% recovery, in whole game seconds. Taking the
// difference at the two budget endpoints also works when time fast-forwards.
uint64_t recoveredSeconds(uint64_t age, uint64_t delay) {
    const auto active = age > delay ? age - delay : 0;
    const auto ramp = std::min(active, RiverStageDuration);
    return active - ramp + ramp * ramp / (2 * RiverStageDuration);
}

unsigned riverTransition(uint64_t age, uint64_t delay) {
    return unsigned(
        std::min(RiverStageDuration, age > delay ? age - delay : 0) /
        RiverStageInterval);
}
}  // namespace

bool ReservoirSnapshot::RiverStage::belowCut(int x, int z, int surface) const {
    const auto dx = WorldWrap::delta(x, cut.x, periodX);
    const auto dz = int64_t(z) - cut.z;
    if (damUpstream && std::abs(dx) <= DamRadius && std::abs(dz) <= DamRadius)
        return !std::binary_search(
            damUpstream->begin(), damUpstream->end(),
            columnKey(int(WorldWrap::mod(x, periodX)), z));
    if (surface == head && (downstream.x || downstream.y))
        return dx * downstream.x + dz * downstream.y > 0;
    return surface <= head;
}

bool ReservoirSnapshot::RiverStage::isolatesLake(uint32_t body, int x,
                                                 int z) const {
    return body && outletLake == body && damUpstream &&
           std::abs(WorldWrap::delta(x, cut.x, periodX)) <= DamRadius &&
           std::abs(int64_t(z) - cut.z) <= DamRadius && belowCut(x, z, head);
}

int ReservoirSnapshot::RiverStage::backwaterAt(int x, int z, int bottom,
                                               int full, uint32_t* source,
                                               Vec2* direction) const {
    int highest = bottom;
    const Backwater* supplier = nullptr;
    constexpr int Reach = 32;
    int nearest = Reach;
    for (const auto& bank : banks) {
        const auto distance =
            std::abs(WorldWrap::delta(x, bank.tile.x, periodX)) +
            std::abs(int64_t(z) - bank.tile.y);
        if (distance >= Reach) continue;
        const int depth = std::min(full, bank.head) - bottom;
        const int head =
            bottom + depth *
                         int(std::min<int64_t>(Reach - 2, Reach - distance)) /
                         (Reach - 2);
        if (head <= bottom || head < highest ||
            (head == highest && distance >= nearest))
            continue;
        highest = head;
        nearest = int(distance);
        supplier = &bank;
    }
    if (supplier) {
        if (source) *source = supplier->reach;
        if (direction && nearest) {
            const float dx =
                float(WorldWrap::delta(x, supplier->tile.x, periodX));
            const float dz = float(z - supplier->tile.y);
            const float length = std::hypot(dx, dz);
            *direction = {dx / length, dz / length};
        }
    }
    return highest;
}

WaterSurface ReservoirSnapshot::projectStanding(
    const GeneratedHydrology* hydrology, int x, int z, int ground,
    WaterSurface baseline, bool includeBackwater) const {
    if (baseline.body && !baseline.reach) {
        for (const auto& [reach, stage] : riverStages)
            if (stage.isolatesLake(baseline.body, x, z)) return {};
        const auto key = columnKey(x, z);
        for (const auto& [id, pool] : pools)
            if (pool.parent == baseline.body &&
                pool.footprint->columns.contains(key))
                return {};
        const int height = level(baseline.body, baseline.surfaceY);
        if (height <= ground + 1 ||
            (height < baseline.surfaceY && hydrology &&
             !hydrology->connectedAt(baseline.body, x, z, height)))
            baseline = {};
        else {
            const bool naturalTop = height == baseline.surfaceY;
            baseline.surfaceY = height;
            if (!naturalTop) baseline.topFill = 255;
            if (const auto it = surfaceFills.find(baseline.body);
                it != surfaceFills.end())
                baseline.topFill =
                    naturalTop
                        ? uint8_t(std::max(
                              1, it->second * int(baseline.topFill) / 255))
                        : it->second;
        }
    } else if (const auto it = riverStages.find(baseline.reach);
               it != riverStages.end()) {
        const auto& stage = it->second;
        const bool belowCut = stage.belowCut(x, z, baseline.surfaceY);
        unsigned limit = belowCut ? stage.percent : 100;
        if (belowCut && stage.damAge) {
            const auto delay =
                riverDelay({stage.cut.x, stage.cut.z}, {x, z}, stage.periodX);
            limit = 100 - riverTransition(*stage.damAge, delay);
        }
        unsigned percent = stage.damAge && belowCut
                               ? limit * stage.lakeSupplyPercent / 100
                               : std::min(limit, stage.lakeSupplyPercent);
        if (stage.receding || stage.recovering ||
            stage.lakeSupplyPercent < 100 || stage.damAge) {
            if (stage.receding || stage.recovering) {
                const auto delay =
                    riverDelay(stage.spring, {x, z}, stage.periodX);
                if (stage.recovering && stage.recoveryAge >= delay) {
                    const auto deficit = riverTransition(stage.recessionAge, 0);
                    const auto recovered =
                        riverTransition(stage.recoveryAge, delay);
                    percent = std::min(
                        limit, 100 - deficit + std::min(deficit, recovered));
                } else {
                    const auto drained = riverTransition(
                        stage.recessionAge + stage.recoveryAge, delay);
                    percent = std::min(limit, 100 - drained);
                }
            }
            unsigned tributaryPercent = 0;
            for (const auto& inlet : stage.inlets) {
                if (stage.damAge && belowCut &&
                    !stage.belowCut(inlet.tile.x, inlet.tile.y, inlet.height))
                    continue;
                if (baseline.surfaceY > inlet.height) continue;
                const auto dx =
                    WorldWrap::delta(x, inlet.tile.x, stage.periodX);
                if (baseline.surfaceY < inlet.height ||
                    dx * inlet.downstream.x +
                            (int64_t(z) - inlet.tile.y) * inlet.downstream.y >=
                        0)
                    tributaryPercent += inlet.percent;
            }
            percent = std::max(percent, std::min(stage.damAge ? 100u : limit,
                                                 tributaryPercent));
        }
        const int depthAtCenter =
            (std::max<int>(1, baseline.riverDepth) - 1) * 255 +
            baseline.topFill;
        const int bottom = (ground + 1) * 255;
        const int full = baseline.topTile() * 255 + baseline.topFill;
        int head =
            percent
                ? full - (depthAtCenter - depthAtCenter * int(percent) / 100)
                : bottom;
        if (includeBackwater && (!stage.damUpstream || belowCut)) {
            Vec2 direction = baseline.flow;
            const int backwater =
                stage.backwaterAt(x, z, bottom, full, nullptr, &direction);
            if (backwater > head) {
                head = backwater;
                baseline.flow = direction;
            }
        }
        const int depth = head - bottom;
        if (depth <= 0) return {};
        baseline.surfaceY = ground + 1 + (depth + 254) / 255;
        baseline.topFill = uint8_t((depth - 1) % 255 + 1);
    }
    return baseline;
}

WaterSurface ReservoirSnapshot::project(const GeneratedHydrology* hydrology,
                                        int x, int z, int ground,
                                        WaterSurface baseline) const {
    auto standing = projectStanding(hydrology, x, z, ground, baseline);
    if (!standing.wet())
        standing = projectLakeInflow(hydrology, x, z, ground, baseline.body);
    return projectLocal(x, z, ground, standing);
}

WaterSurface ReservoirSnapshot::projectLakeInflow(
    const GeneratedHydrology* hydrology, int x, int z, int ground,
    uint32_t body) const {
    if (!hydrology || !body || !levels.contains(body)) return {};
    for (const auto& [reach, stage] : riverStages)
        if (stage.isolatesLake(body, x, z)) return {};
    const auto* inlet = hydrology->lakeInflowAt(x, z);
    if (!inlet || inlet->body != body) {
        if (hydrology->lakeSpringAt(body, x, z) &&
            std::none_of(
                hydrology->riverReaches().begin(),
                hydrology->riverReaches().end(), [body](const auto& r) {
                    return r.end == GeneratedHydrology::RiverEnd::Lake &&
                           r.receiver == body;
                }))
            return {ground + 2, 0, 0, {}, true, 32};
        return {};
    }
    const auto& channel = inlet->channel;
    const auto source = projectStanding(
        hydrology, inlet->mouth.x, inlet->mouth.y, channel.bedY, channel.water);
    if (!source.wet()) return {};
    // This is the moving inlet's surface, not extra stored water. The reach
    // still delivers its volume exactly once through the budget graph.
    const auto fill = uint8_t(std::clamp(
        (source.topTile() - channel.bedY - 1) * 255 + source.topFill, 1, 64));
    return {ground + 2, 0, source.reach, source.flow, true, fill, 1};
}

WaterSurface ReservoirSnapshot::projectLocal(int x, int z, int ground,
                                             WaterSurface baseline,
                                             int* poolGround) const {
    // A runtime pool covers exactly its certified footprint columns; its
    // level moves with the pool's storage without a hydrology survey.
    if (!pools.empty()) {
        const auto key = ReservoirSnapshot::columnKey(x, z);
        for (const auto& [body, pool] : pools) {
            if (pool.level < 0) continue;
            const auto& columns = pool.footprint->columns;
            if (const auto c = columns.find(key); c != columns.end()) {
                const auto [storedFloor, sill] = c->second;
                const int floor = storedFloor < 0 ? ground + 1 : storedFloor;
                const int level = pool.closedCrest < 0
                                      ? pool.level
                                      : std::min(pool.level, pool.closedCrest);
                if (level <= floor || (!poolGround && level <= ground + 1))
                    continue;
                if (level <= sill) continue;
                const WaterSurface surface{
                    level, body,
                    0,     {0, 0},
                    true,  level < pool.level ? uint8_t(255) : pool.fill};
                if (surface.height() > baseline.height()) {
                    baseline = surface;
                    if (poolGround) *poolGround = floor - 1;
                }
            }
        }
    }
    // Reconciliation looks up exact local credits per voxel separately.
    if (poolGround) return baseline;
    if (const auto it = streams.find(columnKey(x, z)); it != streams.end()) {
        const auto& s = it->second;
        WaterSurface sheet{s.top + 1, 0, 0, s.flow, true, 64};
        if (sheet.height() > baseline.height() && s.top > ground)
            baseline = sheet;
    }
    const int local = localTop(x, z);
    if (local > ground + 1) {
        const WaterSurface surface{local, 0,    0,
                                   {},    true, cells.at({x, local - 1, z})};
        if (surface.height() > baseline.height()) return surface;
    }
    return baseline;
}

bool ReservoirChange::affects(const GeneratedHydrology* hydrology, int x0,
                              int z0, int x1, int z1, int period) const {
    const auto contains = [&](TilePos p) {
        if (p.z < z0 || p.z >= z1) return false;
        const auto inside = [&](int x) { return x >= x0 && x < x1; };
        return inside(p.x) ||
               (period > 0 && (inside(p.x + period) || inside(p.x - period)));
    };
    if ((local && contains(*local)) ||
        std::any_of(locals.begin(), locals.end(), contains))
        return true;
    if (hydrology && !reaches.empty() &&
        hydrology->hasChannel(x0, z0, x1, z1, reaches))
        return true;
    if (hydrology &&
        std::any_of(bodies.begin(), bodies.end(), [&](uint32_t id) {
            return hydrology->mayContainBody(id, x0, z0, x1, z1);
        }))
        return true;
    // Pool bodies have no generated region membership: their certified
    // footprint decides.
    return pools && std::any_of(bodies.begin(), bodies.end(), [&](uint32_t id) {
               const auto it = pools->find(id);
               if (it == pools->end()) return false;
               for (const auto& tile : it->second)
                   if (contains(TilePos{tile.x, 0, tile.y})) return true;
               return false;
           });
}

Reservoirs::Reservoirs(std::shared_ptr<const GeneratedHydrology> generated,
                       uint64_t initialStep)
    : generated(std::move(generated)), budgetStep(initialStep) {
    if (this->generated) {
        const auto& reaches = this->generated->riverReaches();
        for (size_t i = 0; i < reaches.size(); ++i) {
            const auto& reach = reaches[i];
            reachIndices.emplace(reach.id, i);
            if (reach.end == GeneratedHydrology::RiverEnd::Confluence)
                tributaries[reach.receiver].push_back(i);
            if (reach.lakeSource)
                lakeOutletReaches.emplace(reach.lakeSource->body, reach.id);
            springRates.emplace(
                reach.id, reach.lakeSource ? 0 : reach.spring.unitsPerStep);
        }
    }
    auto natural = routes({});
    if (!natural) throw std::logic_error("Generated river routing has a cycle");
    order = std::move(*natural);
    buildNaturalBudget();
}

void Reservoirs::define(uint32_t body, StorageCurve curve) {
    if (!body || !curve.capacity() || curves.contains(body) ||
        stores.contains(body))
        throw std::invalid_argument("Invalid reservoir definition");
    curves.emplace(body, std::move(curve));
}

const StorageCurve* Reservoirs::curve(uint32_t body) {
    if (const auto it = curves.find(body); it != curves.end())
        return &it->second;
    if (!generated || !body) return nullptr;
    auto storage = generated->storageCurve(body);
    if (!storage.capacity()) return nullptr;
    return &curves.emplace(body, std::move(storage)).first->second;
}

WaterVolume Reservoirs::volume(uint32_t body) {
    if (const auto it = stores.find(body); it != stores.end())
        return it->second;
    const auto* shape = curve(body);
    return shape ? shape->capacity() : 0;
}

WaterVolume Reservoirs::exchanged(ReservoirExchange reason) const {
    return totals.at(size_t(reason));
}

void Reservoirs::updateLevel(ReservoirSnapshot& next, const StorageCurve& shape,
                             uint32_t body, ReservoirChange& change) {
    const int baseline = shape.levelAt(shape.capacity());
    const auto volume = stores.at(body);
    const int level = shape.levelAt(volume);
    const auto fill = shape.fillAtSurface(volume);
    // Runtime pools publish their level through the pool entry, which also
    // certifies the footprint; generated bodies use the levels map.
    if (const auto pool = next.pools.find(body); pool != next.pools.end()) {
        if (pool->second.level == level && pool->second.fill == fill) return;
        pool->second.level = level;
        pool->second.fill = fill;
        change.bodies.push_back(body);
        return;
    }
    const auto oldFill = next.surfaceFills.find(body);
    if (next.level(body, baseline) == level &&
        (oldFill == next.surfaceFills.end() ? 255 : oldFill->second) == fill)
        return;
    if (level == baseline && fill == 255)
        next.levels.erase(body);
    else
        next.levels[body] = level;
    if (fill == 255)
        next.surfaceFills.erase(body);
    else
        next.surfaceFills[body] = fill;
    change.bodies.push_back(body);
}

void Reservoirs::publish(std::shared_ptr<ReservoirSnapshot> next,
                         const ReservoirChange& change) {
    if (change.local) filmSince.erase(*change.local);
    for (const auto cell : change.locals) filmSince.erase(cell);
    auto emitted = change;
    for (const auto body : change.bodies)
        if (const auto it = lakeOutletReaches.find(body);
            it != lakeOutletReaches.end())
            updateRiverStage(it->second, next, emitted);
    const auto wake = [&](TilePos p) {
        for (auto q :
             {p, TilePos{p.x + 1, p.y, p.z}, TilePos{p.x - 1, p.y, p.z},
              TilePos{p.x, p.y, p.z + 1}, TilePos{p.x, p.y, p.z - 1},
              TilePos{p.x, p.y + 1, p.z}, TilePos{p.x, p.y - 1, p.z}}) {
            if (generated) q.x = int(WorldWrap::mod(q.x, generated->periodX()));
            if (next->cells.contains(q))
                activeCells.insert(q);
            else
                activeCells.erase(q);
        }
    };
    if (!next->cells.empty() || !activeCells.empty()) {
        if (change.local) wake(*change.local);
        for (const auto p : change.locals) wake(p);
    }
    view.store(std::move(next));
    if (!emitted.bodies.empty() || !emitted.reaches.empty() || emitted.local ||
        !emitted.locals.empty()) {
        if (!emitted.pools && !emitted.bodies.empty() &&
            !poolFootprints.empty()) {
            if (!publishedFootprints)
                publishedFootprints = std::make_shared<
                    const std::map<uint32_t, std::vector<Vec2i>>>(
                    poolFootprints);
            emitted.pools = publishedFootprints;
        }
        changed.emit(emitted);
    }
}

WaterVolume Reservoirs::transfer(uint32_t body, WaterVolume amount,
                                 ReservoirExchange reason) {
    const bool source = reason == ReservoirExchange::Input ||
                        reason == ReservoirExchange::River;
    const bool sink = reason == ReservoirExchange::Disposal ||
                      reason == ReservoirExchange::Evaporation;
    if ((!source && !sink) || (source && amount <= 0) || (sink && amount >= 0))
        return 0;
    const auto* shape = curve(body);
    if (!shape) return 0;
    const auto before = volume(body);
    const auto accepted =
        std::clamp(amount, -before, shape->capacity() - before);
    if (!accepted) return 0;
    stores[body] = before + accepted;
    totals[size_t(reason)] = add(totals[size_t(reason)], accepted);
    ++serial;
    if (shape->levelAt(before) != shape->levelAt(stores.at(body)) ||
        shape->fillAtSurface(before) != shape->fillAtSurface(stores.at(body))) {
        auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
        ReservoirChange change;
        updateLevel(*next, *shape, body, change);
        publish(std::move(next), change);
    }
    return accepted;
}

WaterVolume Reservoirs::divert(uint32_t body, TilePos recipient,
                               WaterVolume amount) {
    if (amount <= 0 || recipient.x < 0 || recipient.z < 0 || recipient.y < 0 ||
        recipient.y >= WORLD_HEIGHT_Y)
        return 0;
    const auto* shape = curve(body);
    if (!shape) return 0;
    const auto current = snapshot();
    const auto fill = current->cells.fill(recipient);
    const auto before = volume(body);
    const auto accepted = std::min({amount, before, WaterUnitsPerBlock - fill});
    if (!accepted) return 0;
    auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
    next->cells.set(recipient, uint8_t(fill + accepted));
    stores[body] = before - accepted;
    ++serial;
    ReservoirChange change;
    change.local = recipient;
    updateLevel(*next, *shape, body, change);
    publish(std::move(next), change);
    return accepted;
}

namespace {
template <typename Capacity>
WaterVolume fillCut(const ReservoirSnapshot& current,
                    std::shared_ptr<ReservoirSnapshot>& next,
                    ReservoirChange& change, TilePos mouth,
                    WaterVolume remaining, uint64_t distanceLimit,
                    size_t cellLimit, const Reservoirs::CellQuery& access,
                    int periodX, const Capacity& capacity) {
    // Fill low ground first. Only a full cell can feed the cell above, and
    // only up to the source head. Unknown voxels have no receiving capacity.
    using Node = std::tuple<int, int, int, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<>> pending;
    const auto canonical = [&](TilePos p) {
        if (periodX > 0) p.x = int(WorldWrap::mod(p.x, periodX));
        return p;
    };
    const auto faces = [](TilePos p) {
        return std::array<TilePos, 6>{{{p.x, p.y - 1, p.z},
                                       {p.x - 1, p.y, p.z},
                                       {p.x + 1, p.y, p.z},
                                       {p.x, p.y, p.z - 1},
                                       {p.x, p.y, p.z + 1},
                                       {p.x, p.y + 1, p.z}}};
    };
    // Bound the survey by distance before prioritizing low cells. Otherwise
    // a broad low floor can use the entire budget before we even see the
    // upper chambers along a nearby, already submerged tunnel.
    std::unordered_set<TilePos> surveyed;
    std::vector<std::pair<TilePos, int>> frontier;
    const auto survey = [&](TilePos p, int distance) {
        p = canonical(p);
        if (p.x < 0 || p.z < 0 || p.y < 0 || p.y >= WORLD_HEIGHT_Y ||
            surveyed.size() >= cellLimit || !capacity(p.y, 0) ||
            surveyed.contains(p) || access(p) != Reservoirs::CellAccess::Open)
            return;
        surveyed.insert(p);
        frontier.emplace_back(p, distance);
    };
    survey(mouth, 0);
    if (frontier.empty()) return 0;
    for (size_t i = 0; i < frontier.size() && surveyed.size() < cellLimit;
         ++i) {
        const auto [p, distance] = frontier[i];
        if (uint64_t(distance + 1) >= distanceLimit) continue;
        for (const auto q : faces(p)) survey(q, distance + 1);
    }
    std::unordered_set<TilePos> seen;
    WaterVolume moved = 0;
    const auto enqueue = [&](TilePos p, int distance) {
        p = canonical(p);
        if (uint64_t(distance) >= distanceLimit || !surveyed.contains(p) ||
            !capacity(p.y, moved) || !seen.insert(p).second)
            return false;
        pending.emplace(p.y, distance, p.z, p.x);
        return true;
    };
    enqueue(mouth, 0);
    while (!pending.empty() && remaining > 0) {
        const auto [y, distance, z, x] = pending.top();
        pending.pop();
        const TilePos p{x, y, z};
        // Let a newly discovered drop fill before its downspout.
        const TilePos below{x, y - 1, z};
        if (enqueue(below, distance + 1)) {
            pending.emplace(y, distance, z, x);
            continue;
        }
        for (const auto q : {TilePos{x - 1, y, z}, TilePos{x + 1, y, z},
                             TilePos{x, y, z - 1}, TilePos{x, y, z + 1}})
            enqueue(q, distance + 1);
        const auto& cells = next ? next->cells : current.cells;
        const int fill = cells.fill(p);
        auto accepted =
            std::clamp<WaterVolume>(capacity(y, moved) - fill, 0, remaining);
        // A finite source may recede during this very transfer.
        if (accepted && fill + accepted > capacity(y, moved + accepted)) {
            WaterVolume low = 0, high = accepted;
            while (low < high) {
                const auto mid = (low + high + 1) / 2;
                if (fill + mid <= capacity(y, moved + mid))
                    low = mid;
                else
                    high = mid - 1;
            }
            accepted = low;
        }
        if (accepted) {
            if (!next) next = std::make_shared<ReservoirSnapshot>(current);
            next->cells.set(p, uint8_t(fill + accepted));
            change.locals.push_back(p);
            remaining -= accepted;
            moved += accepted;
        }
        if (fill + accepted == WaterUnitsPerBlock)
            enqueue({x, y + 1, z}, distance + 1);
    }
    return moved;
}
}  // namespace

WaterVolume Reservoirs::drainCut(uint32_t body, TilePos mouth, uint64_t elapsed,
                                 const CellQuery& access, int periodX) {
    const auto* shape = curve(body);
    if (!shape || !elapsed || access(mouth) != CellAccess::Open) return 0;
    const auto before = volume(body);
    const auto available = std::min(before - shape->volumeAt(mouth.y),
                                    scale(WaterUnitsPerBlock, elapsed));
    if (available <= 0) return 0;
    const int restingLayer =
        generated && (body == GeneratedHydrology::Sea || generated->basin(body))
            ? shape->levelAt(shape->capacity()) - 1
            : -1;
    const auto capacity = [&](int y, WaterVolume moved) -> int {
        // Water that has already crossed the sill can fall even as the
        // reservoir runs dry. Below the sill there is no head constraint.
        if (y < mouth.y) return WaterUnitsPerBlock;
        const auto left = before - moved;
        if (left <= shape->volumeAt(y)) return 0;
        const int fill =
            left >= shape->volumeAt(y + 1) ? 255 : shape->fillAtSurface(left);
        return y == restingLayer
                   ? std::max(1, fill * WaterSurface::RestingFill / 255)
                   : fill;
    };
    const auto current = snapshot();
    const int fill = current->cells.fill(mouth);
    // Keep the one-second intake at the mouth until it backs up. A backed-up
    // opening can transmit head through a filled tunnel in either time mode.
    const bool backedUp =
        current->cells.fill({mouth.x, mouth.y - 1, mouth.z}) == 255;
    const auto distanceLimit =
        elapsed == 1 &&
                (backedUp || fill >= std::max(1, capacity(mouth.y, 0) - 1))
            ? MaxVolume
            : elapsed;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    const auto moved =
        fillCut(*current, next, change, mouth, available, distanceLimit,
                elapsed == 1 ? 512 : 4096, access, periodX, capacity);
    if (!moved) return 0;
    stores[body] = before - moved;
    ++serial;
    updateLevel(*next, *shape, body, change);
    publish(std::move(next), change);
    return moved;
}

WaterVolume Reservoirs::creditLocal(TilePos recipient, WaterVolume amount,
                                    ReservoirExchange reason) {
    if (reason != ReservoirExchange::Input || amount <= 0 || recipient.x < 0 ||
        recipient.y < 0 || recipient.y >= WORLD_HEIGHT_Y || recipient.z < 0)
        return 0;
    const auto current = snapshot();
    const int fill = current->cells.fill(recipient);
    const auto accepted = std::min<WaterVolume>(amount, 255 - fill);
    if (!accepted) return 0;
    auto next = std::make_shared<ReservoirSnapshot>(*current);
    next->cells.set(recipient, uint8_t(fill + accepted));
    totals[size_t(reason)] = add(totals[size_t(reason)], accepted);
    ++serial;
    ReservoirChange change;
    change.local = recipient;
    publish(std::move(next), change);
    return accepted;
}

bool Reservoirs::setReachDelivery(uint32_t reach, unsigned percent,
                                  uint32_t receiver) {
    if (!springRates.contains(reach) || percent > 100 ||
        (receiver && !curve(receiver)))
        return false;
    auto proposed = deliveries;
    auto& delivery = proposed[reach];
    if (delivery.percent == percent && delivery.receiver == receiver)
        return true;
    delivery.percent = percent;
    delivery.receiver = receiver;
    auto newOrder = routes(proposed);
    if (!newOrder) return false;
    order = std::move(*newOrder);
    deliveries = std::move(proposed);
    ++serial;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    updateRiverStage(reach, next, change);
    if (next) publish(std::move(next), change);
    return true;
}

bool Reservoirs::setSpringOpen(uint32_t reach, bool open) {
    const auto it = springRates.find(reach);
    if (it == springRates.end() || !it->second) return false;
    if (!(open ? sealedSprings.erase(reach)
               : sealedSprings.insert(reach).second))
        return true;
    if (open) {
        springReopenedAt[reach] = budgetStep;
    } else {
        springReopenedAt.erase(reach);
        springClosedAt[reach] = budgetStep;
    }
    ++serial;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    updateRiverStage(reach, next, change);
    if (next) publish(std::move(next), change);
    return true;
}

std::optional<Reservoirs::BreachSurvey> Reservoirs::surveyBreach(
    Vec2i opening, int head, int periodX, const SurfaceFloor& floor,
    std::vector<TilePos>* route, ValleyOutlet* outlet,
    const SinkQuery& receiver) {
    if (!floor || head <= 0 || head > WORLD_HEIGHT_Y) return {};
    constexpr size_t Budget = 8192;
    const auto key = [](Vec2i p) {
        return ReservoirSnapshot::columnKey(p.x, p.y);
    };
    const auto neighbors = [&](Vec2i p) {
        std::array<Vec2i, 4> result{
            {{p.x - 1, p.y}, {p.x + 1, p.y}, {p.x, p.y - 1}, {p.x, p.y + 1}}};
        if (periodX > 0)
            for (auto& q : result) q.x = (q.x + periodX) % periodX;
        return result;
    };
    std::unordered_map<uint64_t, std::optional<int>> samples;
    const auto at = [&](Vec2i p) -> std::optional<int> {
        if (const auto it = samples.find(key(p)); it != samples.end())
            return it->second;
        if (samples.size() >= Budget) return {};
        return samples.emplace(key(p), floor(p)).first->second;
    };
    const auto inlet = at(opening);
    if (!inlet || *inlet >= head) return {};
    auto landing = opening;
    std::unordered_map<uint64_t, Vec2i> parents;
    if (route) route->clear();
    if (outlet) *outlet = {};
    // Find the nearest downhill exit on each flat. Don't start a lake in
    // an unrelated low spot across a sill that the stream hasn't reached.
    for (int elevation = *inlet;;) {
        std::vector<Vec2i> flat{landing};
        std::unordered_set<uint64_t> seen{key(landing)};
        bool descended = false;
        for (size_t i = 0; i < flat.size() && !descended; ++i)
            for (const auto q : neighbors(flat[i])) {
                const auto y = at(q);
                if (!y) return {};
                if (*y < elevation) {
                    if (route) parents[key(q)] = flat[i];
                    landing = q;
                    elevation = *y;
                    descended = true;
                    break;
                }
                if (*y == elevation && seen.insert(key(q)).second) {
                    if (route) parents[key(q)] = flat[i];
                    flat.push_back(q);
                }
            }
        if (!descended) break;
    }
    if (landing == opening && !route) return {};
    if (route) {
        for (auto p = landing; p != opening; p = parents.at(key(p))) {
            const auto parent = parents.at(key(p));
            for (int y = *at(p); y <= *at(parent); ++y) {
                if (route->size() >= Budget) return {};
                route->push_back({p.x, y, p.y});
            }
        }
    }
    // Certify the connected bowl and each column's admission height. If the
    // frontier is unknown or too large, do not invent a closed reservoir.
    using Entry = std::tuple<int, int, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pending;
    pending.emplace(*at(landing), landing.x, landing.y);
    std::unordered_set<uint64_t> seen{key(landing)};
    parents.clear();
    BreachSurvey result{head, {}};
    while (!pending.empty()) {
        const auto [sill, x, z] = pending.top();
        pending.pop();
        const Vec2i p{x, z};
        result.columns.push_back({p, *at(p), sill});
        for (const auto q : neighbors(p)) {
            if (!seen.insert(key(q)).second) continue;
            const auto y = at(q);
            if (!y) {
                if (!route) return {};
                result.head = sill;
                std::erase_if(result.columns, [sill](const auto& col) {
                    return col.sill >= sill;
                });
                if (result.columns.empty()) return {};
                if (outlet && receiver) {
                    const auto sink = receiver({q.x, sill, q.y});
                    if (sink && sink->reach && sink->surfaceY <= sill) {
                        outlet->receiver = *sink;
                        for (int y = sink->surfaceY - 1; y <= *at(p); ++y)
                            outlet->path.push_back({q.x, y, q.y});
                        for (auto v = p;; v = parents.at(key(v))) {
                            const int floorY = *at(v);
                            outlet->path.push_back({v.x, floorY, v.y});
                            if (v == landing) break;
                            const auto prev = parents.at(key(v));
                            for (int y = floorY + 1; y <= *at(prev); ++y)
                                outlet->path.push_back({v.x, y, v.y});
                            if (outlet->path.size() > 8193) {
                                *outlet = {};
                                break;
                            }
                        }
                    }
                }
                return result;
            }
            if (*y < head) {
                pending.emplace(std::max(sill, *y), q.x, q.y);
                if (outlet) parents[key(q)] = p;
            }
        }
    }
    return result;
}

StorageCurve Reservoirs::breachCurve(const BreachSurvey& survey) {
    StorageCurve::Levels storage{}, addedArea{}, addedFloors{};
    for (const auto& c : survey.columns) {
        ++addedArea[c.sill + 1];
        addedFloors[c.sill + 1] += c.floor;
    }
    WaterVolume area = 0, floors = 0;
    for (int y = 1; y <= survey.head; ++y) {
        area += addedArea[y];
        floors += addedFloors[y];
        storage[y] = (area * y - floors) * WaterUnitsPerBlock;
    }
    std::fill(storage.begin() + survey.head + 1, storage.end(),
              storage[survey.head]);
    return StorageCurve(std::move(storage));
}

// A flowing spillway needs only a shallow sheet above its sill.
WaterVolume Reservoirs::breachLimit(const Breach& breach) const {
    const auto& shape = curves.at(breach.pool);
    if (!breach.dam) return shape.capacity();
    const auto atSill = shape.volumeAt(breach.survey.head - 1);
    return atSill + (shape.capacity() - atSill + 15) / 16;
}

bool Reservoirs::hasBreach(TilePos p) const {
    const auto it = breaches.find(p);
    return it != breaches.end() && it->second.open;
}

bool Reservoirs::openBreach(uint32_t reach, TilePos p, BreachSurvey survey) {
    return openConnection(reach, p, std::move(survey), false);
}

bool Reservoirs::openDam(uint32_t reach, TilePos p, BreachSurvey survey,
                         const std::function<bool(TilePos)>& solid) {
    const auto before = snapshot();
    if (!openConnection(reach, p, std::move(survey), true)) return false;
    const auto current = snapshot();
    if (before == current) return true;
    const auto found = current->riverStages.find(reach);
    if (found == current->riverStages.end() || !found->second.outletLake)
        return true;
    const auto& stage = found->second;
    const auto body = stage.outletLake;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    // Transfer each newly isolated lake cell once. Its remaining water can
    // now run out through the mouth, or stay behind if completely enclosed.
    constexpr int radius = ReservoirSnapshot::RiverStage::DamRadius;
    WaterVolume remaining = 0;
    for (int dz = -radius; dz <= radius; ++dz)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int x = int(WorldWrap::mod(p.x + dx, stage.periodX)),
                      z = p.z + dz;
            if (!stage.isolatesLake(body, x, z)) continue;
            if (const auto old = before->riverStages.find(reach);
                old != before->riverStages.end() &&
                old->second.isolatesLake(body, x, z))
                continue;
            const auto [ground, base] = generated->sampleColumn(x, z);
            if (base.body != body || base.reach) continue;
            const auto water =
                before->projectStanding(generated.get(), x, z, ground, base);
            for (int y = ground + 1; y < water.surfaceY; ++y) {
                const TilePos cell{x, y, z};
                if (solid && solid(cell)) continue;
                if (!next) {
                    next = std::make_shared<ReservoirSnapshot>(*current);
                    remaining = volume(body);
                }
                const auto fill = next->cells.fill(cell);
                const int moved = int(std::min<WaterVolume>(
                    remaining,
                    std::max(0, int(water.fillAt(y, ground)) - fill)));
                if (!moved) continue;
                next->cells.set(cell, uint8_t(fill + moved));
                remaining -= moved;
                change.locals.push_back(cell);
            }
        }
    for (auto it = channels.begin(); it != channels.end();) {
        auto& channel = it->second;
        if (channel.from == body &&
            std::erase_if(channel.mouths, [&](const auto& entry) {
                const auto intake = entry.second.path.back();
                return stage.isolatesLake(body, intake.x, intake.z);
            }))
            channelPathsChanged = true;
        it = channel.mouths.empty() ? channels.erase(it) : std::next(it);
    }
    if (next && !change.locals.empty()) {
        stores[body] = remaining;
        updateLevel(*next, *curve(body), body, change);
        publish(std::move(next), change);
    }
    return true;
}

bool Reservoirs::connectValley(uint32_t source, TilePos gate, TilePos intake,
                               BreachSurvey survey, std::vector<TilePos> route,
                               ValleyOutlet outlet) {
    if (!generated || !source || survey.columns.empty() ||
        survey.columns.front().floor >= gate.y || survey.head <= 0 ||
        survey.head > WORLD_HEIGHT_Y ||
        !generated->containsColumn(gate.x, gate.z) ||
        !generated->containsColumn(intake.x, intake.z) || gate.y < 0 ||
        gate.y >= WORLD_HEIGHT_Y)
        return false;
    for (const auto& c : survey.columns)
        if (!generated->containsColumn(c.tile.x, c.tile.y) || c.floor < 0 ||
            c.sill < c.floor || c.sill >= survey.head)
            return false;
    route.push_back(gate);
    route.push_back(intake);
    if (route.size() > 8194) return false;
    const auto landing = survey.columns.front().tile;
    const auto key = ReservoirSnapshot::columnKey(landing.x, landing.y);
    const int crest = survey.head;
    const auto connect = [&](uint32_t pool) {
        if (!setChannelMouth(source, {pool, 0, -1}, gate, gate.y,
                             std::move(route)))
            return false;
        if (!outlet.path.empty() && outlet.receiver.reach &&
            outlet.receiver.surfaceY <= crest) {
            const auto spill = std::max_element(
                outlet.path.begin(), outlet.path.end(),
                [](TilePos a, TilePos b) { return a.y < b.y; });
            const auto mouth = *spill;
            if (mouth.y == crest && mouth != gate)
                setChannelMouth(pool, outlet.receiver, mouth, crest,
                                std::move(outlet.path));
        }
        return true;
    };
    for (const auto& [id, pool] : snapshot()->pools)
        if (pool.footprint->columns.contains(key))
            return pool.parent == source && connect(id);
    const auto existing = snapshot();
    for (const auto& c : survey.columns)
        for (const auto& [id, pool] : existing->pools)
            if (pool.footprint->columns.contains(
                    ReservoirSnapshot::columnKey(c.tile.x, c.tile.y)))
                return false;
    const TilePos seed{landing.x, survey.columns.front().floor, landing.y};
    if (!detachLake(source, seed, std::move(survey), 0)) return false;
    return connect(breaches.at(seed).pool);
}

bool Reservoirs::detachLake(uint32_t parent, TilePos seed, BreachSurvey survey,
                            WaterVolume trapped) {
    const auto* parentCurve = curve(parent);
    if (!parentCurve || breaches.contains(seed) || survey.columns.empty() ||
        trapped < 0 || trapped > volume(parent))
        return false;
    auto shape = breachCurve(survey);
    if (trapped > shape.capacity()) return false;
    uint32_t body = 0x80000000u;
    while (curves.contains(body)) ++body;
    auto geometry = std::make_shared<PoolFootprint>();
    auto& footprint = poolFootprints[body];
    for (const auto& c : survey.columns) {
        footprint.push_back(c.tile);
        geometry->add(c.tile, generated->periodX(), seed.x, c.floor, c.sill);
    }
    publishedFootprints.reset();
    curves.emplace(body, shape);
    stores[parent] = volume(parent) - trapped;
    stores[body] = trapped;
    auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
    next->pools.emplace(body, PoolState{-1, 255, std::move(geometry), parent});
    breaches.emplace(
        seed, Breach{0, body, std::move(survey), false, false, false, parent});
    ReservoirChange change;
    if (!trapped) {
        for (const auto& c : breaches.at(seed).survey.columns) {
            const auto x = c.tile.x, z = c.tile.y;
            snapshot()->cells.visitRow(x, z, z, [&](TilePos p, uint8_t fill) {
                if (p.y < c.floor || p.y >= breaches.at(seed).survey.head)
                    return;
                stores[body] += fill;
                change.locals.push_back(p);
                next->cells.set(p, 0);
            });
        }
    }
    updateLevel(*next, *parentCurve, parent, change);
    updateLevel(*next, shape, body, change);
    ++serial;
    publish(std::move(next), change);
    return true;
}

void Reservoirs::rejoinLake(uint32_t body) {
    const auto current = snapshot();
    const auto pool = current->pools.find(body);
    if (pool == current->pools.end() || !pool->second.parent) return;
    const uint32_t parent = pool->second.parent;
    auto volumeToReturn = volume(body);
    if (const auto pending = pendingInflow.find(body);
        pending != pendingInflow.end()) {
        volumeToReturn = add(volumeToReturn, pending->second);
        pendingInflow.erase(pending);
    }

    // Channels touching the old compartment need a new survey against the
    // parent lake's head. Local intakes will still run in the meantime.
    channelPathsChanged |=
        std::erase_if(channels, [body](const auto& entry) {
            return entry.second.from == body || entry.second.to.body == body;
        }) != 0;
    std::erase_if(breaches, [body](const auto& entry) {
        return entry.second.pool == body;
    });
    stores.erase(body);
    curves.erase(body);
    publishedFootprints.reset();

    auto next = std::make_shared<ReservoirSnapshot>(*current);
    next->pools.erase(body);
    for (auto& [cell, target] : next->portals)
        if (target == body) target = parent;
    auto footprint = poolFootprints.extract(body);
    ReservoirChange change;
    change.bodies.push_back(body);
    auto removedFootprint =
        std::make_shared<std::map<uint32_t, std::vector<Vec2i>>>();
    removedFootprint->insert(std::move(footprint));
    change.pools = std::move(removedFootprint);
    receive({parent}, volumeToReturn, next, change);
    ++serial;
    publish(std::move(next), change);
}

bool Reservoirs::hasLakeOutletFlow(WaterSurface source) const {
    if (!source.reach) return lakeOutletReaches.contains(source.body);
    const auto it = reachIndices.find(source.reach);
    return it != reachIndices.end() &&
           generated->riverReaches()[it->second].lakeSource.has_value();
}

std::optional<size_t> Reservoirs::lakeOutletIndex(uint32_t body) const {
    const auto it = lakeOutletReaches.find(body);
    if (it == lakeOutletReaches.end()) return {};
    return reachIndices.at(it->second);
}

std::optional<TilePos> Reservoirs::damAt(uint32_t reach) const {
    for (const auto& [p, breach] : breaches)
        if (breach.reach == reach && breach.open && breach.dam) return p;
    return {};
}

std::vector<std::pair<uint32_t, TilePos>> Reservoirs::damSites() const {
    std::vector<std::pair<uint32_t, TilePos>> result;
    for (const auto& [p, breach] : breaches)
        if (breach.dam) result.emplace_back(breach.reach, p);
    return result;
}

bool Reservoirs::openConnection(uint32_t reach, TilePos p, BreachSurvey survey,
                                bool dam) {
    if (!springRates.contains(reach) || survey.head <= (dam ? 0 : p.y) ||
        survey.head > WORLD_HEIGHT_Y || survey.columns.empty() ||
        survey.columns.size() > 8192 || p.y < 0 || p.y >= WORLD_HEIGHT_Y ||
        !generated->containsColumn(p.x, p.z))
        return false;
    auto existing = breaches.find(p);
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    if (auto it = existing; it != breaches.end()) {
        if (it->second.reach != reach || it->second.dam != dam) return false;
        const auto& old = it->second.survey;
        const bool sameSide = old.damUpstream == survey.damUpstream ||
                              (old.damUpstream && survey.damUpstream &&
                               *old.damUpstream == *survey.damUpstream);
        const bool same = old.head == survey.head &&
                          old.columns == survey.columns && sameSide;
        if (!same && !dam) return false;
        if (same) {
            if (it->second.open) return true;
            it->second.open = true;
            it->second.closedAt = budgetStep;
            ++serial;
            if (dam) {
                next = std::make_shared<ReservoirSnapshot>(*snapshot());
                next->pools.at(it->second.pool).closedCrest = -1;
                change.bodies.push_back(it->second.pool);
            }
            updateRiverStage(reach, next, change);
            if (next) publish(std::move(next), change);
            return true;
        }
    }
    {
        const bool replacing = existing != breaches.end();
        uint32_t body = replacing ? existing->second.pool : 0x80000000u;
        if (!replacing)
            while (curves.contains(body)) ++body;
        std::vector<Vec2i> footprint;
        footprint.reserve(survey.columns.size());
        auto geometry = std::make_shared<PoolFootprint>();
        const auto current = snapshot();
        for (const auto& c : survey.columns) {
            const auto key = ReservoirSnapshot::columnKey(c.tile.x, c.tile.y);
            if (!generated->containsColumn(c.tile.x, c.tile.y) || c.floor < 0 ||
                c.sill < c.floor || c.sill >= survey.head ||
                geometry->columns.contains(key))
                return false;
            // A second cut needs to connect to the same store, not certify
            // and fill identical capacity a second time. Until merging is
            // supported, do not create an overlapping terminal lake.
            if (!dam &&
                std::any_of(current->pools.begin(), current->pools.end(),
                            [&](const auto& entry) {
                                return entry.second.footprint->columns.contains(
                                    key);
                            }))
                return false;
            geometry->add(c.tile, generated->periodX(), p.x, c.floor, c.sill);
            footprint.push_back(c.tile);
        }
        const auto shape = breachCurve(survey);
        if (!shape.capacity()) return false;
        next = std::make_shared<ReservoirSnapshot>(*current);
        const auto before = replacing ? volume(body) : 0;
        const auto priorStage = current->riverStages.find(reach);
        const uint64_t closedAt =
            replacing && existing->second.open &&
                    priorStage != current->riverStages.end() &&
                    priorStage->second.damAge
                ? existing->second.closedAt
                : budgetStep;
        if (replacing)
            for (const auto& c : existing->second.survey.columns)
                for (int slab = c.floor / Chunk::SIZE_Y;
                     slab <= (existing->second.survey.head - 1) / Chunk::SIZE_Y;
                     ++slab)
                    change.locals.push_back(
                        {c.tile.x, slab * Chunk::SIZE_Y, c.tile.y});
        curves.insert_or_assign(body, shape);
        stores[body] = std::min(before, shape.capacity());
        if (before > stores.at(body))
            deliveries[reach].held =
                add(deliveries[reach].held, before - stores.at(body));
        poolFootprints.insert_or_assign(body, std::move(footprint));
        publishedFootprints.reset();
        next->pools.insert_or_assign(body,
                                     PoolState{-1, 255, std::move(geometry)});
        updateLevel(*next, shape, body, change);
        breaches.insert_or_assign(p, Breach{reach, body, std::move(survey),
                                            true, false, dam, 0, closedAt});
    }
    ++serial;
    updateRiverStage(reach, next, change);
    if (next) publish(std::move(next), change);
    return true;
}

bool Reservoirs::closeBreach(TilePos p) {
    const auto it = breaches.find(p);
    if (it == breaches.end() || !it->second.open) return false;
    it->second.open = false;
    ++serial;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    if (it->second.dam) {
        next = std::make_shared<ReservoirSnapshot>(*snapshot());
        next->pools.at(it->second.pool).closedCrest =
            it->second.survey.head - 1;
        change.bodies.push_back(it->second.pool);
    }
    updateRiverStage(it->second.reach, next, change);
    if (next) publish(std::move(next), change);
    return true;
}

const std::vector<Reservoirs::JunctionColumn>& Reservoirs::junctionBanks(
    uint32_t reach) {
    auto [entry, inserted] = junctionColumns.try_emplace(reach);
    if (!inserted) return entry->second;
    const auto& route = generated->riverReaches()[reachIndices.at(reach)];
    // A tributary can stop beside the receiving channel when that channel
    // is above its grade. The delivery graph stays downhill, but water can
    // still enter the lower reach across their shared edge.
    std::vector<Vec2i> joins{route.outlet};
    if (const auto it = tributaries.find(reach); it != tributaries.end())
        for (auto index : it->second)
            joins.push_back(generated->riverReaches()[index].outlet);
    std::set<std::pair<int, int>> sampled;
    for (auto join : joins)
        for (int dz = -16; dz <= 16; ++dz)
            for (int dx = -16; dx <= 16; ++dx) {
                const int x =
                    int(WorldWrap::mod(join.x + dx, generated->periodX()));
                const int z = join.y + dz;
                const auto other = generated->riverAt(x, z);
                if (!other || other->water.reach == reach ||
                    other->distance > other->radius)
                    continue;
                for (auto d :
                     std::array<Vec2i, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                    const auto own = generated->riverAt(x + d.x, z + d.y);
                    if (!own || own->water.reach != reach ||
                        own->distance > own->radius)
                        continue;
                    if (sampled.emplace(x, z).second) {
                        const auto [ground, water] =
                            generated->sampleRiverColumn(x, z);
                        if (water.reach == other->water.reach)
                            entry->second.push_back({{x, z}, ground, water});
                    }
                    break;
                }
            }
    return entry->second;
}

void Reservoirs::updateRiverStage(uint32_t reach,
                                  std::shared_ptr<ReservoirSnapshot>& next,
                                  ReservoirChange& change) {
    ReservoirSnapshot::RiverStage stage;
    const auto& route = generated->riverReaches()[reachIndices.at(reach)];
    if (const auto it = deliveries.find(reach); it != deliveries.end())
        stage.percent = it->second.percent;
    bool hasBreachStage = false;
    for (const auto& [p, breach] : breaches)
        if (breach.reach == reach && breach.open &&
            (volume(breach.pool) < breachLimit(breach) ||
             (breach.dam && route.lakeSource))) {
            if (breach.dam && stage.percent) {
                const auto channel = generated->riverAt(p.x, p.z);
                if (!channel) continue;
                stage.head = channel->water.surfaceY;
                stage.percent = 0;
                stage.cut = p;
                stage.downstream = channel->water.flow;
                stage.periodX = generated->periodX();
                stage.damUpstream = breach.survey.damUpstream;
                if (volume(breach.pool) >= breachLimit(breach))
                    stage.percent = 100;
                else
                    stage.damAge =
                        std::min(MaxRiverAge, (budgetStep - breach.closedAt) /
                                                  RiverStageInterval *
                                                  RiverStageInterval);
                if (route.lakeSource) stage.outletLake = route.lakeSource->body;
                break;
            }
            if (stage.percent == 100 || hasBreachStage) {
                stage.head = hasBreachStage
                                 ? std::max(stage.head, breach.survey.head)
                                 : breach.survey.head;
                stage.percent /= 2;
                if (!hasBreachStage) {
                    const int period = generated->periodX();
                    for (const auto d : std::array<Vec2i, 4>{
                             {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                        const auto channel = generated->riverAt(
                            int(WorldWrap::mod(p.x + d.x, period)), p.z + d.y);
                        if (!channel || channel->water.reach != reach) continue;
                        stage.cut = p;
                        stage.periodX = period;
                        stage.downstream = channel->water.flow;
                        break;
                    }
                }
                hasBreachStage = true;
            }
        }
    stage.receding = sealedSprings.contains(reach);
    const auto reopened = springReopenedAt.find(reach);
    stage.recovering = reopened != springReopenedAt.end();
    if (route.lakeSource) {
        const auto body = route.lakeSource->body;
        if (const auto store = stores.find(body); store != stores.end()) {
            const auto& shape = curves.at(body);
            stage.lakeSupplyPercent =
                shape.levelAt(store->second) == shape.levelAt(shape.capacity())
                    ? unsigned(shape.fillAtSurface(store->second)) * 100 / 255
                    : 0;
        }
    }
    if (stage.receding || stage.recovering || stage.lakeSupplyPercent < 100 ||
        stage.damAge) {
        stage.spring =
            route.lakeSource ? route.lakeSource->tile : route.spring.tile;
        stage.periodX = generated->periodX();
        // Coalesce projection invalidations rather than remeshing on each
        // fixed second. A v5 seal without a timestamp has already drained.
        const auto closed = springClosedAt.find(reach);
        const auto age = [](uint64_t elapsed) {
            return std::min(MaxRiverAge,
                            elapsed / RiverStageInterval * RiverStageInterval);
        };
        stage.recessionAge =
            closed == springClosedAt.end()
                ? MaxRiverAge
                : age((stage.recovering ? reopened->second : budgetStep) -
                      closed->second);
        if (stage.recovering)
            stage.recoveryAge = age(budgetStep - reopened->second);
        if (const auto it = tributaries.find(reach); it != tributaries.end())
            for (size_t index : it->second) {
                const auto& branch = generated->riverReaches()[index];
                auto rate = springRates.at(branch.id);
                if (branch.lakeSource) {
                    rate = naturalReachRates.at(branch.id);
                    const auto lake = branch.lakeSource->body;
                    if (stores.contains(lake)) {
                        const auto& shape = curves.at(lake);
                        const auto stored = stores.at(lake);
                        rate = shape.levelAt(stored) ==
                                       shape.levelAt(shape.capacity())
                                   ? rate * shape.fillAtSurface(stored) / 255
                                   : 0;
                    }
                }
                if (!rate || sealedSprings.contains(branch.id)) continue;
                if (const auto open = springReopenedAt.find(branch.id);
                    open != springReopenedAt.end()) {
                    const auto delay = riverDelay(branch.spring.tile,
                                                  branch.outlet, stage.periodX);
                    rate = rate *
                           riverTransition(budgetStep - open->second, delay) /
                           100;
                }
                if (std::any_of(breaches.begin(), breaches.end(),
                                [&](const auto& entry) {
                                    const auto& b = entry.second;
                                    return b.reach == branch.id && b.dam &&
                                           b.open &&
                                           volume(b.pool) < breachLimit(b);
                                }))
                    continue;
                if (const auto delivery = deliveries.find(branch.id);
                    delivery != deliveries.end()) {
                    if (delivery->second.receiver) continue;
                    rate = rate * delivery->second.percent / 100;
                }
                if (!rate) continue;
                const auto junction =
                    generated->riverAt(branch.outlet.x, branch.outlet.y);
                if (!junction || junction->water.reach != reach) continue;
                const auto flow = junction->water.flow;
                const Vec2i downstream = std::abs(flow.x) >= std::abs(flow.y)
                                             ? Vec2i{flow.x > 0 ? 1 : -1, 0}
                                             : Vec2i{0, flow.y > 0 ? 1 : -1};
                const auto normal = naturalReachRates.at(reach);
                const auto supply =
                    std::clamp<WaterVolume>(rate * 100 / normal, 1, 100);
                stage.inlets.push_back({branch.outlet, downstream,
                                        junction->water.surfaceY,
                                        unsigned(supply)});
            }
        const auto current = snapshot();
        const auto& projection = next ? *next : *current;
        for (const auto& bank : junctionBanks(reach)) {
            const auto wet = projection.projectStanding(
                generated.get(), bank.tile.x, bank.tile.y, bank.ground,
                bank.water, false);
            if (wet.wet())
                stage.banks.push_back(
                    {bank.tile, wet.topTile() * 255 + wet.topFill, wet.reach});
        }
    }
    const auto view = snapshot();
    const auto* current = next ? next.get() : view.get();
    const auto it = current->riverStages.find(reach);
    const bool natural = stage.percent == 100 && !stage.receding &&
                         !stage.recovering && stage.lakeSupplyPercent == 100 &&
                         !stage.outletLake;
    if (natural ? it == current->riverStages.end()
                : it != current->riverStages.end() && it->second == stage)
        return;
    const bool lakeMaskChanged =
        route.lakeSource &&
        (it == current->riverStages.end()
             ? stage.outletLake != 0
             : it->second.outletLake != stage.outletLake ||
                   it->second.damUpstream != stage.damUpstream);
    if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
    if (natural)
        next->riverStages.erase(reach);
    else
        next->riverStages[reach] = stage;
    change.reaches.push_back(reach);
    if (lakeMaskChanged) change.bodies.push_back(route.lakeSource->body);
    if (route.end == GeneratedHydrology::RiverEnd::Lake &&
        next->levels.contains(route.receiver))
        change.bodies.push_back(route.receiver);
}

WaterVolume Reservoirs::held(uint32_t reach) const {
    const auto it = deliveries.find(reach);
    return it == deliveries.end() ? 0 : it->second.held;
}

bool Reservoirs::definePool(uint32_t body, const StorageCurve& shape,
                            std::vector<Vec2i> footprint) {
    if (!body || footprint.empty() || curves.contains(body) ||
        !shape.capacity() || poolFootprints.contains(body))
        return false;
    curves.emplace(body, shape);
    poolFootprints.emplace(body, footprint);
    publishedFootprints.reset();
    // The pool starts empty: a store entry at zero overrides the generated
    // "full by default" convention for bodies without one.
    stores.emplace(body, 0);
    auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
    auto geometry = std::make_shared<PoolFootprint>();
    for (const auto& tile : footprint)
        geometry->add(tile, generated ? generated->periodX() : 0,
                      footprint.front().x);
    next->pools.emplace(body, PoolState{-1, 255, std::move(geometry)});
    ++serial;
    publish(std::move(next), ReservoirChange{});
    return true;
}

bool Reservoirs::setSpillRoute(uint32_t reach, uint32_t poolBody,
                               uint32_t receiver, int height,
                               WaterVolume ratePerStep) {
    if (!reach || !poolBody || !receiver || !curve(poolBody) ||
        !curve(receiver) || height <= 0 || height >= WORLD_HEIGHT_Y ||
        ratePerStep <= 0)
        return false;
    auto& spill = spills[reach];
    spill.pool = poolBody;
    spill.body = receiver;
    spill.height = height;
    spill.rate = ratePerStep;
    ++serial;
    return true;
}

std::optional<int> Reservoirs::spillHeight(uint32_t reach) const {
    const auto it = spills.find(reach);
    if (it == spills.end()) return std::nullopt;
    return it->second.height;
}

uint32_t Reservoirs::registerOutlet(uint32_t reachId, Vec2i rockSite, int rockY,
                                    TilePos face, WaterVolume ratePerStep) {
    if (ratePerStep <= 0) return 0;
    // One generated rock site, one outlet: a second exposed face or a chunk
    // reload adopts the existing key instead of duplicating the source. The
    // flow follows the newest exposed face; reopening re-opens the outlet.
    for (auto& [id, outlet] : outlets)
        if (outlet.reach == reachId) {
            outlet.face = face;
            outlet.open = true;
            return id;
        }
    const auto id = nextOutletId + 1;
    outlets[id] =
        Outlet{id, reachId, rockSite, face, rockY, ratePerStep, 0, 0, true};
    nextOutletId = id;
    ++serial;
    return id;
}

bool Reservoirs::closeAtRock(int x, int z, int rockY) {
    for (auto& [id, outlet] : outlets)
        if (outlet.rock.x == x && outlet.rock.y == z && outlet.rockY == rockY &&
            outlet.open)
            return closeOutlet(id);
    return false;
}

bool Reservoirs::closeOutlet(uint32_t outletId) {
    const auto it = outlets.find(outletId);
    if (it == outlets.end() || !it->second.open) return false;
    it->second.open = false;
    ++serial;
    return true;
}

WaterVolume Reservoirs::outletRate(uint32_t outletId) const {
    const auto it = outlets.find(outletId);
    return it == outlets.end() || !it->second.open ? 0 : it->second.rate;
}

bool Reservoirs::setPortal(TilePos cell, uint32_t body) {
    if (cell.x < 0 || cell.y < 0 || cell.y >= WORLD_HEIGHT_Y || cell.z < 0 ||
        !curve(body))
        return false;
    auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
    next->portals[cell] = body;
    ++serial;
    ReservoirChange change;
    change.local = cell;
    publish(std::move(next), change);
    return true;
}

bool Reservoirs::clearPortal(TilePos cell) {
    const auto current = snapshot();
    if (!current->portals.contains(cell)) return false;
    auto next = std::make_shared<ReservoirSnapshot>(*current);
    next->portals.erase(cell);
    ++serial;
    ReservoirChange change;
    change.local = cell;
    publish(std::move(next), change);
    return true;
}

void Reservoirs::reconcile() {
    bool any = false;
    // Open aquifer outlets feed surface cells at their registered face. The
    // credit is the same boundary input as creditLocal, so repeated small
    // deliveries keep their volume and the Input total stays exact.
    for (auto& [id, outlet] : outlets) {
        if (!outlet.open || outlet.rate <= 0) continue;
        const auto accepted =
            creditLocal(outlet.face, outlet.rate, ReservoirExchange::Input);
        (void)id;
        any = any || accepted > 0;
    }
    // Portal exchanges run before reach delivery each step: a confirmed
    // connection moves volume through the ledger in one operation, without
    // loading the voxels between the pocket and its receiver.
    const auto current = snapshot();
    if (current->portals.empty()) {
        (void)any;
        return;
    }
    auto next = std::make_shared<ReservoirSnapshot>(*current);
    ReservoirChange change;
    bool moved = any;
    for (const auto& [cell, body] : current->portals) {
        const auto* shape = curve(body);
        if (!shape) continue;
        const int fill = next->cells.fill(cell);
        // The cell's head is its fill fraction of one block above the tile
        // floor (cell.y). The body's surface is its level. Water moves
        // toward the lower head: an empty cell fills only while the body's
        // surface covers the tile, and a full cell drains once the surface
        // drops below the tile.
        const int level = next->level(body, shape->levelAt(shape->capacity()));
        if (level <= cell.y) {
            // The reservoir stands below this tile entirely: the pocket
            // drains its remainder into it, up to the body's capacity.
            if (fill) {
                const auto before = volume(body);
                const auto accepted =
                    std::min<WaterVolume>(1, shape->capacity() - before);
                if (accepted > 0) {
                    if (fill == 1)
                        next->cells.set(cell, 0);
                    else
                        next->cells.set(cell, uint8_t(fill - 1));
                    stores[body] = before + accepted;
                    totals[size_t(ReservoirExchange::River)] =
                        add(totals[size_t(ReservoirExchange::River)], accepted);
                    updateLevel(*next, *shape, body, change);
                    change.locals.push_back(cell);
                    moved = true;
                }
            }
            continue;
        }
        const bool refill = level > cell.y + 1 || !fill;
        const auto delta = refill ? std::min<WaterVolume>(255 - fill, 1)
                                  : -std::min<WaterVolume>(fill, 1);
        if (!delta) continue;
        const auto before = volume(body);
        const auto net = -delta;
        if (net > before) continue;
        stores[body] = before + net;
        if (delta > 0) {
            next->cells.set(cell, uint8_t(fill + int(delta)));
        } else if (fill == 1) {
            next->cells.set(cell, 0);
        } else {
            next->cells.set(cell, uint8_t(fill - 1));
        }
        totals[size_t(ReservoirExchange::River)] =
            add(totals[size_t(ReservoirExchange::River)], delta);
        updateLevel(*next, *shape, body, change);
        change.locals.push_back(cell);
        moved = true;
    }
    if (moved) {
        ++serial;
        publish(std::move(next), change);
    }
}

void Reservoirs::wakeLocalBox(TilePos lo, TilePos hi, int periodX) {
    const auto state = snapshot();
    if (state->cells.empty()) return;
    for (int x = lo.x; x <= hi.x; ++x) {
        int wx = periodX > 0 ? int(WorldWrap::mod(x, periodX)) : x;
        state->cells.visitRow(wx, lo.z, hi.z, [&](TilePos p, uint8_t) {
            if (p.y >= lo.y && p.y <= hi.y) activeCells.insert(p);
        });
    }
}

size_t Reservoirs::flow(size_t budget, const CellQuery& access, int periodX,
                        const SinkQuery& sink) {
    if (!budget || !access) return 0;
    const auto current = snapshot();
    if (activeCells.empty()) return 0;
    auto cursor =
        flowCursor ? activeCells.upper_bound(*flowCursor) : activeCells.begin();
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    const auto visited = std::min(budget, activeCells.size());
    auto fillAt = [&](TilePos p) -> int {
        const auto& cells = next ? next->cells : current->cells;
        return cells.fill(p);
    };
    for (size_t i = 0; i < visited; ++i) {
        if (cursor == activeCells.end()) cursor = activeCells.begin();
        const TilePos source = *cursor;
        cursor = activeCells.erase(cursor);
        flowCursor = source;
        if (!fillAt(source) || access(source) != CellAccess::Open) continue;
        const auto canonical = [periodX](TilePos p) {
            if (periodX > 0) {
                p.x %= periodX;
                if (p.x < 0) p.x += periodX;
            }
            return p;
        };
        const auto open = [&](TilePos p) {
            return p.x >= 0 && p.z >= 0 && p.y >= 0 && p.y < WORLD_HEIGHT_Y &&
                   access(p) == CellAccess::Open;
        };
        const auto remove = [&](int amount) {
            const int from = fillAt(source);
            if (!next) next = std::make_shared<ReservoirSnapshot>(*current);
            if (from == amount)
                next->cells.set(source, 0);
            else
                next->cells.set(source, uint8_t(from - amount));
            change.locals.push_back(source);
        };
        const auto absorb = [&](TilePos dest, int amount) {
            if (!sink || amount <= 0) return false;
            const auto target = sink(dest);
            if (!target) return false;
            remove(amount);
            receive(*target, amount, next, change);
            return true;
        };
        if (absorb(source, fillAt(source))) continue;
        const auto transfer = [&](TilePos dest, bool downward) {
            dest = canonical(dest);
            if (dest == source || dest.y < 0) return;
            const int from = fillAt(source);
            const int offered =
                downward ? from : std::clamp((from - fillAt(dest)) / 2, 0, 32);
            if (!offered) return;
            if ((downward && absorb(dest, offered)) || !open(dest) ||
                fillAt(dest) == 255)
                return;
            const int entryY = dest.y;
            // A parcel can fall through a loaded air column in one sweep.
            // Never leap over a full cell, a solid block or an unloaded slab.
            for (TilePos below{dest.x, dest.y - 1, dest.z}; below.y >= 0;
                 --below.y) {
                if (absorb(below, offered)) return;
                if (!open(below) || fillAt(below) == 255) break;
                dest = below;
            }
            int remaining = offered;
            for (; remaining && dest.y <= entryY; ++dest.y) {
                const int to = fillAt(dest);
                const int amount = std::min(remaining, 255 - to);
                remove(amount);
                next->cells.set(dest, uint8_t(to + amount));
                change.locals.push_back(dest);
                remaining -= amount;
            }
        };
        transfer({source.x, source.y - 1, source.z}, true);
        const int film = fillAt(source);
        const TilePos above{source.x, source.y + 1, source.z};
        if (film > 0 && film < 20 && open(above) && !fillAt(above) &&
            !current->portals.contains(source)) {
            activeCells.insert(source);
            const auto since = filmSince.try_emplace(source, budgetStep).first;
            if (budgetStep - since->second >= 300) {
                remove(film);
                totals[size_t(ReservoirExchange::Evaporation)] -= film;
                continue;
            }
        } else {
            filmSince.erase(source);
        }
        if (film < 2) continue;
        // Alternating axes avoids a fixed directional preference across steps.
        const int sign = (budgetStep & 1) ? -1 : 1;
        const std::array<Vec2i, 4> directions{
            {{sign, 0}, {0, sign}, {-sign, 0}, {0, -sign}}};
        struct Search {
            TilePos cell, first;
            int distance;
        };
        constexpr int Radius = 3;
        std::array<Search, 1 + 2 * Radius*(Radius + 1)> pending{};
        pending[0] = {source, source, 0};
        size_t count = 1;
        std::optional<TilePos> preferred;
        for (size_t i = 0; i < count && !preferred; ++i) {
            const auto [at, first, distance] = pending[i];
            const TilePos below{at.x, at.y - 1, at.z};
            if (distance && open(below) && fillAt(below) < 255) {
                preferred = first;
                break;
            }
            if (distance == Radius) continue;
            for (const auto d : directions) {
                const auto p = canonical({at.x + d.x, at.y, at.z + d.y});
                if (std::any_of(pending.begin(), pending.begin() + count,
                                [&](const Search& e) { return e.cell == p; }) ||
                    fillAt(p) + 1 >= fillAt(source) || !open(p))
                    continue;
                pending[count++] = {p, distance ? first : p, distance + 1};
            }
        }
        if (preferred)
            transfer(*preferred, false);
        else
            for (const auto d : directions)
                transfer({source.x + d.x, source.y, source.z + d.y}, false);
    }
    if (!next) return visited;
    ++serial;
    publish(std::move(next), change);
    return visited;
}

void Reservoirs::receive(Sink to, WaterVolume amount,
                         std::shared_ptr<ReservoirSnapshot>& next,
                         ReservoirChange& change) {
    if (amount <= 0) return;
    if (to.reach) {
        // Joins the reach's delivery on the next budget step.
        if (!springRates.contains(to.reach)) return;
        auto& delivery = deliveries[to.reach];
        delivery.held = add(delivery.held, amount);
        return;
    }
    // A full body (no store entry) passes arrivals over its spill without
    // building its curve; the sea's curve is expensive on first use.
    if (const auto it = stores.find(to.body); it != stores.end()) {
        if (const auto* shape = curve(to.body)) {
            const auto accepted =
                std::min(amount, shape->capacity() - it->second);
            if (accepted > 0) {
                it->second += accepted;
                amount -= accepted;
                if (!next)
                    next = std::make_shared<ReservoirSnapshot>(*snapshot());
                updateLevel(*next, *shape, to.body, change);
                if (it->second == shape->capacity()) stores.erase(it);
            }
        }
    }
    if (amount > 0)
        pendingInflow[to.body] = add(pendingInflow[to.body], amount);
}

bool Reservoirs::setChannelMouth(uint32_t from, Sink to, TilePos mouth,
                                 int threshold, std::vector<TilePos> path) {
    if (!from || from == to.body ||
        (!to.body && !to.reach &&
         (to.surfaceY < 0 || to.surfaceY >= mouth.y)) ||
        (to.reach && !springRates.contains(to.reach)) || !curve(from) ||
        path.empty() || path.size() > 8194 || threshold < mouth.y ||
        threshold < to.surfaceY || threshold >= WORLD_HEIGHT_Y)
        return false;
    // A mouth feeds one channel; a new survey may find a different receiver.
    const ChannelKey key{from, to.body, to.reach};
    for (auto it = channels.begin(); it != channels.end();) {
        if (it->first != key) it->second.mouths.erase(mouth);
        it = it->second.mouths.empty() && it->first != key ? channels.erase(it)
                                                           : std::next(it);
    }
    auto& channel = channels[key];
    channel.from = from;
    channel.to = to;
    Channel::Mouth certificate{threshold, std::move(path)};
    auto [entry, inserted] =
        channel.mouths.try_emplace(mouth, std::move(certificate));
    if (!inserted) {
        if (entry->second == certificate) return true;
        entry->second = std::move(certificate);
    }
    channelPathsChanged = true;
    ++serial;
    return true;
}

bool Reservoirs::hasChannelMouth(TilePos mouth) const {
    return std::any_of(
        channels.begin(), channels.end(),
        [&](const auto& entry) { return entry.second.mouths.contains(mouth); });
}

bool Reservoirs::feedsPool(TilePos mouth) const {
    const auto state = snapshot();
    return std::any_of(
        channels.begin(), channels.end(), [&](const auto& entry) {
            const auto pool = state->pools.find(entry.second.to.body);
            return pool != state->pools.end() && pool->second.parent &&
                   entry.second.mouths.contains(mouth);
        });
}

void Reservoirs::blockChannelCell(TilePos cell) {
    for (auto it = channels.begin(); it != channels.end();) {
        if (std::erase_if(it->second.mouths, [&](const auto& entry) {
                const auto& path = entry.second.path;
                return entry.first == cell ||
                       std::find(path.begin(), path.end(), cell) != path.end();
            }))
            ++serial;
        it = it->second.mouths.empty() ? channels.erase(it) : std::next(it);
    }
}

bool Reservoirs::clearChannelMouth(TilePos mouth) {
    bool removed = false;
    for (auto it = channels.begin(); it != channels.end();) {
        removed |= it->second.mouths.erase(mouth) > 0;
        it = it->second.mouths.empty() ? channels.erase(it) : std::next(it);
    }
    if (removed) ++serial;
    return removed;
}

void Reservoirs::drainChannels(uint64_t elapsed,
                               std::shared_ptr<ReservoirSnapshot>& next,
                               ReservoirChange& change) {
    std::set<TilePos, TilePosLess> moving;
    const auto previous = snapshot();
    for (const auto& [key, channel] : channels) {
        const auto* shape = curve(channel.from);
        if (!shape) continue;
        const auto before = volume(channel.from);
        auto current = before;
        // Piecewise: the number of fed mouths changes as the surface drops
        // past each mouth threshold, so a long skip uses the same rates as
        // one-second steps.
        for (uint64_t remaining = elapsed; remaining && current > 0;) {
            const int level = shape->levelAt(current);
            WaterVolume count = 0;
            int floor = -1;
            for (const auto& [mouth, cert] : channel.mouths)
                if (cert.threshold < level) {
                    ++count;
                    floor = std::max(floor, cert.threshold);
                }
            if (!count) break;
            const auto rate = ChannelUnitsPerMouth * count *
                              (channel.to.body || channel.to.reach ? 1 : 8);
            const auto limit = current - shape->volumeAt(floor);
            if (limit <= 0) break;
            const uint64_t steps =
                std::min<uint64_t>(remaining, uint64_t(1 + (limit - 1) / rate));
            current -= std::min(limit, scale(rate, steps));
            remaining -= steps;
        }
        // A depleted receiving lake may rise above the surveyed tailwater
        // during one long step. Stop at equal head, even while unloaded.
        if (const auto it = curves.find(channel.to.body); it != curves.end()) {
            const auto& dest = it->second;
            const auto stored = volume(channel.to.body);
            WaterVolume low = 0, high = before - current;
            while (low < high) {
                const auto mid = low + (high - low) / 2 + 1;
                const auto arrival =
                    std::min(add(stored, mid), dest.capacity());
                const auto head = [](const StorageCurve& shape, WaterVolume v) {
                    if (!v) return (shape.levelAt(1) - 1) * 255;
                    return (shape.levelAt(v) - 1) * 255 +
                           shape.fillAtSurface(v);
                };
                if (head(*shape, before - mid) >= head(dest, arrival))
                    low = mid;
                else
                    high = mid - 1;
            }
            current = before - low;
        }
        const Channel* spill = nullptr;
        WaterVolume retained = before - current;
        if (const auto pool = previous->pools.find(channel.to.body);
            pool != previous->pools.end() && pool->second.parent) {
            const auto& dest = curves.at(channel.to.body);
            retained =
                std::min(retained, dest.capacity() - volume(channel.to.body));
            const auto crest = dest.levelAt(dest.capacity());
            auto out = channels.lower_bound({channel.to.body, 0, 0});
            for (; out != channels.end() && out->second.from == channel.to.body;
                 ++out) {
                if (out->second.to.reach &&
                    std::any_of(out->second.mouths.begin(),
                                out->second.mouths.end(),
                                [crest](const auto& entry) {
                                    return entry.second.threshold == crest;
                                })) {
                    spill = &out->second;
                    break;
                }
            }
            if (!spill) current = before - retained;
        }
        const auto moved = before - current;
        if (moved <= 0) continue;
        if (const auto pool = previous->pools.find(channel.to.body);
            pool != previous->pools.end() && pool->second.parent) {
            for (const auto& [mouth, cert] : channel.mouths)
                if (current > shape->volumeAt(cert.threshold) &&
                    (spill || volume(channel.to.body) + moved <
                                  curves.at(channel.to.body).capacity()))
                    moving.insert(mouth);
        }
        stores[channel.from] = current;
        if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
        updateLevel(*next, *shape, channel.from, change);
        if (spill && moved > retained) {
            receive(spill->to, moved - retained, next, change);
            const auto crest =
                curves.at(channel.to.body)
                    .levelAt(curves.at(channel.to.body).capacity());
            for (const auto& [mouth, cert] : spill->mouths)
                if (cert.threshold == crest) moving.insert(mouth);
        }
        if (channel.to.body || channel.to.reach)
            receive(channel.to, retained, next, change);
        else
            totals[size_t(ReservoirExchange::Disposal)] =
                add(totals[size_t(ReservoirExchange::Disposal)], -moved);
    }
    if (moving == flowingMouths && (!channelPathsChanged || moving.empty()))
        return;
    flowingMouths = std::move(moving);
    channelPathsChanged = false;
    if (!next) next = std::make_shared<ReservoirSnapshot>(*previous);
    auto invalidate = [&] {
        for (const auto& [key, column] : next->streams)
            for (int y = column.bottom; y <= column.top; ++y)
                change.locals.push_back(
                    {int(key >> 32), y, int(uint32_t(key))});
    };
    invalidate();
    next->streams.clear();
    for (const auto& [key, channel] : channels) {
        for (const auto& [mouth, cert] : channel.mouths) {
            if (!flowingMouths.contains(mouth)) continue;
            Vec2 direction{};
            for (size_t i = cert.path.size(); i > 1; --i) {
                const auto upstream = cert.path[i - 1], p = cert.path[i - 2];
                const int dx = generated
                                   ? int(WorldWrap::delta(p.x, upstream.x,
                                                          generated->periodX()))
                                   : p.x - upstream.x;
                if (dx || p.z != upstream.z)
                    direction = {float(dx), float(p.z - upstream.z)};
                auto [it, inserted] = next->streams.try_emplace(
                    ReservoirSnapshot::columnKey(p.x, p.z),
                    ReservoirSnapshot::StreamColumn{p.y, p.y, direction});
                if (!inserted) {
                    it->second.bottom = std::min(it->second.bottom, p.y);
                    it->second.top = std::max(it->second.top, p.y);
                }
            }
        }
    }
    invalidate();
}

bool Reservoirs::needsFixedSteps() const {
    const auto state = snapshot();
    return !activeCells.empty() || !state->portals.empty() || !spills.empty() ||
           std::any_of(outlets.begin(), outlets.end(),
                       [](const auto& entry) { return entry.second.open; });
}

std::optional<std::vector<Reservoirs::Route>> Reservoirs::routes(
    const std::map<uint32_t, ReachDelivery>& state) const {
    std::vector<Route> sorted;
    if (!generated) return sorted;
    using End = GeneratedHydrology::RiverEnd;
    std::map<Node, Node> edges;
    std::map<Node, unsigned> incoming;
    for (const auto& reach : generated->riverReaches()) {
        const Node id = reach.id | ReachFlag;
        if (reach.lakeSource) edges[reach.lakeSource->body] = id;
        Node next = 0;
        if (const auto it = state.find(reach.id); it != state.end())
            next = it->second.receiver;
        if (!next) {
            if (reach.end == End::Sea || reach.end == End::Lake)
                next = reach.receiver;
            else if (reach.end == End::Confluence)
                next = reach.receiver | ReachFlag;
        }
        edges[id] = next;
    }
    for (const auto& [id, loss] : lossRates) edges.try_emplace(id, 0);
    for (const auto& [node, next] : edges) {
        incoming.try_emplace(node, 0);
        if (next) ++incoming[next];
    }
    for (const auto& [node, degree] : incoming)
        if (!degree) sorted.push_back({node, edges[node]});
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto next = sorted[i].next;
        if (next && !--incoming.at(next)) sorted.push_back({next, edges[next]});
    }
    if (sorted.size() != edges.size()) return std::nullopt;
    return sorted;
}

void Reservoirs::buildNaturalBudget() {
    std::unordered_map<Node, WaterVolume> incoming;
    for (const auto [node, next] : order) {
        auto amount = incoming[node];
        if (node & ReachFlag) {
            amount += springRates.at(uint32_t(node));
            naturalReachRates[uint32_t(node)] = amount;
        } else {
            // At a full through-lake, leave half of normal inflow for its
            // outlet. A terminal body loses the entire normal delivery.
            const auto loss = next ? amount / 2 : amount;
            // An outlet from a groundwater-fed lake also has a water
            // budget when its source lake is full.
            if (next && !amount && lakeOutletReaches.contains(uint32_t(node))) {
                groundwaterOutlets.insert(uint32_t(node));
                amount = LakeSeepRate;
            }
            if (loss) lossRates[uint32_t(node)] = loss;
            amount -= loss;
        }
        if (next) incoming[next] += amount;
    }
}

void Reservoirs::applyBudget(uint32_t body, WaterVolume amount,
                             WaterVolume loss, bool outlet,
                             WaterVolume& overflow,
                             std::shared_ptr<ReservoirSnapshot>& next,
                             ReservoirChange& change) {
    // A full, undisturbed basin can be balanced without surveying its curve.
    if (!stores.contains(body) && amount >= loss) {
        if (outlet)
            overflow = amount - loss;
        else if (amount > loss)
            totals[size_t(ReservoirExchange::Disposal)] =
                add(totals[size_t(ReservoirExchange::Disposal)], loss - amount);
        totals[size_t(ReservoirExchange::River)] =
            add(totals[size_t(ReservoirExchange::River)], amount - overflow);
        totals[size_t(ReservoirExchange::Evaporation)] =
            add(totals[size_t(ReservoirExchange::Evaporation)], -loss);
        return;
    }
    const auto* shape = curve(body);
    if (!shape) {
        // Unavailable receivers do not spend the arriving water.
        overflow = amount;
        return;
    }
    const auto before = volume(body);
    const auto used = std::min(loss, add(before, amount));
    const auto net =
        std::clamp(amount - used, -before, shape->capacity() - before);
    const auto excess = amount - used - net;
    if (outlet) overflow = excess;
    if (excess && !outlet)
        totals[size_t(ReservoirExchange::Disposal)] =
            add(totals[size_t(ReservoirExchange::Disposal)], -excess);
    totals[size_t(ReservoirExchange::River)] =
        add(totals[size_t(ReservoirExchange::River)], amount - overflow);
    totals[size_t(ReservoirExchange::Evaporation)] =
        add(totals[size_t(ReservoirExchange::Evaporation)], -used);
    if (net) {
        stores[body] = before + net;
        if (shape->levelAt(before) != shape->levelAt(stores.at(body)) ||
            shape->fillAtSurface(before) !=
                shape->fillAtSurface(stores.at(body))) {
            if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
            updateLevel(*next, *shape, body, change);
        }
        if (stores.at(body) == shape->capacity()) stores.erase(body);
    }
}

void Reservoirs::advanceTo(uint64_t step, std::span<const RiverIntake> intakes,
                           const CellQuery& access, int periodX) {
    if (step <= budgetStep) return;
    const auto elapsed = step - budgetStep;
    budgetStep = step;
    ++serial;
    std::unordered_map<Node, WaterVolume> incoming;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    drainChannels(elapsed, next, change);
    // Arrivals at full bodies continue through the budget graph; a body
    // outside it is terminal, so they leave as disposal.
    if (!pendingInflow.empty()) {
        std::unordered_set<Node> routed;
        for (const auto& route : order) routed.insert(route.node);
        for (const auto& [body, amount] : pendingInflow)
            if (routed.contains(body))
                incoming[body] = add(incoming[body], amount);
            else
                totals[size_t(ReservoirExchange::Disposal)] =
                    add(totals[size_t(ReservoirExchange::Disposal)], -amount);
        pendingInflow.clear();
    }
    const auto seepBudget = scale(LakeSeepRate, elapsed);
    for (const auto body : groundwaterOutlets) {
        const Node reach = ReachFlag | lakeOutletReaches.at(body);
        incoming[reach] = add(incoming[reach], seepBudget);
    }
    // A drawn-down natural basin exposes groundwater. Bound the seep to
    // one eighth block per game second and stop exactly at its resting level.
    for (auto& [body, stored] : stores) {
        if (!generated || !generated->basin(body) || lossRates.contains(body))
            continue;
        const auto* shape = curve(body);
        if (!shape) continue;
        WaterVolume capacity = shape->capacity();
        for (const auto& [p, breach] : breaches)
            if (breach.parent == body &&
                generated->waterAt(p.x, p.z).body == body)
                capacity -= curves.at(breach.pool).capacity();
        const auto supplied =
            std::clamp<WaterVolume>(capacity - stored, 0, seepBudget);
        if (!supplied) continue;
        if (groundwaterOutlets.contains(body))
            incoming[ReachFlag | lakeOutletReaches.at(body)] -= supplied;
        const auto before = stored;
        stored += supplied;
        totals[size_t(ReservoirExchange::Input)] =
            add(totals[size_t(ReservoirExchange::Input)], supplied);
        if (shape->levelAt(before) == shape->levelAt(stored) &&
            shape->fillAtSurface(before) == shape->fillAtSurface(stored))
            continue;
        if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
        updateLevel(*next, *shape, body, change);
    }
    if (order.empty()) {
        if (next) publish(std::move(next), change);
        return;
    }
    const auto previousStages = snapshot();
    std::unordered_map<Node, WaterVolume> belowDam;
    std::unordered_map<uint32_t, std::vector<const RiverIntake*>> byReach;
    for (const auto& intake : intakes)
        if (intake.cell.x >= 0 && intake.cell.z >= 0 && intake.cell.y >= 0 &&
            intake.cell.y < WORLD_HEIGHT_Y)
            byReach[intake.reach].push_back(&intake);
    for (const auto [node, target] : order) {
        auto amount = incoming[node];
        if (node & ReachFlag) {
            const auto id = uint32_t(node);
            // An open runtime outlet redirects the generated spring through
            // the exposed face; closing it resumes the original bed's feed.
            WaterVolume rate = springRates.at(id);
            for (const auto& [outletId, outlet] : outlets)
                if (outlet.reach == id) rate = outlet.open ? outlet.rate : rate;
            if (sealedSprings.contains(id)) rate = 0;
            auto sourceTime = elapsed;
            if (const auto reopened = springReopenedAt.find(id);
                reopened != springReopenedAt.end()) {
                const auto& reach =
                    generated->riverReaches()[reachIndices.at(id)];
                const auto delay = riverDelay(reach.spring.tile, reach.outlet,
                                              generated->periodX());
                const auto age = budgetStep - reopened->second;
                sourceTime =
                    recoveredSeconds(age, delay) -
                    recoveredSeconds(age - std::min(age, elapsed), delay);
            }
            amount = add(amount, scale(rate, sourceTime));
            const auto offerIntakes = [&](bool downstream) {
                const auto it = byReach.find(id);
                if (it == byReach.end()) return false;
                bool blocked = false;
                for (const auto* intake : it->second) {
                    if (intake->belowDam != downstream) continue;
                    blocked |= intake->blocked;
                    const auto current = snapshot();
                    const auto& cells = next ? next->cells : current->cells;
                    const int fill = cells.fill(intake->cell);
                    const auto head = [&](int y, WaterVolume) {
                        return intake->head.fillAt(y, -1);
                    };
                    if (access && amount &&
                        (elapsed > 1 || fill >= head(intake->cell.y, 0))) {
                        const auto distanceLimit =
                            elapsed == 1 ? uint64_t(MaxVolume) : elapsed;
                        amount -=
                            fillCut(*current, next, change, intake->cell,
                                    std::min(amount, scale(255, elapsed)),
                                    distanceLimit, elapsed == 1 ? 512 : 4096,
                                    access, periodX, head);
                        continue;
                    }
                    const auto accepted = std::clamp<WaterVolume>(
                        int(head(intake->cell.y, 0)) - fill, 0, amount);
                    if (!accepted) continue;
                    if (!next)
                        next = std::make_shared<ReservoirSnapshot>(*current);
                    next->cells.set(intake->cell, uint8_t(fill + accepted));
                    change.locals.push_back(intake->cell);
                    amount -= accepted;
                }
                return blocked;
            };
            if (offerIntakes(false)) {
                auto& delivery = deliveries[id];
                delivery.held = add(delivery.held, amount);
                amount = 0;
            }
            // Retained head releases once if the wall has been removed.
            for (const auto& [opening, breach] : breaches) {
                if (breach.reach != id || !breach.dam || breach.open) continue;
                const auto before = volume(breach.pool);
                const auto released = std::min(before, scale(255, elapsed));
                if (!released) continue;
                stores[breach.pool] = before - released;
                amount = add(amount, released);
                if (!next)
                    next = std::make_shared<ReservoirSnapshot>(*snapshot());
                updateLevel(*next, curves.at(breach.pool), breach.pool, change);
            }
            for (auto& [opening, breach] : breaches) {
                if (breach.reach != id || !breach.open) continue;
                const auto& shape = curves.at(breach.pool);
                const auto before = volume(breach.pool);
                const auto limit = breachLimit(breach);
                // Older saves may hold a full voxel above the spillway.
                // Route that excess through the same downstream budget.
                if (before > limit) {
                    stores[breach.pool] = limit;
                    amount = add(amount, before - limit);
                    if (!next)
                        next = std::make_shared<ReservoirSnapshot>(*snapshot());
                    updateLevel(*next, shape, breach.pool, change);
                }
                if (before >= limit || !amount) continue;
                const auto offered =
                    breach.dam ? amount
                               : amount / 2 + (amount % 2 && breach.oddUnit);
                if (!breach.dam) breach.oddUnit ^= bool(amount % 2);
                const auto accepted = std::min(offered, limit - before);
                if (!accepted) continue;
                stores[breach.pool] = before + accepted;
                amount -= accepted;
                if (!next)
                    next = std::make_shared<ReservoirSnapshot>(*snapshot());
                updateLevel(*next, shape, breach.pool, change);
                if (stores.at(breach.pool) == limit)
                    updateRiverStage(id, next, change);
            }
            if (const auto it = belowDam.find(node); it != belowDam.end())
                amount = add(amount, it->second);
            offerIntakes(true);
            if (!target && amount) {
                // Closed generated routes retain arrivals until a confirmed
                // receiver becomes available.
                auto& delivery = deliveries[id];
                delivery.held = add(delivery.held, amount);
                continue;
            }
            if (auto it = deliveries.find(id); it != deliveries.end()) {
                auto& delivery = it->second;
                const auto partial =
                    amount % 100 * delivery.percent + delivery.remainder;
                const auto passed =
                    amount / 100 * delivery.percent + partial / 100;
                delivery.remainder = unsigned(partial % 100);
                delivery.held = add(delivery.held, amount - passed);
                amount = passed;
                if (delivery.percent == 100) {
                    amount = add(amount, delivery.held);
                    delivery.held = 0;
                }
            }
        } else {
            const auto id = uint32_t(node);
            const auto loss = lossRates.find(id);
            WaterVolume overflow = 0;
            applyBudget(
                id, amount,
                loss == lossRates.end() ? 0 : scale(loss->second, elapsed),
                target != 0, overflow, next, change);
            amount = overflow;
        }
        if (target && amount) {
            // Water entering below the wall bypasses its upstream pool.
            auto* arrivals = &incoming;
            if ((node & ReachFlag) && (target & ReachFlag)) {
                const auto stage =
                    previousStages->riverStages.find(uint32_t(target));
                if (stage != previousStages->riverStages.end() &&
                    stage->second.damAge) {
                    const auto join =
                        generated
                            ->riverReaches()[reachIndices.at(uint32_t(node))]
                            .outlet;
                    const auto column = generated->riverAt(join.x, join.y);
                    if (column && stage->second.belowCut(
                                      join.x, join.y, column->water.surfaceY))
                        arrivals = &belowDam;
                }
            }
            auto& received = (*arrivals)[target];
            received = add(received, amount);
        }
    }
    // Dam spills drain the reach's held delivery into their upstream pool
    // band by band until the pool reaches the crest; the surplus crosses
    // into the spill's receiver at a bounded rate. A full receiver holds
    // the water upstream until its level drops.
    for (auto& [reachId, spill] : spills) {
        auto& delivery = deliveries[reachId];
        // Fill the upstream pool band by band up to the crest height.
        if (const auto* poolShape = curve(spill.pool)) {
            const auto before = volume(spill.pool);
            const auto room = poolShape->capacity() - before;
            const auto filled = std::min(delivery.held, room);
            if (filled > 0) {
                stores[spill.pool] = before + filled;
                delivery.held -= filled;
                next = next ? next
                            : std::make_shared<ReservoirSnapshot>(*snapshot());
                updateLevel(*next, *poolShape, spill.pool, change);
            }
        }
        // The pool stands at the crest: the surplus crosses at the spill's
        // bounded rate into the receiver below.
        if (delivery.held > 0) {
            const auto partial =
                delivery.held % WaterUnitsPerBlock * spill.rate +
                spill.remainder;
            const auto whole = delivery.held / WaterUnitsPerBlock * spill.rate +
                               partial / WaterUnitsPerBlock;
            spill.remainder = unsigned(partial % WaterUnitsPerBlock);
            if (const auto* shape = curve(spill.body)) {
                const auto before = volume(spill.body);
                const auto accepted = std::min(
                    {whole, delivery.held, shape->capacity() - before});
                if (accepted <= 0) continue;
                stores[spill.body] = before + accepted;
                totals[size_t(ReservoirExchange::River)] =
                    add(totals[size_t(ReservoirExchange::River)], accepted);
                if (!next)
                    next = std::make_shared<ReservoirSnapshot>(*snapshot());
                updateLevel(*next, *shape, spill.body, change);
                delivery.held -= accepted;
            }
        }
    }
    // Downstream stages are needed first when projecting junction backwater.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (!(it->node & ReachFlag)) continue;
        const auto id = uint32_t(it->node);
        const auto& reach = generated->riverReaches()[reachIndices.at(id)];
        const auto reopened = springReopenedAt.find(id);
        const bool recovering = reopened != springReopenedAt.end();
        if (recovering && budgetStep - reopened->second >= MaxRiverAge) {
            springReopenedAt.erase(reopened);
            springClosedAt.erase(id);
        }
        if (sealedSprings.contains(id) || recovering || reach.lakeSource ||
            (previousStages->riverStages.contains(id) &&
             previousStages->riverStages.at(id).damAge))
            updateRiverStage(id, next, change);
    }
    if (next) publish(std::move(next), change);
}

Bytes Reservoirs::encode() const {
    BinaryWriter out;
    out.write(budgetStep);
    for (auto value : totals) out.write(value);
    out.write(uint32_t(breaches.size()));
    for (const auto& [p, breach] : breaches) {
        out.write(int32_t(p.x));
        out.write(int16_t(p.y));
        out.write(int32_t(p.z));
        out.write(breach.reach);
        out.write(breach.pool);
        out.write(breach.parent);
        out.write(int16_t(breach.survey.head));
        out.write(uint8_t(breach.open | (breach.dam ? 2 : 0)));
        out.write(uint8_t(breach.oddUnit));
        out.write(uint32_t(breach.survey.columns.size()));
        for (const auto& c : breach.survey.columns) {
            out.write(int32_t(c.tile.x));
            out.write(int32_t(c.tile.y));
            out.write(int16_t(c.floor));
            out.write(int16_t(c.sill));
        }
        const auto& upstream = breach.survey.damUpstream;
        out.write(uint32_t(upstream ? upstream->size() : 0));
        if (upstream)
            for (const auto key : *upstream) out.write(key);
        out.write(breach.closedAt);
    }
    const std::set<uint32_t> orderedSprings(sealedSprings.begin(),
                                            sealedSprings.end());
    out.write(uint32_t(orderedSprings.size()));
    for (const auto reach : orderedSprings) out.write(reach);
    out.write(uint32_t(stores.size()));
    for (const auto& [id, volume] : stores) {
        out.write(id);
        out.write(volume);
    }
    const auto current = snapshot();
    out.write(uint32_t(current->cells.size()));
    std::map<std::tuple<int, int, int>, uint8_t> ordered;
    current->cells.forEach(
        [&](TilePos p, uint8_t fill) { ordered[{p.x, p.y, p.z}] = fill; });
    for (const auto& [key, fill] : ordered) {
        const auto [x, y, z] = key;
        out.write(int32_t(x));
        out.write(int16_t(y));
        out.write(int32_t(z));
        out.write(fill);
    }
    out.write(uint32_t(deliveries.size()));
    for (const auto& [id, state] : deliveries) {
        out.write(id);
        out.write(uint8_t(state.percent));
        out.write(state.receiver);
        out.write(state.held);
        out.write(uint8_t(state.remainder));
    }
    out.write(uint32_t(current->portals.size()));
    for (const auto& [cell, body] : current->portals) {
        out.write(int32_t(cell.x));
        out.write(int16_t(cell.y));
        out.write(int32_t(cell.z));
        out.write(body);
    }
    out.write(uint32_t(outlets.size()));
    for (const auto& [id, outlet] : outlets) {
        out.write(id);
        out.write(outlet.reach);
        out.write(int32_t(outlet.rock.x));
        out.write(int32_t(outlet.rock.y));
        out.write(int32_t(outlet.face.x));
        out.write(int16_t(outlet.face.y));
        out.write(int32_t(outlet.face.z));
        out.write(int16_t(outlet.rockY));
        out.write(outlet.rate);
        out.write(outlet.held);
        out.write(uint8_t(outlet.remainder));
        out.write(uint8_t(outlet.open));
    }
    out.write(uint32_t(spills.size()));
    for (const auto& [reach, spill] : spills) {
        out.write(reach);
        out.write(spill.pool);
        out.write(spill.body);
        out.write(int32_t(spill.height));
        out.write(spill.rate);
        out.write(uint8_t(spill.remainder));
    }
    out.write(uint32_t(channels.size()));
    for (const auto& [key, channel] : channels) {
        out.write(channel.from);
        out.write(channel.to.body);
        out.write(channel.to.reach);
        out.write(int16_t(channel.to.surfaceY));
        out.write(uint32_t(channel.mouths.size()));
        for (const auto& [mouth, cert] : channel.mouths) {
            out.write(int32_t(mouth.x));
            out.write(int16_t(mouth.y));
            out.write(int32_t(mouth.z));
            out.write(int16_t(cert.threshold));
            out.write(uint16_t(cert.path.size()));
            for (const auto p : cert.path) {
                out.write(int32_t(p.x));
                out.write(int16_t(p.y));
                out.write(int32_t(p.z));
            }
        }
    }
    out.write(uint32_t(pendingInflow.size()));
    for (const auto& [body, amount] : pendingInflow) {
        out.write(body);
        out.write(amount);
    }
    out.write(uint32_t(springClosedAt.size()));
    for (const auto& [reach, step] : springClosedAt) {
        out.write(reach);
        out.write(step);
    }
    out.write(uint32_t(springReopenedAt.size()));
    for (const auto& [reach, step] : springReopenedAt) {
        out.write(reach);
        out.write(step);
    }
    SectionContainer file;
    file.put("RSVR", 9, out.take());
    return file.encode();
}

void Reservoirs::restore(ByteView bytes) {
    const auto file = SectionContainer::decode(bytes);
    const auto* section = file.find("RSVR");
    if (!section || section->version > 9 || !section->version)
        throw SerializationError("Unsupported reservoir ledger");
    BinaryReader in(section->data);
    const auto savedStep = in.read<uint64_t>();
    Totals savedTotals{};
    for (auto& value : savedTotals) in.read(value);
    std::map<uint32_t, WaterVolume> savedStores;
    auto next = std::make_shared<ReservoirSnapshot>();
    decltype(breaches) savedBreaches;
    decltype(sealedSprings) savedSealed;
    std::map<uint32_t, StorageCurve> savedCurves;
    std::map<uint32_t, std::vector<Vec2i>> savedFootprints;
    if (section->version >= 3) {
        const auto count = in.read<uint32_t>();
        for (uint32_t i = 0; i < count; ++i) {
            const TilePos p{in.read<int32_t>(), in.read<int16_t>(),
                            in.read<int32_t>()};
            const auto reach = in.read<uint32_t>();
            const auto body = in.read<uint32_t>();
            const auto parent = section->version >= 4 ? in.read<uint32_t>() : 0;
            const int head = in.read<int16_t>();
            const auto open = in.read<uint8_t>();
            const auto odd = in.read<uint8_t>();
            const auto columns = in.read<uint32_t>();
            if (p.y < 0 || p.y >= WORLD_HEIGHT_Y ||
                head <= ((open & 2) ? 0 : p.y) || head > WORLD_HEIGHT_Y ||
                (parent ? (reach || open || odd || parent == body ||
                           parent >= 0x80000000u || !curve(parent))
                        : !springRates.contains(reach)) ||
                !generated->containsColumn(p.x, p.z) || body < 0x80000000u ||
                savedCurves.contains(body) || open > 3 || odd > 1 || !columns ||
                columns > 8192)
                throw SerializationError("Invalid river breach");
            auto geometry = std::make_shared<PoolFootprint>();
            auto& footprint = savedFootprints[body];
            BreachSurvey survey{head, {}};
            for (uint32_t j = 0; j < columns; ++j) {
                const Vec2i tile{in.read<int32_t>(), in.read<int32_t>()};
                const int floor = in.read<int16_t>(), sill = in.read<int16_t>();
                const auto key = ReservoirSnapshot::columnKey(tile.x, tile.y);
                if (!generated->containsColumn(tile.x, tile.y) || floor < 0 ||
                    sill < floor || sill >= head ||
                    geometry->columns.contains(key))
                    throw SerializationError("Invalid river breach footprint");
                geometry->add(tile, generated->periodX(), p.x, floor, sill);
                footprint.push_back(tile);
                survey.columns.push_back({tile, floor, sill});
            }
            if (section->version >= 8) {
                const auto count = in.read<uint32_t>();
                constexpr int Radius = ReservoirSnapshot::RiverStage::DamRadius;
                if (count > (2 * Radius + 1) * (2 * Radius + 1) ||
                    (count && !(open & 2)))
                    throw SerializationError("Invalid dam cut size");
                if (count) {
                    auto upstream = std::make_shared<std::vector<uint64_t>>();
                    upstream->reserve(count);
                    for (uint32_t k = 0; k < count; ++k) {
                        const auto key = in.read<uint64_t>();
                        const int x = int(key >> 32), z = int(uint32_t(key));
                        if (!generated->containsColumn(x, z) ||
                            std::abs(WorldWrap::delta(
                                x, p.x, generated->periodX())) > Radius ||
                            std::abs(int64_t(z) - p.z) > Radius ||
                            (!upstream->empty() && upstream->back() >= key))
                            throw SerializationError("Invalid dam cut column");
                        upstream->push_back(key);
                    }
                    survey.damUpstream = std::move(upstream);
                }
            }
            const uint64_t closedAt =
                section->version >= 9 ? in.read<uint64_t>() : 0;
            if (closedAt > savedStep)
                throw SerializationError("Invalid dam closure time");
            next->pools.emplace(
                body, PoolState{-1, 255, std::move(geometry), parent,
                                (open & 2) && !(open & 1) ? head - 1 : -1});
            savedCurves.emplace(body, breachCurve(survey));
            if (!savedBreaches
                     .emplace(p, Breach{reach, body, std::move(survey),
                                        bool(open & 1), bool(odd),
                                        bool(open & 2), parent, closedAt})
                     .second)
                throw SerializationError("Duplicate river breach");
        }
        const auto sealed = in.read<uint32_t>();
        for (uint32_t i = 0; i < sealed; ++i) {
            const auto reach = in.read<uint32_t>();
            if (!springRates.contains(reach) ||
                !savedSealed.insert(reach).second)
                throw SerializationError("Invalid sealed spring");
        }
    }
    const auto savedCurve = [&](uint32_t body) -> const StorageCurve* {
        if (const auto it = savedCurves.find(body); it != savedCurves.end())
            return &it->second;
        return curve(body);
    };
    const auto bodies = in.read<uint32_t>();
    for (uint32_t i = 0; i < bodies; ++i) {
        const auto id = in.read<uint32_t>();
        const auto volume = in.read<WaterVolume>();
        const auto* shape = savedCurve(id);
        if (!shape || volume < 0 || volume > shape->capacity() ||
            !savedStores.emplace(id, volume).second)
            throw SerializationError("Invalid reservoir store");
        if (!next->pools.contains(id) && volume != shape->capacity()) {
            next->levels[id] = shape->levelAt(volume);
            const auto fill = shape->fillAtSurface(volume);
            if (fill != 255) next->surfaceFills[id] = fill;
        }
    }
    for (auto& [id, pool] : next->pools) {
        const auto& shape = savedCurves.at(id);
        const auto volume =
            savedStores.contains(id) ? savedStores.at(id) : shape.capacity();
        pool.level = shape.levelAt(volume);
        pool.fill = shape.fillAtSurface(volume);
    }
    const auto cells = in.read<uint32_t>();
    for (uint32_t i = 0; i < cells; ++i) {
        const TilePos p{in.read<int32_t>(), in.read<int16_t>(),
                        in.read<int32_t>()};
        const auto fill = in.read<uint8_t>();
        if (p.x < 0 || p.z < 0 || p.y < 0 || p.y >= WORLD_HEIGHT_Y || !fill ||
            next->cells.contains(p))
            throw SerializationError("Invalid local water store");
        next->cells.set(p, fill);
    }
    std::map<uint32_t, ReachDelivery> savedDeliveries;
    const auto count = in.read<uint32_t>();
    for (uint32_t i = 0; i < count; ++i) {
        const auto id = in.read<uint32_t>();
        const ReachDelivery state{in.read<uint8_t>(), in.read<uint32_t>(),
                                  in.read<WaterVolume>(), in.read<uint8_t>()};
        if (!springRates.contains(id) || state.percent > 100 ||
            state.remainder >= 100 || state.held < 0 ||
            (state.receiver && !savedCurve(state.receiver)) ||
            !savedDeliveries.emplace(id, state).second)
            throw SerializationError("Invalid reach delivery");
    }
    std::map<TilePos, uint32_t, TilePosLess> savedPortals;
    std::map<uint32_t, Outlet> savedOutlets;
    uint32_t savedNextOutlet = 0;
    std::map<uint32_t, Spill> savedSpills;
    if (section->version >= 2) {
        const auto portalCount = in.read<uint32_t>();
        for (uint32_t i = 0; i < portalCount; ++i) {
            const TilePos cell{in.read<int32_t>(), in.read<int16_t>(),
                               in.read<int32_t>()};
            const auto body = in.read<uint32_t>();
            if (cell.x < 0 || cell.y < 0 || cell.y >= WORLD_HEIGHT_Y ||
                cell.z < 0 || !savedCurve(body) ||
                !savedPortals.emplace(cell, body).second)
                throw SerializationError("Invalid reservoir portal");
        }
        const auto outletCount = in.read<uint32_t>();
        for (uint32_t i = 0; i < outletCount; ++i) {
            Outlet outlet;
            outlet.id = in.read<uint32_t>();
            outlet.reach = in.read<uint32_t>();
            outlet.rock = {in.read<int32_t>(), in.read<int32_t>()};
            outlet.face = {in.read<int32_t>(), in.read<int16_t>(),
                           in.read<int32_t>()};
            outlet.rockY = in.read<int16_t>();
            outlet.rate = in.read<WaterVolume>();
            outlet.held = in.read<WaterVolume>();
            outlet.remainder = in.read<uint8_t>();
            outlet.open = in.read<uint8_t>() != 0;
            if (outlet.rate <= 0 || outlet.remainder >= WaterUnitsPerBlock ||
                outlet.held < 0 || outlet.rockY < 0 || outlet.rock.x < 0 ||
                outlet.rock.y < 0 || outlet.face.x < 0 || outlet.face.y < 0 ||
                outlet.face.z < 0 ||
                !savedOutlets.emplace(outlet.id, outlet).second)
                throw SerializationError("Invalid aquifer outlet");
            savedNextOutlet = std::max(savedNextOutlet, outlet.id);
        }
        const auto spillCount = in.read<uint32_t>();
        for (uint32_t i = 0; i < spillCount; ++i) {
            Spill spill;
            const auto reach = in.read<uint32_t>();
            spill.pool = in.read<uint32_t>();
            spill.body = in.read<uint32_t>();
            spill.height = in.read<int32_t>();
            spill.rate = in.read<WaterVolume>();
            spill.remainder = in.read<uint8_t>();
            if (!reach || !spill.pool || !spill.body || spill.height <= 0 ||
                spill.height >= WORLD_HEIGHT_Y || spill.rate <= 0 ||
                spill.remainder >= WaterUnitsPerBlock ||
                !savedCurve(spill.pool) || !savedCurve(spill.body) ||
                !savedSpills.emplace(reach, spill).second)
                throw SerializationError("Invalid spill route");
        }
    }
    decltype(channels) savedChannels;
    decltype(pendingInflow) savedPending;
    if (section->version >= 5) {
        const auto channelCount = in.read<uint32_t>();
        for (uint32_t i = 0; i < channelCount; ++i) {
            Channel channel;
            channel.from = in.read<uint32_t>();
            channel.to.body = in.read<uint32_t>();
            channel.to.reach = in.read<uint32_t>();
            channel.to.surfaceY = in.read<int16_t>();
            const auto mouths = in.read<uint32_t>();
            if (!channel.from || channel.from == channel.to.body ||
                !savedCurve(channel.from) ||
                (section->version < 6 && !channel.to.body &&
                 !channel.to.reach) ||
                (channel.to.reach && !springRates.contains(channel.to.reach)) ||
                !mouths || mouths > 65536)
                throw SerializationError("Invalid reservoir channel");
            for (uint32_t j = 0; j < mouths; ++j) {
                const TilePos mouth{in.read<int32_t>(), in.read<int16_t>(),
                                    in.read<int32_t>()};
                const int threshold = in.read<int16_t>();
                const auto count = in.read<uint16_t>();
                if (!count || count > 8194)
                    throw SerializationError("Invalid reservoir channel path");
                Channel::Mouth cert{threshold, {}};
                for (unsigned k = 0; k < count; ++k) {
                    const TilePos p{in.read<int32_t>(), in.read<int16_t>(),
                                    in.read<int32_t>()};
                    if (p.x < 0 || p.z < 0 || p.y < 0 || p.y >= WORLD_HEIGHT_Y)
                        throw SerializationError("Invalid channel cell");
                    cert.path.push_back(p);
                }
                if (mouth.x < 0 || mouth.z < 0 || mouth.y < 0 ||
                    mouth.y >= WORLD_HEIGHT_Y || threshold < mouth.y ||
                    threshold >= WORLD_HEIGHT_Y ||
                    (!channel.to.body && !channel.to.reach &&
                     (channel.to.surfaceY < 0 ||
                      channel.to.surfaceY >= mouth.y)) ||
                    !channel.mouths.emplace(mouth, std::move(cert)).second)
                    throw SerializationError("Invalid reservoir channel mouth");
            }
            const ChannelKey key{channel.from, channel.to.body,
                                 channel.to.reach};
            if (!savedChannels.emplace(key, std::move(channel)).second)
                throw SerializationError("Duplicate reservoir channel");
        }
        const auto pending = in.read<uint32_t>();
        for (uint32_t i = 0; i < pending; ++i) {
            const auto body = in.read<uint32_t>();
            const auto amount = in.read<WaterVolume>();
            if (!body || amount <= 0 ||
                !savedPending.emplace(body, amount).second)
                throw SerializationError("Invalid pending reservoir inflow");
        }
    }
    decltype(springClosedAt) savedClosedAt;
    if (section->version >= 6) {
        const auto count = in.read<uint32_t>();
        for (uint32_t i = 0; i < count; ++i) {
            const auto reach = in.read<uint32_t>();
            const auto step = in.read<uint64_t>();
            if ((section->version < 7 && !savedSealed.contains(reach)) ||
                step > savedStep || !savedClosedAt.emplace(reach, step).second)
                throw SerializationError("Invalid spring closure time");
        }
    }
    decltype(springReopenedAt) savedReopenedAt;
    if (section->version >= 7) {
        const auto count = in.read<uint32_t>();
        for (uint32_t i = 0; i < count; ++i) {
            const auto reach = in.read<uint32_t>();
            const auto step = in.read<uint64_t>();
            const auto closed = savedClosedAt.find(reach);
            const auto rate = springRates.find(reach);
            if (rate == springRates.end() || !rate->second ||
                savedSealed.contains(reach) || step > savedStep ||
                (closed != savedClosedAt.end() && step < closed->second) ||
                !savedReopenedAt.emplace(reach, step).second)
                throw SerializationError("Invalid spring reopening time");
        }
        for (const auto& [reach, step] : savedClosedAt)
            if (!savedSealed.contains(reach) &&
                !savedReopenedAt.contains(reach))
                throw SerializationError("Orphaned spring closure time");
    }
    if (!in.atEnd()) throw SerializationError("Trailing reservoir data");
    auto recoveredOrder = routes(savedDeliveries);
    if (!recoveredOrder) throw SerializationError("Cyclic reservoir route");
    order = std::move(*recoveredOrder);
    stores = std::move(savedStores);
    deliveries = std::move(savedDeliveries);
    totals = savedTotals;
    budgetStep = savedStep;
    activeCells.clear();
    next->cells.forEach([&](TilePos p, uint8_t) { activeCells.insert(p); });
    flowCursor.reset();
    filmSince.clear();
    next->portals = std::move(savedPortals);
    outlets = std::move(savedOutlets);
    if (section->version == 2)
        for (auto& [id, outlet] : outlets)
            if (springRates.contains(outlet.reach)) outlet.open = false;
    nextOutletId = savedNextOutlet;
    spills = std::move(savedSpills);
    for (const auto& [p, breach] : breaches) {
        curves.erase(breach.pool);
        poolFootprints.erase(breach.pool);
    }
    curves.merge(savedCurves);
    poolFootprints.merge(savedFootprints);
    publishedFootprints.reset();
    breaches = std::move(savedBreaches);
    sealedSprings = std::move(savedSealed);
    springClosedAt = std::move(savedClosedAt);
    springReopenedAt = std::move(savedReopenedAt);
    // Older shoreline surveys could persist a runoff mouth inside the
    // lake's own outlet. The reach already accounts for that delivery.
    for (auto& [key, channel] : savedChannels) {
        const auto outlet = lakeOutletReaches.find(channel.from);
        if (outlet == lakeOutletReaches.end()) continue;
        std::erase_if(channel.mouths, [&](const auto& entry) {
            const auto p = entry.first;
            const auto [bed, surface] = generated->sampleColumn(p.x, p.z);
            return surface.fills(p.y, bed) && (surface.body == channel.from ||
                                               surface.reach == outlet->second);
        });
    }
    std::erase_if(savedChannels, [](const auto& entry) {
        const auto& c = entry.second;
        return c.mouths.empty() ||
               (c.from == GeneratedHydrology::Sea && !c.to.body && !c.to.reach);
    });
    channels = std::move(savedChannels);
    flowingMouths.clear();
    channelPathsChanged = true;
    pendingInflow = std::move(savedPending);
    ReservoirChange change;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (it->node & ReachFlag)
            updateRiverStage(uint32_t(it->node), next, change);
    // Side-by-side channels can supply backwater without a delivery edge.
    // Recheck against the restored stages, regardless of graph order.
    const auto stagedReaches = change.reaches;
    for (const auto reach : stagedReaches)
        updateRiverStage(reach, next, change);
    view.store(std::move(next));
    ++serial;
}
