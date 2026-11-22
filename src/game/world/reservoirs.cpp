#include "reservoirs.hpp"

#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

#include "generated_hydrology.hpp"
#include "serialization/section_container.hpp"

namespace {
constexpr auto MaxVolume = std::numeric_limits<WaterVolume>::max();
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

int ReservoirSnapshot::localTop(int x, int z) const {
    const auto it = localTops.find(columnKey(x, z));
    return it == localTops.end() ? -1 : it->second;
}

int ReservoirSnapshot::level(uint32_t body, int baseline) const {
    const auto it = levels.find(body);
    return it == levels.end() ? baseline : it->second;
}

WaterSurface ReservoirSnapshot::projectStanding(
    const GeneratedHydrology* hydrology, int x, int z, int ground,
    WaterSurface baseline) const {
    if (baseline.body && !baseline.reach) {
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
            baseline.surfaceY = height;
            if (const auto it = surfaceFills.find(baseline.body);
                it != surfaceFills.end())
                baseline.topFill = it->second;
            baseline.softenShore(ground);
        }
    } else if (const auto it = riverStages.find(baseline.reach);
               it != riverStages.end()) {
        const auto& stage = it->second;
        if (baseline.surfaceY > stage.head) return baseline;
        if (baseline.surfaceY == stage.head &&
            (stage.downstream.x || stage.downstream.y)) {
            const auto dx = WorldWrap::delta(x, stage.cut.x, stage.periodX);
            if (dx * stage.downstream.x +
                    int64_t(z - stage.cut.z) * stage.downstream.y <=
                0)
                return baseline;
        }
        const int depth =
            ((baseline.depth(ground) - 1) * 255 + baseline.topFill) *
            int(stage.percent) / 100;
        if (depth <= 0) return {};
        baseline.surfaceY = ground + 1 + (depth + 254) / 255;
        baseline.topFill = uint8_t((depth - 1) % 255 + 1);
    }
    return baseline;
}

