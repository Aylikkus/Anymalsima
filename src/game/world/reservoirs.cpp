#include "reservoirs.hpp"

#include <limits>
#include <stdexcept>

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

uint64_t ReservoirSnapshot::columnKey(int x, int z) {
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(z);
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
        const int height = level(baseline.body, baseline.surfaceY);
        if (height <= ground + 1 ||
            (height < baseline.surfaceY && hydrology &&
             !hydrology->connectedAt(baseline.body, x, z, height)))
            baseline = {};
        else
            baseline.surfaceY = height;
    }
    return baseline;
}

WaterSurface ReservoirSnapshot::project(const GeneratedHydrology* hydrology,
                                        int x, int z, int ground,
                                        WaterSurface baseline) const {
    baseline = projectStanding(hydrology, x, z, ground, baseline);
    const int local = localTop(x, z);
    if (local > std::max(baseline.surfaceY, ground + 1))
        return {local, 0, 0, {}, true};
    return baseline;
}

bool ReservoirChange::affects(const GeneratedHydrology* hydrology, int x0,
                              int z0, int x1, int z1, int period) const {
    if (local && local->z >= z0 && local->z < z1) {
        const auto inside = [&](int x) { return x >= x0 && x < x1; };
        if (inside(local->x) || (period > 0 && (inside(local->x + period) ||
                                                inside(local->x - period))))
            return true;
    }
    return hydrology &&
           std::any_of(bodies.begin(), bodies.end(), [&](uint32_t id) {
               return hydrology->mayContainBody(id, x0, z0, x1, z1);
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
    const int level = shape.levelAt(stores.at(body));
    if (next.level(body, baseline) == level) return;
    if (level == baseline)
        next.levels.erase(body);
    else
        next.levels[body] = level;
    change.bodies.push_back(body);
}

void Reservoirs::publish(std::shared_ptr<ReservoirSnapshot> next,
                         const ReservoirChange& change) {
    view.store(std::move(next));
    if (!change.bodies.empty() || change.local) changed.emit(change);
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
    if (shape->levelAt(before) != shape->levelAt(stores.at(body))) {
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
    ReservoirChange change{{}, recipient};
    updateLevel(*next, *shape, body, change);
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
    return true;
}

WaterVolume Reservoirs::held(uint32_t reach) const {
    const auto it = deliveries.find(reach);
    return it == deliveries.end() ? 0 : it->second.held;
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
    if (!stores.contains(body) &&
        (amount == loss || (outlet && amount > loss))) {
        if (outlet) overflow = amount - loss;
        totals[size_t(ReservoirExchange::River)] =
            add(totals[size_t(ReservoirExchange::River)], loss);
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
        if (shape->levelAt(before) != shape->levelAt(stores.at(body))) {
            if (!next) next = std::make_shared<ReservoirSnapshot>(*snapshot());
            updateLevel(*next, *shape, body, change);
        }
        if (stores.at(body) == shape->capacity()) stores.erase(body);
    }
}

void Reservoirs::advanceTo(uint64_t step) {
    if (step <= budgetStep) return;
    const auto elapsed = step - budgetStep;
    budgetStep = step;
    ++serial;
    if (order.empty()) return;
    std::unordered_map<Node, WaterVolume> incoming;
    std::shared_ptr<ReservoirSnapshot> next;
    ReservoirChange change;
    for (const auto [node, target] : order) {
        auto amount = incoming[node];
        if (node & ReachFlag) {
            const auto id = uint32_t(node);
            amount = add(amount, scale(springRates.at(id), elapsed));
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
    if (!change.bodies.empty()) publish(std::move(next), change);
}

Bytes Reservoirs::encode() const {
    BinaryWriter out;
    out.write(budgetStep);
    for (auto value : totals) out.write(value);
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
    SectionContainer file;
    file.put("RSVR", 1, out.take());
    return file.encode();
}

void Reservoirs::restore(ByteView bytes) {
    const auto file = SectionContainer::decode(bytes);
    const auto* section = file.find("RSVR");
    if (!section || section->version != 1)
        throw SerializationError("Unsupported reservoir ledger");
    BinaryReader in(section->data);
    const auto savedStep = in.read<uint64_t>();
    Totals savedTotals{};
    for (auto& value : savedTotals) in.read(value);
    std::map<uint32_t, WaterVolume> savedStores;
    auto next = std::make_shared<ReservoirSnapshot>();
    const auto bodies = in.read<uint32_t>();
    for (uint32_t i = 0; i < bodies; ++i) {
        const auto id = in.read<uint32_t>();
        const auto volume = in.read<WaterVolume>();
        const auto* shape = curve(id);
        if (!shape || volume < 0 || volume > shape->capacity() ||
            !savedStores.emplace(id, volume).second)
            throw SerializationError("Invalid reservoir store");
        if (volume != shape->capacity())
            next->levels[id] = shape->levelAt(volume);
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
            (state.receiver && !curve(state.receiver)) ||
            !savedDeliveries.emplace(id, state).second)
            throw SerializationError("Invalid reach delivery");
    }
    if (!in.atEnd()) throw SerializationError("Trailing reservoir data");
    auto recoveredOrder = routes(savedDeliveries);
    if (!recoveredOrder) throw SerializationError("Cyclic reservoir route");
    order = std::move(*recoveredOrder);
    stores = std::move(savedStores);
    deliveries = std::move(savedDeliveries);
    totals = savedTotals;
    budgetStep = savedStep;
    view.store(std::move(next));
    ++serial;
}
