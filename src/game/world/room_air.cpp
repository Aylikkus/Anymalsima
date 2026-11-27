#include "room_air.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "chunk.hpp"
#include "consts.hpp"
#include "world.hpp"
#include "world_climate.hpp"
#include "world_editor.hpp"
#include "world_weather.hpp"
#include "world_wrap.hpp"

namespace room {
namespace {

uint64_t roomKey(const TilePos& p) {
    // Canonical X and Z fit in 24 bits in normal worlds; Y in 16.
    return (uint64_t(uint32_t(p.y)) << 48) | (uint64_t(uint32_t(p.z)) << 24) |
           uint64_t(uint32_t(p.x));
}

TilePos keyCell(uint64_t key) {
    return TilePos{int(key & 0xffffffull), int((key >> 48) & 0xffffull),
                   int((key >> 24) & 0xffffffull)};
}

void aabbDistance(const Definition& definition, const std::array<double, 3>& p,
                  double& distance, std::array<double, 3>& nearest) {
    const auto& b = definition.bounds;
    nearest[0] = std::clamp(p[0], double(b[0]), double(b[3] + 1));
    nearest[1] = std::clamp(p[1], double(b[1]), double(b[4] + 1));
    nearest[2] = std::clamp(p[2], double(b[2]), double(b[5] + 1));
    const double dx = p[0] - nearest[0];
    const double dy = p[1] - nearest[1];
    const double dz = p[2] - nearest[2];
    distance = std::sqrt(dx * dx + dy * dy + dz * dz);
}

// One implicit Euler step for the two-node air/wall model. It is
// unconditionally stable, so the caller can take long steps during a time
// jump without the air or wall store exploding.
void advanceStep(State& state, const Parameters& p, const Definition& def,
                 double h, double outdoorC, double groundC,
                 double sourceOffsetC) {
    if (!(h > 0)) return;
    const double airCapacity =
        p.airCapacityPerVolume * std::max(1, def.airCells);
    const double wallCapacity =
        p.wallCapacityPerArea * std::max(0.5, double(def.wallFaces));
    const double wallAir = p.wallAirConductance * std::max(1, def.wallFaces);
    const double leak = p.leakConductance * double(def.leakFaces);
    const double ground = p.groundConductance * std::max(1, def.wallFaces);
    // With no source (or a thermostat commanding zero inside its deadband)
    // there must be no coupling to `outdoor + 0`: removing the heater cannot
    // keep ventilating the room to the outdoor sample.
    const double sourceCoupling =
        sourceOffsetC == 0
            ? 0
            : airCapacity / std::max(1., p.sourceCouplingSeconds);

    const double target = outdoorC + sourceOffsetC;
    // (A + gs + gwa + gl) T' - gwa W' = A T + gs target + gl out
    // -gwa T' + (B + gwa + gg) W' = B W + gg ground
    const double A = airCapacity / h;
    const double B = wallCapacity / h;
    const double a11 = A + sourceCoupling + wallAir + leak;
    const double a12 = -wallAir;
    const double a21 = -wallAir;
    const double a22 = B + wallAir + ground;
    const double b1 =
        A * state.airC + sourceCoupling * target + leak * outdoorC;
    const double b2 = B * state.wallC + ground * groundC;
    const double det = a11 * a22 - a12 * a21;
    if (std::abs(det) < 1e-12) return;
    const double air = (b1 * a22 - a12 * b2) / det;
    const double wall = (a11 * b2 - a21 * b1) / det;
    state.airC = std::clamp(air, -120., 250.);
    state.wallC = std::clamp(wall, -120., 250.);
}

}  // namespace

RoomAir::RoomAir(const World& world, ChunkManager& chunks,
                 ThermalSourceRegistry& sources, thermal::BarrierFn barrier)
    : world(world),
      chunks(chunks),
      sources(sources),
      barrier(std::move(barrier)) {}

RoomAir::~RoomAir() {
    chunks.becameReady.disconnect(readyConnection);
    chunks.removed.disconnect(removedConnection);
    if (watchedEditor) watchedEditor->blockChanged.disconnect(editorConnection);
}

void RoomAir::watch(WorldEditor& editor) {
    watchedEditor = &editor;
    readyConnection =
        chunks.becameReady.connect([this](const ChunkCoord&) { invalidate(); });
    removedConnection =
        chunks.removed.connect([this](const ChunkCoord&) { invalidate(); });
    editorConnection = editor.blockChanged.connect(
        [this](const BlockChange&) { invalidate(); });
}

void RoomAir::invalidate() { rebuildPending = true; }

double RoomAir::outdoorAt(double x, double y, double z,
                          double worldDays) const {
    return world_weather::at(world, x, z, worldDays, y).temperatureC;
}

double RoomAir::groundAt(double x, double y, double z) const {
    // Ground temperature is the annual mean at the local elevation, without
    // the daily or seasonal wave. A sheltered cave is insulated from it by
    // the wall store and the small ground conductance.
    const auto climate = world_climate::at(world, x, z, 0.0, y);
    return double(climate.meanC + climate.elevationC);
}

bool RoomAir::loadedAir(int x, int y, int z, bool& loaded) const {
    const Tile* tile = chunks.tileAt(x, y, z);
    if (!tile) {
        loaded = false;
        return false;
    }
    loaded = true;
    return !tile->isSolid() && !tile->isLiquid();
}

bool RoomAir::sheltered(int x, int y, int z) {
    const TilePos key{x, y, z};
    if (shelterCache.contains(key)) return true;
    if (exposedCache.contains(key)) return false;
    const int top = std::min(WORLD_HEIGHT_Y - 1, y + params.ceilingHeight);
    for (int above = y + 1; above <= top; ++above) {
        const Tile* tile = chunks.tileAt(x, above, z);
        if (tile && tile->isSolid()) {
            shelterCache.insert(key);
            return true;
        }
    }
    exposedCache.insert(key);
    return false;
}

void RoomAir::rebuild(std::array<double, 3> focus) {
    definitions.clear();
    runtime.clear();
    cellRoom.clear();
    shelterCache.clear();
    exposedCache.clear();
    lastFocus = focus;
    hasFocus = true;
    rebuildPending = false;

    const auto wrap = world.wrap();
    const int zLimit = world.height > 0 ? world.height * WorldWrap::RegionSize
                                        : std::numeric_limits<int>::max() / 2;
    const auto inBounds = [&](int x, int y, int z) {
        return y >= 0 && y < WORLD_HEIGHT_Y && z >= 0 && z < zLimit &&
               (wrap.tiles <= 0 || (x >= 0 && x < wrap.tiles));
    };
    const auto addSeed = [&](std::unordered_set<TilePos>& seeds, int x, int y,
                             int z) {
        if (!inBounds(x, y, z)) return;
        bool loaded = false;
        if (!loadedAir(x, y, z, loaded)) return;
        seeds.insert(TilePos{x, y, z});
    };

    std::unordered_set<TilePos> seeds;
    const int fx = int(std::floor(focus[0]));
    const int fy = int(std::floor(focus[1]));
    const int fz = int(std::floor(focus[2]));
    for (int dy = -2; dy <= 1; ++dy) addSeed(seeds, fx, fy + dy, fz);

    const double focusRadius = double(params.scanRadius);
    sources.forEach(
        [&](ThermalSourceRegistry::Id, const ThermalSource& source) {
            const double dx = wrap.deltaX(source.position[0], focus[0]);
            const double dz = source.position[2] - focus[2];
            if (std::abs(dx) > focusRadius || std::abs(dz) > focusRadius)
                return;
            const int sx = int(std::floor(source.position[0]));
            const int sy = int(std::floor(source.position[1]));
            const int sz = int(std::floor(source.position[2]));
            addSeed(seeds, sx, sy, sz);
            addSeed(seeds, sx + 1, sy, sz);
            addSeed(seeds, sx - 1, sy, sz);
            addSeed(seeds, sx, sy + 1, sz);
            addSeed(seeds, sx, sy - 1, sz);
            addSeed(seeds, sx, sy, sz + 1);
            addSeed(seeds, sx, sy, sz - 1);
        });

    for (const auto& [key, state] : states) {
        (void)state;
        const TilePos cell = keyCell(key);
        const double dx = wrap.deltaX(cell.x, focus[0]);
        if (std::abs(dx) > focusRadius || std::abs(cell.z - fz) > focusRadius)
            continue;
        addSeed(seeds, cell.x, cell.y, cell.z);
    }

    static constexpr int Directions[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::unordered_set<TilePos> seen;
    size_t scanned = 0;
    for (const auto& seed : seeds) {
        if (definitions.size() >= params.maxRooms) break;
        if (scanned >= params.maxScanCells) break;
        if (seen.contains(seed)) continue;
        bool loaded = false;
        if (!loadedAir(seed.x, seed.y, seed.z, loaded) || !loaded) continue;
        if (!sheltered(seed.x, seed.y, seed.z)) {
            seen.insert(seed);
            continue;
        }

        Definition definition;
        definition.minCell = seed;
        std::vector<TilePos> queue{seed};
        seen.insert(seed);
        for (size_t head = 0; head < queue.size(); ++head) {
            if (definition.cells.size() >= params.maxRoomCells ||
                scanned >= params.maxScanCells) {
                definition.approximateFaces += 1;
                break;
            }
            const TilePos cell = queue[head];
            ++scanned;
            definition.cells.push_back(cell);
            if (cell.x < definition.minCell.x ||
                (cell.x == definition.minCell.x &&
                 cell.y < definition.minCell.y) ||
                (cell.x == definition.minCell.x &&
                 cell.y == definition.minCell.y &&
                 cell.z < definition.minCell.z))
                definition.minCell = cell;
            definition.centroid[0] += double(cell.x) + .5;
            definition.centroid[1] += double(cell.y) + .5;
            definition.centroid[2] += double(cell.z) + .5;
            for (const auto& d : Directions) {
                int nx = cell.x + d[0];
                const int ny = cell.y + d[1];
                const int nz = cell.z + d[2];
                if (wrap.tiles > 0) nx = int(wrap.wrapTile(nx));
                if (!inBounds(nx, ny, nz)) {
                    ++definition.leakFaces;
                    continue;
                }
                bool neighbourLoaded = false;
                const bool air = loadedAir(nx, ny, nz, neighbourLoaded);
                if (!neighbourLoaded) {
                    ++definition.leakFaces;
                    ++definition.approximateFaces;
                    continue;
                }
                const Tile* tile = chunks.tileAt(nx, ny, nz);
                if (tile && tile->isSolid()) {
                    ++definition.wallFaces;
                    continue;
                }
                if (tile && tile->isLiquid()) {
                    ++definition.leakFaces;
                    continue;
                }
                if (!air) {
                    ++definition.wallFaces;
                    continue;
                }
                if (!sheltered(nx, ny, nz)) {
                    ++definition.leakFaces;
                    continue;
                }
                const TilePos next{nx, ny, nz};
                if (seen.insert(next).second) queue.push_back(next);
            }
        }
        if (definition.cells.empty()) continue;
        const double count = double(definition.cells.size());
        definition.centroid[0] /= count;
        definition.centroid[1] /= count;
        definition.centroid[2] /= count;
        definition.airCells = int(definition.cells.size());
        definition.key = roomKey(definition.minCell);
        int minX = definition.cells.front().x, maxX = minX;
        int minY = definition.cells.front().y, maxY = minY;
        int minZ = definition.cells.front().z, maxZ = minZ;
        for (const auto& cell : definition.cells) {
            minX = std::min(minX, cell.x);
            maxX = std::max(maxX, cell.x);
            minY = std::min(minY, cell.y);
            maxY = std::max(maxY, cell.y);
            minZ = std::min(minZ, cell.z);
            maxZ = std::max(maxZ, cell.z);
        }
        definition.bounds = {minX, minY, minZ, maxX, maxY, maxZ};
        const size_t index = definitions.size();
        for (const auto& cell : definition.cells) cellRoom[cell] = index;
        definitions.push_back(std::move(definition));
    }
}

State& RoomAir::stateFor(const Definition& definition, double worldDays) {
    auto [it, inserted] = states.try_emplace(definition.key);
    State& state = it->second;
    if (!state.initialized) {
        state.airC = outdoorAt(definition.centroid[0], definition.centroid[1],
                               definition.centroid[2], worldDays);
        state.wallC = groundAt(definition.centroid[0], definition.centroid[1],
                               definition.centroid[2]);
        state.groundC = state.wallC;
        state.lastDay = worldDays;
        state.initialized = true;
    }
    return state;
}

void RoomAir::simulate(const Definition& definition, State& state,
                       double worldDays,
                       std::unordered_set<uint64_t>& claimedThermostats) {
    const double outdoor =
        outdoorAt(definition.centroid[0], definition.centroid[1],
                  definition.centroid[2], worldDays);
    const double ground = groundAt(
        definition.centroid[0], definition.centroid[1], definition.centroid[2]);

    const double elapsed = (worldDays - state.lastDay) * 86400.;
    const int steps =
        elapsed > 0
            ? std::clamp(
                  int(std::ceil(elapsed / std::max(1., params.maxStepSeconds))),
                  1, std::max(1, params.maxSubsteps))
            : 1;
    const double h = elapsed > 0 ? elapsed / steps : 0;

    // Geometry and barriers do not depend on the controller state, so solve
    // them once for this room and only recompute commands per substep.
    struct Affected {
        ThermalSourceRegistry::Id id;
        const ThermalSource* source;
        float distance;
        double barrierFactor;
    };
    std::vector<Affected> affected;
    sources.forEach(
        [&](ThermalSourceRegistry::Id id, const ThermalSource& source) {
            if (!source.emission.emits()) return;
            std::array<double, 3> nearest{};
            double distance = 0;
            aabbDistance(definition, source.position, distance, nearest);
            // A block occupying a face cell is at the wall surface.
            distance = std::max(0., distance - .5);
            if (distance >= source.emission.radius) return;
            double factor = 1;
            if (barrier) {
                const double blocked = barrier(source.position, nearest);
                factor = std::exp(-blocked / thermal::BarrierEfoldMetres);
            }
            affected.push_back({id, &source, float(distance), factor});
        });

    Runtime info;
    for (int step = 0; step < steps; ++step) {
        double sourceOffset = 0;
        info = Runtime{};
        float strongestCommand = 0;
        bool saturated = false;
        for (const auto& entry : affected) {
            const ThermalSource& source = *entry.source;
            float command = source.commandDeltaC;
            if (source.emission.thermostat()) {
                auto& controller = controllers[source.key];
                command = source.emission.updateCommand(state.airC, controller);
                sources.setCommand(entry.id, command, controller.saturated);
                claimedThermostats.insert(source.key);
                if (std::abs(command) >= std::abs(strongestCommand)) {
                    strongestCommand = command;
                    saturated = controller.saturated;
                }
            }
            sourceOffset += source.emission.offsetAt(entry.distance, command) *
                            entry.barrierFactor;
            ++info.sources;
        }
        info.commandC = strongestCommand;
        info.saturated = saturated;
        if (h > 0)
            advanceStep(state, params, definition, h, outdoor, ground,
                        sourceOffset);
    }
    runtime[definition.key] = info;
    state.groundC = ground;
    state.lastDay = worldDays;
    if (elapsed > 0) stateDirty = true;
}

void RoomAir::updateUnclaimedThermostats(
    double worldDays, const std::unordered_set<uint64_t>& claimed) {
    sources.forEach([&](ThermalSourceRegistry::Id id,
                        const ThermalSource& source) {
        if (!source.emission.thermostat() || !source.emission.emits()) return;
        if (claimed.contains(source.key)) return;
        auto& controller = controllers[source.key];
        const double ambient = outdoorAt(source.position[0], source.position[1],
                                         source.position[2], worldDays);
        const float command =
            source.emission.updateCommand(float(ambient), controller);
        sources.setCommand(id, command, controller.saturated);
    });
}

void RoomAir::update(std::array<double, 3> focus, double worldDays) {
    const double dx = focus[0] - lastFocus[0];
    const double dz = focus[2] - lastFocus[2];
    const double moved = std::sqrt(dx * dx + dz * dz);
    if (rebuildPending || !hasFocus || moved > params.rescanDistance)
        rebuild(focus);

    std::unordered_set<uint64_t> claimed;
    for (const auto& definition : definitions) {
        State& state = stateFor(definition, worldDays);
        simulate(definition, state, worldDays, claimed);
    }
    updateUnclaimedThermostats(worldDays, claimed);
}

bool RoomAir::sample(std::array<double, 3> position, double,
                     thermal::RoomSample& out) const {
    const TilePos cell{int(std::floor(position[0])),
                       int(std::floor(position[1])),
                       int(std::floor(position[2]))};
    const auto found = cellRoom.find(cell);
    if (found == cellRoom.end() || found->second >= definitions.size())
        return false;
    const auto& definition = definitions[found->second];
    const auto stateIt = states.find(definition.key);
    if (stateIt == states.end() || !stateIt->second.initialized) return false;
    const auto runtimeIt = runtime.find(definition.key);
    out.indoors = true;
    out.airC = float(stateIt->second.airC);
    out.wallC = float(stateIt->second.wallC);
    out.groundC = float(stateIt->second.groundC);
    out.volume = definition.airCells;
    out.wallFaces = definition.wallFaces;
    out.leakFaces = definition.leakFaces;
    out.approximate = definition.approximateFaces > 0;
    if (runtimeIt != runtime.end()) {
        out.sources = runtimeIt->second.sources;
        out.commandC = runtimeIt->second.commandC;
        out.saturated = runtimeIt->second.saturated;
    }
    return true;
}

void RoomAir::fillDebug(const Definition& definition, Debug& debug) const {
    const auto stateIt = states.find(definition.key);
    if (stateIt == states.end() || !stateIt->second.initialized) return;
    const auto& state = stateIt->second;
    debug.valid = true;
    debug.key = definition.key;
    debug.airC = float(state.airC);
    debug.wallC = float(state.wallC);
    debug.groundC = float(state.groundC);
    debug.volume = definition.airCells;
    debug.wallFaces = definition.wallFaces;
    debug.leakFaces = definition.leakFaces;
    debug.approximateFaces = definition.approximateFaces;
    debug.lastDay = state.lastDay;
    if (const auto it = runtime.find(definition.key); it != runtime.end()) {
        debug.sources = it->second.sources;
        debug.commandC = it->second.commandC;
        debug.saturated = it->second.saturated;
    }
}

RoomAir::Debug RoomAir::debugAt(std::array<double, 3> position) const {
    Debug debug;
    const TilePos cell{int(std::floor(position[0])),
                       int(std::floor(position[1])),
                       int(std::floor(position[2]))};
    const auto found = cellRoom.find(cell);
    if (found == cellRoom.end() || found->second >= definitions.size())
        return debug;
    fillDebug(definitions[found->second], debug);
    return debug;
}

RoomAir::Debug RoomAir::nearest(std::array<double, 3> position,
                                double maxDistance) const {
    Debug best;
    double bestDistance = maxDistance;
    for (const auto& definition : definitions) {
        std::array<double, 3> point{};
        double distance = 0;
        aabbDistance(definition, position, distance, point);
        if (distance > bestDistance) continue;
        Debug candidate;
        fillDebug(definition, candidate);
        if (!candidate.valid) continue;
        bestDistance = distance;
        best = candidate;
    }
    return best;
}

std::vector<Record> RoomAir::saveRecords() const {
    std::vector<Record> records;
    // Persist every known significant store, not only the rooms inside the
    // current scan window: a warm building must survive the player travelling
    // away before saving.
    for (const auto& [key, state] : states) {
        if (!state.initialized) continue;
        if (std::abs(state.airC - state.groundC) <= params.storeThresholdC &&
            std::abs(state.wallC - state.groundC) <= params.storeThresholdC)
            continue;
        const TilePos cell = keyCell(key);
        records.push_back({cell.x, cell.y, cell.z, float(state.airC),
                           float(state.wallC), state.lastDay});
    }
    // Keep the strongest stores if an explored world accumulated more rooms
    // than the save section accepts, then restore a deterministic order.
    constexpr size_t MaxSavedRooms = 4096;
    if (records.size() > MaxSavedRooms) {
        std::sort(records.begin(), records.end(),
                  [](const Record& a, const Record& b) {
                      const float sa = std::abs(a.wallC) + std::abs(a.airC);
                      const float sb = std::abs(b.wallC) + std::abs(b.airC);
                      return sa > sb;
                  });
        records.resize(MaxSavedRooms);
    }
    std::sort(records.begin(), records.end(),
              [](const Record& a, const Record& b) {
                  if (a.x != b.x) return a.x < b.x;
                  if (a.y != b.y) return a.y < b.y;
                  return a.z < b.z;
              });
    return records;
}

void RoomAir::loadRecords(const std::vector<Record>& records) {
    for (const auto& record : records) {
        State state;
        state.airC = record.airC;
        state.wallC = record.wallC;
        state.groundC = groundAt(record.x + .5, record.y + .5, record.z + .5);
        state.lastDay = record.lastDay;
        state.initialized = true;
        states[roomKey(TilePos{record.x, record.y, record.z})] = state;
    }
    if (!records.empty()) rebuildPending = true;
}

}  // namespace room