WaterSurface ReservoirSnapshot::project(const GeneratedHydrology* hydrology,
                                        int x, int z, int ground,
                                        WaterSurface baseline) const {
    return projectLocal(x, z, ground,
                        projectStanding(hydrology, x, z, ground, baseline));
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
                if (pool.level <= floor ||
                    (!poolGround && pool.level <= ground + 1))
                    continue;
                if (pool.level <= sill) continue;
                const WaterSurface surface{pool.level, body, 0,
                                           {},         true, pool.fill};
                if (surface.height() > baseline.height()) {
                    baseline = surface;
                    if (poolGround) *poolGround = floor - 1;
                }
            }
        }
    }
    // Reconciliation looks up exact local credits per voxel separately.
    if (poolGround) return baseline;
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
    if (this->generated)
        for (const auto& reach : this->generated->riverReaches())
            springRates.emplace(
                reach.id, reach.lakeSource ? 0 : reach.spring.unitsPerStep);
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
    view.store(std::move(next));
    if (!change.bodies.empty() || !change.reaches.empty() || change.local ||
        !change.locals.empty()) {
        auto emitted = change;
        if (!emitted.bodies.empty() && !poolFootprints.empty()) {
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
    const auto it = current->cells.find(recipient);
    const auto fill = it == current->cells.end() ? 0 : it->second;
    const auto before = volume(body);
    const auto accepted = std::min({amount, before, WaterUnitsPerBlock - fill});
    if (!accepted) return 0;
    auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
    next->cells[recipient] = uint8_t(fill + accepted);
    auto& top =
        next->localTops[ReservoirSnapshot::columnKey(recipient.x, recipient.z)];
    top = std::max(top, recipient.y + 1);
    stores[body] = before - accepted;
    ++serial;
    ReservoirChange change;
    change.local = recipient;
    updateLevel(*next, *shape, body, change);
    publish(std::move(next), change);
    return accepted;
}

namespace {
WaterVolume fillCut(ReservoirSnapshot& next, ReservoirChange& change,
                    TilePos mouth, WaterVolume remaining, uint64_t elapsed,
                    const Reservoirs::CellQuery& access, int periodX) {
    // Bulk skip: visit only confirmed open cells connected downhill or
    // horizontally from the outlet. Fill the low ground first. Unknown
    // voxels have no receiving capacity, and a full channel stops the debit.
    using Node = std::tuple<int, int, int, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<>> pending;
    std::unordered_set<TilePos> seen;
    constexpr size_t MaxCells = 4096;
    const auto enqueue = [&](TilePos p, int distance) {
        if (periodX > 0) p.x = int(WorldWrap::mod(p.x, periodX));
        if (p.x < 0 || p.z < 0 || p.y < 0 || uint64_t(distance) >= elapsed ||
            seen.size() >= MaxCells || !seen.insert(p).second ||
            access(p) != Reservoirs::CellAccess::Open)
            return;
        pending.emplace(p.y, distance, p.z, p.x);
    };
    enqueue(mouth, 0);
    WaterVolume moved = 0;
    while (!pending.empty() && remaining > 0) {
        const auto [y, distance, z, x] = pending.top();
        pending.pop();
        const TilePos p{x, y, z};
        for (const auto q :
             {TilePos{x, y - 1, z}, TilePos{x - 1, y, z}, TilePos{x + 1, y, z},
              TilePos{x, y, z - 1}, TilePos{x, y, z + 1}})
            enqueue(q, distance + 1);
        // Leave air above a downspout if water can go directly down.
        const TilePos below{x, y - 1, z};
        const auto belowFill = next.cells.find(below);
        if (y > 0 && uint64_t(distance + 1) < elapsed &&
            access(below) == Reservoirs::CellAccess::Open &&
            (belowFill == next.cells.end() || belowFill->second < 255))
            continue;
        const auto cell = next.cells.find(p);
        const int fill = cell == next.cells.end() ? 0 : cell->second;
        const auto accepted = std::min(remaining, WaterUnitsPerBlock - fill);
        if (!accepted) continue;
        next.cells[p] = uint8_t(fill + accepted);
        auto& top = next.localTops[ReservoirSnapshot::columnKey(x, z)];
        top = std::max(top, y + 1);
        change.locals.push_back(p);
        remaining -= accepted;
        moved += accepted;
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
    if (elapsed == 1) return divert(body, mouth, available);
    auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
    ReservoirChange change;
    const auto moved =
        fillCut(*next, change, mouth, available, elapsed, access, periodX);
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
    const auto it = current->cells.find(recipient);
    const int fill = it == current->cells.end() ? 0 : it->second;
    const auto accepted = std::min<WaterVolume>(amount, 255 - fill);
    if (!accepted) return 0;
    auto next = std::make_shared<ReservoirSnapshot>(*current);
    next->cells[recipient] = uint8_t(fill + accepted);
    auto& top =
        next->localTops[ReservoirSnapshot::columnKey(recipient.x, recipient.z)];
    top = std::max(top, recipient.y + 1);
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
    ++serial;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    updateRiverStage(reach, next, change);
    if (next) publish(std::move(next), change);
    return true;
}

std::optional<Reservoirs::BreachSurvey> Reservoirs::surveyBreach(
    Vec2i opening, int head, int periodX, const SurfaceFloor& floor) {
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
                    landing = q;
                    elevation = *y;
                    descended = true;
                    break;
                }
                if (*y == elevation && seen.insert(key(q)).second)
                    flat.push_back(q);
            }
        if (!descended) break;
    }
    if (landing == opening) return {};
    // Certify the connected bowl and each column's admission height. If the
    // frontier is unknown or too large, do not invent a closed reservoir.
    using Entry = std::tuple<int, int, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pending;
    pending.emplace(*at(landing), landing.x, landing.y);
    std::unordered_set<uint64_t> seen{key(landing)};
    BreachSurvey result{head, {}};
    while (!pending.empty()) {
        const auto [sill, x, z] = pending.top();
        pending.pop();
        const Vec2i p{x, z};
        result.columns.push_back({p, *at(p), sill});
        for (const auto q : neighbors(p)) {
            if (!seen.insert(key(q)).second) continue;
            const auto y = at(q);
            if (!y) return {};
            if (*y < head) pending.emplace(std::max(sill, *y), q.x, q.y);
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

bool Reservoirs::hasBreach(TilePos p) const {
    const auto it = breaches.find(p);
    return it != breaches.end() && it->second.open;
}

bool Reservoirs::openBreach(uint32_t reach, TilePos p, BreachSurvey survey) {
    return openConnection(reach, p, std::move(survey), false);
}

bool Reservoirs::openDam(uint32_t reach, TilePos p, BreachSurvey survey) {
    return openConnection(reach, p, std::move(survey), true);
}

bool Reservoirs::detachLake(uint32_t parent, TilePos seed, BreachSurvey survey,
                            WaterVolume trapped) {
    const auto* parentCurve = curve(parent);
    if (!parentCurve || breaches.contains(seed) || survey.columns.empty() ||
        trapped <= 0 || trapped > volume(parent))
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
    updateLevel(*next, *parentCurve, parent, change);
    updateLevel(*next, shape, body, change);
    ++serial;
    publish(std::move(next), change);
    return true;
}

std::optional<TilePos> Reservoirs::damAt(uint32_t reach) const {
    for (const auto& [p, breach] : breaches)
        if (breach.reach == reach && breach.open && breach.dam) return p;
    return {};
}

bool Reservoirs::openConnection(uint32_t reach, TilePos p, BreachSurvey survey,
                                bool dam) {
    if (!springRates.contains(reach) || survey.head <= p.y ||
        survey.head > WORLD_HEIGHT_Y || survey.columns.empty() ||
        survey.columns.size() > 8192 || p.y < 0 ||
        !generated->containsColumn(p.x, p.z))
        return false;
    if (auto it = breaches.find(p); it != breaches.end()) {
        if (it->second.reach != reach || it->second.dam != dam) return false;
        if (it->second.open) return true;
        if (it->second.survey.head != survey.head ||
            it->second.survey.columns != survey.columns)
            return false;
        it->second.open = true;
    } else {
        uint32_t body = 0x80000000u;
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
        if (!definePool(body, breachCurve(survey), std::move(footprint)))
            return false;
        auto next = std::make_shared<ReservoirSnapshot>(*snapshot());
        next->pools.at(body).footprint = std::move(geometry);
        publish(std::move(next), {});
        breaches.emplace(
            p, Breach{reach, body, std::move(survey), true, false, dam});
    }
    ++serial;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
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
    updateRiverStage(it->second.reach, next, change);
    if (next) publish(std::move(next), change);
    return true;
}

void Reservoirs::updateRiverStage(uint32_t reach,
                                  std::shared_ptr<ReservoirSnapshot>& next,
                                  ReservoirChange& change) {
    ReservoirSnapshot::RiverStage stage;
    if (const auto it = deliveries.find(reach); it != deliveries.end())
        stage.percent = it->second.percent;
    if (sealedSprings.contains(reach)) stage.percent = 0;
    bool hasBreachStage = false;
    for (const auto& [p, breach] : breaches)
        if (breach.reach == reach && breach.open &&
            volume(breach.pool) < curves.at(breach.pool).capacity()) {
            if (breach.dam && stage.percent) {
                const auto channel = generated->riverAt(p.x, p.z);
                if (!channel) continue;
                const auto flow = channel->water.flow;
                const int dx = std::abs(flow.x) >= std::abs(flow.y)
                                   ? (flow.x > 0 ? 1 : -1)
                                   : 0;
                stage = {channel->water.surfaceY,
                         0,
                         p,
                         {dx, dx ? 0 : (flow.y > 0 ? 1 : -1)},
                         generated->periodX()};
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
                        const auto flow = channel->water.flow;
                        stage.cut = p;
                        stage.periodX = period;
                        stage.downstream = std::abs(flow.x) >= std::abs(flow.y)
                                               ? Vec2i{flow.x > 0 ? 1 : -1, 0}
                                               : Vec2i{0, flow.y > 0 ? 1 : -1};
                        break;
                    }
                }
                hasBreachStage = true;
            }
        }
    const auto view = snapshot();
    const auto* current = next ? next.get() : view.get();
    const auto it = current->riverStages.find(reach);
    if (stage.percent == 100
            ? it == current->riverStages.end()
            : it != current->riverStages.end() && it->second == stage)
        return;
    if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
    if (stage.percent == 100)
        next->riverStages.erase(reach);
    else
        next->riverStages[reach] = stage;
    change.reaches.push_back(reach);
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
        const auto cellIt = next->cells.find(cell);
        const int fill = cellIt == next->cells.end() ? 0 : cellIt->second;
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
                        next->cells.erase(cell);
                    else
                        next->cells[cell] = uint8_t(fill - 1);
                    stores[body] = before + accepted;
                    totals[size_t(ReservoirExchange::River)] =
                        add(totals[size_t(ReservoirExchange::River)], accepted);
                    updateLevel(*next, *shape, body, change);
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
            next->cells[cell] = uint8_t(fill + int(delta));
            auto& top =
                next->localTops[ReservoirSnapshot::columnKey(cell.x, cell.z)];
            top = std::max(top, cell.y + 1);
        } else if (fill == 1) {
            next->cells.erase(cell);
        } else {
            next->cells[cell] = uint8_t(fill - 1);
        }
        totals[size_t(ReservoirExchange::River)] =
            add(totals[size_t(ReservoirExchange::River)], delta);
        updateLevel(*next, *shape, body, change);
        moved = true;
    }
    if (moved) {
        next->localTops.clear();
        for (const auto& [p, fill] : next->cells)
            if (fill) {
                auto& top =
                    next->localTops[ReservoirSnapshot::columnKey(p.x, p.z)];
                top = std::max(top, p.y + 1);
            }
        ++serial;
        publish(std::move(next), change);
    }
}

size_t Reservoirs::flow(size_t budget, const CellQuery& access, int periodX,
                        const SinkQuery& sink) {
    if (!budget || !access) return 0;
    const auto current = snapshot();
    if (current->cells.empty()) return 0;
    std::vector<TilePos> active;
    active.reserve(current->cells.size());
    for (const auto& [p, fill] : current->cells)
        if (fill) active.push_back(p);
    // Lowest cells first; ties are stable across hash-map iteration and reload.
    std::sort(active.begin(), active.end(), [](TilePos a, TilePos b) {
        return std::tie(a.y, a.z, a.x) < std::tie(b.y, b.z, b.x);
    });
    std::rotate(active.begin(), active.begin() + flowCursor % active.size(),
                active.end());
    auto next = std::make_shared<ReservoirSnapshot>(*current);
    ReservoirChange change;
    size_t visited = 0;
    auto fillAt = [&](TilePos p) -> int {
        const auto it = next->cells.find(p);
        return it == next->cells.end() ? 0 : it->second;
    };
    for (TilePos source : active) {
        if (visited++ >= budget) break;
        if (access(source) != CellAccess::Open) continue;
        const auto canonical = [periodX](TilePos p) {
            if (periodX > 0) {
                p.x %= periodX;
                if (p.x < 0) p.x += periodX;
            }
            return p;
        };
        // Water resting on a lake, the sea or a river joins it. Without
        // this a channel into a lower body clogs and its source stops.
        if (const TilePos below{source.x, source.y - 1, source.z};
            sink && below.y >= 0 && access(below) == CellAccess::Blocked)
            if (const auto target = sink(below)) {
                const auto fill = fillAt(source);
                if (!fill) continue;
                next->cells.erase(source);
                change.locals.push_back(source);
                receive(*target, fill, next, change);
                continue;
            }
        const auto open = [&](TilePos p) {
            return p.x >= 0 && p.z >= 0 && p.y >= 0 && p.y < WORLD_HEIGHT_Y &&
                   access(p) == CellAccess::Open;
        };
        const auto transfer = [&](TilePos dest, bool downward) {
            dest = canonical(dest);
            if (dest == source || !open(dest)) return;
            const int from = fillAt(source), to = fillAt(dest);
            const int amount = downward ? std::min(from, 255 - to)
                                        : std::clamp((from - to) / 2, 0, 32);
            if (amount <= 0) return;
            if (from == amount)
                next->cells.erase(source);
            else
                next->cells[source] = uint8_t(from - amount);
            next->cells[dest] = uint8_t(to + amount);
            change.locals.push_back(source);
            change.locals.push_back(dest);
        };
        transfer({source.x, source.y - 1, source.z}, true);
        if (fillAt(source) < 2) continue;
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
    flowCursor += std::min(visited, budget);
    if (change.locals.empty()) return std::min(visited, budget);
    next->localTops.clear();
    for (const auto& [p, fill] : next->cells)
        if (fill) {
            auto& top = next->localTops[ReservoirSnapshot::columnKey(p.x, p.z)];
            top = std::max(top, p.y + 1);
        }
    ++serial;
    publish(std::move(next), change);
    return std::min(visited, budget);
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
    if (!from || from == to.body || (!to.body && !to.reach) ||
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
    ++serial;
    return true;
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
            const auto rate = ChannelUnitsPerMouth * count;
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
        const auto moved = before - current;
        if (moved <= 0) continue;
        stores[channel.from] = current;
        if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
        updateLevel(*next, *shape, channel.from, change);
        receive(channel.to, moved, next, change);
    }
}

bool Reservoirs::needsFixedSteps() const {
    const auto state = snapshot();
    return !state->cells.empty() || !state->portals.empty() ||
           !spills.empty() ||
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
        } else {
            // At a full through-lake, leave half of normal inflow for its
            // outlet. A terminal body loses the entire normal delivery.
            const auto loss = next ? amount / 2 : amount;
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
    if (order.empty()) {
        if (next) publish(std::move(next), change);
        return;
    }
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
            amount = add(amount, scale(rate, elapsed));
            if (const auto it = byReach.find(id); it != byReach.end()) {
                bool blocked = false;
                for (const auto* intake : it->second) {
                    blocked |= intake->blocked;
                    const auto current = snapshot();
                    if (elapsed > 1 && access && amount) {
                        if (!next)
                            next =
                                std::make_shared<ReservoirSnapshot>(*current);
                        amount -= fillCut(*next, change, intake->cell,
                                          std::min(amount, scale(255, elapsed)),
                                          elapsed, access, periodX);
                        continue;
                    }
                    const auto& cells = next ? next->cells : current->cells;
                    const auto cell = cells.find(intake->cell);
                    const int fill = cell == cells.end() ? 0 : cell->second;
                    const auto accepted =
                        std::min(amount, WaterUnitsPerBlock - fill);
                    if (!accepted) continue;
                    if (!next)
                        next = std::make_shared<ReservoirSnapshot>(*current);
                    next->cells[intake->cell] = uint8_t(fill + accepted);
                    auto& top = next->localTops[ReservoirSnapshot::columnKey(
                        intake->cell.x, intake->cell.z)];
                    top = std::max(top, intake->cell.y + 1);
                    change.locals.push_back(intake->cell);
                    amount -= accepted;
                }
                if (blocked) {
                    auto& delivery = deliveries[id];
                    delivery.held = add(delivery.held, amount);
                    continue;
                }
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
                if (breach.reach != id || !breach.open || !amount) continue;
                const auto& shape = curves.at(breach.pool);
                const auto before = volume(breach.pool);
                if (before == shape.capacity()) continue;
                const auto offered =
                    breach.dam ? amount
                               : amount / 2 + (amount % 2 && breach.oddUnit);
                if (!breach.dam) breach.oddUnit ^= bool(amount % 2);
                const auto accepted =
                    std::min(offered, shape.capacity() - before);
                if (!accepted) continue;
                stores[breach.pool] = before + accepted;
                amount -= accepted;
                if (!next)
                    next = std::make_shared<ReservoirSnapshot>(*snapshot());
                updateLevel(*next, shape, breach.pool, change);
                if (stores.at(breach.pool) == shape.capacity())
                    updateRiverStage(id, next, change);
            }
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
        if (target && amount) incoming[target] = add(incoming[target], amount);
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
    for (const auto& [p, fill] : current->cells)
        ordered[{p.x, p.y, p.z}] = fill;
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
    SectionContainer file;
    file.put("RSVR", 5, out.take());
    return file.encode();
}

void Reservoirs::restore(ByteView bytes) {
    const auto file = SectionContainer::decode(bytes);
    const auto* section = file.find("RSVR");
    if (!section || section->version > 5 || !section->version)
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
            if (p.y < 0 || head <= p.y || head > WORLD_HEIGHT_Y ||
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
            next->pools.emplace(
                body, PoolState{-1, 255, std::move(geometry), parent});
            savedCurves.emplace(body, breachCurve(survey));
            if (!savedBreaches
                     .emplace(p, Breach{reach, body, std::move(survey),
                                        bool(open & 1), bool(odd),
                                        bool(open & 2), parent})
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
            !next->cells.emplace(p, fill).second)
            throw SerializationError("Invalid local water store");
        auto& top = next->localTops[ReservoirSnapshot::columnKey(p.x, p.z)];
        top = std::max(top, p.y + 1);
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
                (!channel.to.body && !channel.to.reach) ||
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
    if (!in.atEnd()) throw SerializationError("Trailing reservoir data");
    auto recoveredOrder = routes(savedDeliveries);
    if (!recoveredOrder) throw SerializationError("Cyclic reservoir route");
    order = std::move(*recoveredOrder);
    stores = std::move(savedStores);
    deliveries = std::move(savedDeliveries);
    totals = savedTotals;
    budgetStep = savedStep;
    flowCursor = 0;
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
    channels = std::move(savedChannels);
    pendingInflow = std::move(savedPending);
    ReservoirChange change;
    for (const auto& [id, rate] : springRates)
        updateRiverStage(id, next, change);
    view.store(std::move(next));
    ++serial;
}
