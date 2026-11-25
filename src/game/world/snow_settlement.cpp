#include "snow_settlement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <span>
#include <tuple>
#include <vector>

namespace snow {
namespace {

using tile_geometry::CellGeometry;
using Footprint = std::pair<int, int>;

struct TilePosLess {
    bool operator()(const TilePos& a, const TilePos& b) const {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

constexpr float SillTolerance = 1e-5f;
constexpr double VolumeTolerance = 1e-6;  // cm3
constexpr int MaxOpeningLookup = 64;
constexpr size_t MaxSerializedColumns = 1 << 22;
constexpr size_t MaxSerializedPending = 1 << 20;
constexpr int LevelIterations = 44;

struct ColumnInfo {
    int x = 0;
    int z = 0;
    int baseY = 0;
    float baseLocal = 0;

    double baseWorld() const { return double(baseY) + baseLocal; }
};

struct WorkBudget {
    size_t limit = 0;
    size_t used = 0;

    bool spend(size_t amount = 1) {
        used += amount;
        return used <= limit;
    }
    bool exhausted() const { return used > limit; }
};

class TerrainCache {
   public:
    TerrainCache(const SnowTerrain& terrain, WorkBudget& budget)
        : terrain(terrain), budget(budget) {}

    const std::optional<CellGeometry>& at(TilePos pos) {
        const auto found = cells.find(pos);
        if (found != cells.end()) return found->second;
        budget.spend();
        if (budget.exhausted()) return missing;
        const auto [entry, inserted] = cells.emplace(pos, terrain.cell(pos));
        (void)inserted;
        return entry->second;
    }

    const SnowTerrain& terrain;
    WorkBudget& budget;

   private:
    std::map<TilePos, std::optional<CellGeometry>, TilePosLess> cells;
    std::optional<CellGeometry> missing;
};

struct Edge {
    int a = 0;
    int b = 0;
    float sill = 0;
};

struct Basin {
    std::set<int> columns;
    double volume = 0;  // cm3
    double level = 0;   // world metres
};

std::optional<ColumnInfo> findBase(int x, int z, int fromY,
                                   TerrainCache& cache) {
    const auto& terrain = cache.terrain;
    const int top = std::min(fromY, terrain.maxY() - 1);
    for (int y = top; y >= terrain.minY(); --y) {
        if (!cache.budget.spend()) return std::nullopt;
        const auto& cell = cache.at({x, y, z});
        if (!cell) return std::nullopt;
        if (cell->isFull()) {
            const int above = y + 1;
            if (above <= fromY && above < terrain.maxY())
                return ColumnInfo{x, z, above, 0};
            return std::nullopt;
        }
        if (const auto support = cell->lowestUpwardSupport())
            return ColumnInfo{x, z, y, *support};
    }
    return std::nullopt;
}

std::optional<float> sillBetween(const ColumnInfo& a, const ColumnInfo& b,
                                 Face faceA, Face faceB, TerrainCache& cache) {
    const auto& terrain = cache.terrain;
    const int start = std::max(a.baseY, b.baseY);
    const int end = std::min(terrain.maxY(), start + MaxOpeningLookup);
    const double accessible = std::max(a.baseWorld(), b.baseWorld());
    for (int y = start; y < end; ++y) {
        if (!cache.budget.spend()) return std::nullopt;
        const auto& cellA = cache.at({a.x, y, a.z});
        const auto& cellB = cache.at({b.x, y, b.z});
        if (!cellA || !cellB) return std::nullopt;
        const auto openA = cellA->lowestOpening(faceA);
        const auto openB = cellB->lowestOpening(faceB);
        if (!openA || !openB) continue;
        const double world = double(y) + std::max(*openA, *openB);
        if (world + 1e-6 < accessible) continue;
        return float(world);
    }
    return std::nullopt;
}

double capacity(const ColumnInfo& column, double level, TerrainCache& cache) {
    if (!(level > column.baseWorld() + 1e-9)) return 0;
    const auto& terrain = cache.terrain;
    const int last =
        std::min(terrain.maxY() - 1, int(std::floor(level + 1e-9)));
    double total = 0;
    for (int y = column.baseY; y <= last; ++y) {
        if (!cache.budget.spend()) break;
        const auto& cell = cache.at({column.x, y, column.z});
        if (!cell) break;
        const double full = cell->freeVolumeBelow(1.f);
        if (full <= 1e-9) break;  // a closed cap
        const float local = float(std::min(level - double(y), 1.0));
        const float baseLocal = (y == column.baseY) ? column.baseLocal : 0.f;
        if (local > baseLocal)
            total +=
                cell->freeVolumeBelow(local) - cell->freeVolumeBelow(baseLocal);
        if (level <= double(y) + 1) break;
    }
    return std::max(0.0, total);
}

double capacityUnits(const ColumnInfo& column, double level,
                     TerrainCache& cache) {
    return capacity(column, level, cache) * double(UnitsPerCubicMetre);
}

double basinCapacityUnits(std::span<const int> columns,
                          const std::vector<ColumnInfo>& info, double level,
                          TerrainCache& cache) {
    double total = 0;
    for (int index : columns) total += capacityUnits(info[index], level, cache);
    return total;
}

double levelForVolume(std::span<const int> columns,
                      const std::vector<ColumnInfo>& info, double volume,
                      TerrainCache& cache) {
    if (columns.empty()) return 0;
    double low = std::numeric_limits<double>::infinity();
    for (int index : columns) low = std::min(low, info[index].baseWorld());
    if (volume <= VolumeTolerance) return low;

    // The top of the world is a closed edge; any excess is accounted as sink
    // by the caller. The bracket stays bounded even for very large volumes.
    double high = double(cache.terrain.maxY());
    for (int i = 0; i < LevelIterations && !cache.budget.exhausted(); ++i) {
        const double mid = (low + high) * .5;
        if (basinCapacityUnits(columns, info, mid, cache) >= volume)
            high = mid;
        else
            low = mid;
    }
    return high;
}

struct NeighborStep {
    int dx;
    int dz;
    Face face;
    Face opposite;
};

constexpr std::array<NeighborStep, 4> neighborSteps{{
    {1, 0, Face::PosX, Face::NegX},
    {-1, 0, Face::NegX, Face::PosX},
    {0, 1, Face::PosZ, Face::NegZ},
    {0, -1, Face::NegZ, Face::PosZ},
}};

struct Component {
    std::vector<ColumnInfo> info;
    std::vector<Edge> edges;
};

bool discover(const ColumnInfo& seed, TerrainCache& cache, WorkBudget& budget,
              Component& out) {
    std::map<Footprint, int> index;
    std::set<std::pair<int, int>> edgeKeys;
    std::deque<int> queue;

    index.emplace(Footprint{seed.x, seed.z}, 0);
    out.info.push_back(seed);
    queue.push_back(0);

    while (!queue.empty()) {
        const int current = queue.front();
        queue.pop_front();
        const ColumnInfo here = out.info[current];

        for (const auto& step : neighborSteps) {
            if (!budget.spend()) return false;
            const int nx =
                step.dx ? cache.terrain.wrapX(here.x + step.dx) : here.x;
            const int nz = here.z + step.dz;
            if (nx == here.x && nz == here.z) continue;
            if (!cache.terrain.inBounds(nx, nz)) continue;

            const auto found = index.find(Footprint{nx, nz});
            int other = 0;
            ColumnInfo otherInfo{};
            if (found != index.end()) {
                other = found->second;
                otherInfo = out.info[other];
            } else {
                auto base = findBase(nx, nz, cache.terrain.maxY() - 1, cache);
                if (!base) {
                    if (budget.exhausted()) return false;
                    continue;
                }
                other = int(out.info.size());
                otherInfo = *base;
                index.emplace(Footprint{nx, nz}, other);
                out.info.push_back(otherInfo);
                queue.push_back(other);
            }

            const int lo = std::min(current, other);
            const int hi = std::max(current, other);
            if (!edgeKeys.emplace(lo, hi).second) continue;

            const auto sill =
                sillBetween(here, otherInfo, step.face, step.opposite, cache);
            if (budget.exhausted()) return false;
            if (sill) out.edges.push_back(Edge{lo, hi, *sill});
        }
    }
    return !budget.exhausted();
}

struct ColumnAssignment {
    int column = 0;
    double capacity = 0;
    double fraction = 0;
};

bool solveComponent(Component& component, std::vector<double>& volume,
                    TerrainCache& cache, WorkBudget& budget,
                    std::map<int, Volume>& assigned, Volume& overflow) {
    const int count = int(component.info.size());
    if (count == 0) return false;

    std::vector<int> basinOf(count);
    std::map<int, Basin> basins;
    int nextId = 0;
    for (int i = 0; i < count; ++i) {
        const int id = nextId++;
        basinOf[i] = id;
        Basin basin;
        basin.columns.insert(i);
        basin.volume = volume[i];
        const std::array<int, 1> single{i};
        basin.level =
            levelForVolume(single, component.info, basin.volume, cache);
        basins.emplace(id, std::move(basin));
    }

    std::vector<float> deferred;
    int iterations = 0;
    const int maxIterations = count * 8 + 128;

    while (iterations++ < maxIterations && !budget.exhausted()) {
        float minimum = std::numeric_limits<float>::infinity();
        bool found = false;
        for (const auto& edge : component.edges) {
            if (basinOf[edge.a] == basinOf[edge.b]) continue;
            bool skipped = false;
            for (float sill : deferred)
                if (std::abs(sill - edge.sill) <= SillTolerance) {
                    skipped = true;
                    break;
                }
            if (skipped) continue;
            if (!found || edge.sill < minimum) {
                minimum = edge.sill;
                found = true;
            }
        }
        if (!found) break;

        std::vector<const Edge*> groupEdges;
        std::vector<int> groupBasins;
        std::map<int, int> localIndex;
        for (const auto& edge : component.edges) {
            if (basinOf[edge.a] == basinOf[edge.b]) continue;
            if (std::abs(edge.sill - minimum) > SillTolerance) continue;
            groupEdges.push_back(&edge);
            for (int basin : {basinOf[edge.a], basinOf[edge.b]}) {
                if (!localIndex.contains(basin)) {
                    const int next = int(groupBasins.size());
                    localIndex.emplace(basin, next);
                    groupBasins.push_back(basin);
                }
            }
        }
        if (groupEdges.empty()) break;

        // Union basins that meet at this sill.
        std::vector<int> parent(groupBasins.size());
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
        const auto root = [&](int node) {
            while (parent[node] != node) {
                parent[node] = parent[parent[node]];
                node = parent[node];
            }
            return node;
        };
        for (const Edge* edge : groupEdges) {
            const int a = root(localIndex[basinOf[edge->a]]);
            const int b = root(localIndex[basinOf[edge->b]]);
            if (a != b) parent[b] = a;
        }

        std::map<int, std::vector<int>> groups;
        for (size_t i = 0; i < groupBasins.size(); ++i)
            groups[root(int(i))].push_back(groupBasins[i]);

        bool changed = false;
        for (auto& [rootIndex, members] : groups) {
            (void)rootIndex;
            if (members.size() < 2) continue;

            double totalVolume = 0;
            std::vector<int> columns;
            for (int id : members) {
                const auto& basin = basins.at(id);
                totalVolume += basin.volume;
                columns.insert(columns.end(), basin.columns.begin(),
                               basin.columns.end());
            }
            const double totalCapacity =
                basinCapacityUnits(columns, component.info, minimum, cache);
            if (budget.exhausted()) return false;

            if (totalVolume + VolumeTolerance >= totalCapacity) {
                // The shared level is at or above this sill: one basin.
                std::set<int> mergedColumns;
                for (int index : columns) mergedColumns.insert(index);
                Basin mergedBasin;
                mergedBasin.columns = std::move(mergedColumns);
                mergedBasin.volume = totalVolume;
                const std::vector<int> ordered(mergedBasin.columns.begin(),
                                               mergedBasin.columns.end());
                mergedBasin.level =
                    levelForVolume(ordered, component.info, totalVolume, cache);
                if (mergedBasin.level < double(minimum))
                    mergedBasin.level = minimum;
                const int id = nextId++;
                for (int index : mergedBasin.columns) basinOf[index] = id;
                for (int old : members) basins.erase(old);
                basins.emplace(id, std::move(mergedBasin));
                changed = true;
                continue;
            }

            // Not enough volume to join: sources above the sill spill into
            // the basins below it.
            struct Spill {
                int source = 0;
                double excess = 0;
                std::vector<int> targets;
            };
            std::vector<Spill> spills;
            std::map<int, double> capacityAtSill;
            for (int id : members) {
                const auto& basin = basins.at(id);
                std::vector<int> columns(basin.columns.begin(),
                                         basin.columns.end());
                const double capacityHere =
                    basinCapacityUnits(columns, component.info, minimum, cache);
                capacityAtSill[id] = capacityHere;
            }
            if (budget.exhausted()) return false;

            for (int id : members) {
                const auto& basin = basins.at(id);
                const double excess = basin.volume - capacityAtSill[id];
                if (excess <= VolumeTolerance) continue;
                Spill spill;
                spill.source = id;
                spill.excess = excess;

                std::vector<int> sinks;
                std::vector<int> all;
                std::set<int> seen;
                for (const Edge* edge : groupEdges) {
                    int other = -1;
                    if (basinOf[edge->a] == id)
                        other = basinOf[edge->b];
                    else if (basinOf[edge->b] == id)
                        other = basinOf[edge->a];
                    if (other < 0 || other == id || !seen.insert(other).second)
                        continue;
                    all.push_back(other);
                    const auto& target = basins.at(other);
                    std::vector<int> targetColumns(target.columns.begin(),
                                                   target.columns.end());
                    const double targetCapacity = basinCapacityUnits(
                        targetColumns, component.info, minimum, cache);
                    if (target.volume <= targetCapacity + VolumeTolerance)
                        sinks.push_back(other);
                }
                if (!sinks.empty())
                    spill.targets = std::move(sinks);
                else
                    spill.targets = std::move(all);
                if (!spill.targets.empty() && spill.excess > VolumeTolerance)
                    spills.push_back(std::move(spill));
            }
            if (budget.exhausted()) return false;
            if (spills.empty()) continue;

            for (auto& spill : spills) {
                auto& source = basins.at(spill.source);
                source.volume -= spill.excess;
                std::vector<int> columns(source.columns.begin(),
                                         source.columns.end());
                source.level = levelForVolume(columns, component.info,
                                              source.volume, cache);
                const double share = spill.excess / spill.targets.size();
                for (int targetId : spill.targets) {
                    auto& target = basins.at(targetId);
                    target.volume += share;
                    std::vector<int> targetColumns(target.columns.begin(),
                                                   target.columns.end());
                    target.level = levelForVolume(targetColumns, component.info,
                                                  target.volume, cache);
                }
                changed = true;
            }
            if (budget.exhausted()) return false;
        }

        if (!changed) {
            deferred.push_back(minimum);
        } else {
            // A merge can reveal higher sills, and a spill can raise a target
            // above this one. Re-evaluate from the lowest active sill.
            deferred.clear();
        }
    }
    if (budget.exhausted()) return false;

    // Assign integer centimetres with the largest-remainder rule. This keeps
    // the supplied volume exact and gives equal outlets a deterministic
    // tie-break by footprint.
    double inputVolume = 0;
    for (double value : volume) inputVolume += value;
    std::vector<ColumnAssignment> assignments;
    assignments.reserve(size_t(count));
    const Volume targetTotal = Volume(std::llround(inputVolume));
    for (const auto& [id, basin] : basins) {
        (void)id;
        std::vector<int> ordered(basin.columns.begin(), basin.columns.end());
        for (int index : ordered) {
            const double value =
                capacityUnits(component.info[index], basin.level, cache);
            if (budget.exhausted()) return false;
            ColumnAssignment entry;
            entry.column = index;
            entry.capacity = value;
            entry.fraction = value - std::floor(value);
            assignments.push_back(entry);
        }
    }
    if (budget.exhausted()) return false;

    std::sort(assignments.begin(), assignments.end(),
              [](const ColumnAssignment& a, const ColumnAssignment& b) {
                  if (a.fraction != b.fraction) return a.fraction > b.fraction;
                  return a.column < b.column;
              });
    std::vector<Volume> rounded(assignments.size());
    Volume subtotal = 0;
    double capacitySum = 0;
    for (size_t i = 0; i < assignments.size(); ++i) {
        rounded[i] = Volume(std::floor(assignments[i].capacity));
        subtotal += rounded[i];
        capacitySum += assignments[i].capacity;
    }
    Volume target = targetTotal;
    if (double(target) > capacitySum + 1) {
        const Volume excess = Volume(std::floor(double(target) - capacitySum));
        if (excess > 0) {
            overflow += excess;
            target -= excess;
        }
    }
    Volume remaining = target - subtotal;
    for (size_t i = 0; i < assignments.size() && remaining > 0; ++i) {
        rounded[i] += 1;
        --remaining;
    }
    if (remaining < 0) {
        for (size_t i = 0; i < assignments.size() && remaining < 0; ++i) {
            if (rounded[i] > 0) {
                --rounded[i];
                ++remaining;
            }
        }
    }
    if (remaining > 0) {
        // The basin exceeds the closed world top. Account the remainder.
        overflow += remaining;
        remaining = 0;
    }
    for (size_t i = 0; i < assignments.size(); ++i)
        assigned[assignments[i].column] = rounded[i];
    return true;
}

}  // namespace

SnowSettlement::SnowSettlement(const SnowTerrain& terrain)
    : terrain_(terrain) {}

Volume SnowSettlement::storedVolume() const {
    Volume total = 0;
    for (const auto& [key, column] : columns_) {
        (void)key;
        total += column.volume;
    }
    return total;
}

Volume SnowSettlement::pendingVolume() const {
    Volume total = 0;
    for (const auto& [key, deposit] : pending_) {
        (void)key;
        total += deposit.volume;
    }
    return total;
}

Volume SnowSettlement::volumeAt(int x, int z) const {
    const auto found = columns_.find(Footprint{terrain_.wrapX(x), z});
    return found == columns_.end() ? 0 : found->second.volume;
}

std::optional<double> SnowSettlement::levelAt(int x, int z) const {
    const auto found = columns_.find(Footprint{terrain_.wrapX(x), z});
    if (found == columns_.end()) return std::nullopt;
    const auto& column = found->second;
    WorkBudget budget{DefaultWorkBudget};
    TerrainCache cache(terrain_, budget);
    const std::vector<ColumnInfo> info{
        {column.x, column.z, column.baseY, column.baseLocal}};
    const std::array<int, 1> single{0};
    return levelForVolume(single, info, double(column.volume), cache);
}

void SnowSettlement::queue(const PendingDeposit& deposit) {
    const Footprint key{deposit.x, deposit.z};
    const auto found = pending_.find(key);
    if (found == pending_.end()) {
        pending_.emplace(key, deposit);
    } else {
        found->second.volume += deposit.volume;
    }
}

void SnowSettlement::eraseColumn(int x, int z) {
    columns_.erase(Footprint{x, z});
}

SnowSettlement::ApplyResult SnowSettlement::tryApply(
    const PendingDeposit& deposit, size_t budget, size_t* used) {
    WorkBudget work{budget};
    const auto finish = [&](ApplyResult result) {
        if (used) *used = work.used;
        return result;
    };
    TerrainCache cache(terrain_, work);
    const int x = terrain_.wrapX(deposit.x);
    if (!terrain_.inBounds(x, deposit.z)) return finish(ApplyResult::Blocked);

    const auto seed = findBase(x, deposit.z, deposit.fromY, cache);
    if (work.exhausted()) return finish(ApplyResult::Retry);
    if (!seed) return finish(ApplyResult::Blocked);

    Component component;
    if (!discover(*seed, cache, work, component))
        return finish(work.exhausted() ? ApplyResult::Retry
                                       : ApplyResult::Blocked);
    if (component.info.empty()) return finish(ApplyResult::Blocked);

    std::map<Footprint, int> index;
    for (int i = 0; i < int(component.info.size()); ++i)
        index.emplace(Footprint{component.info[i].x, component.info[i].z}, i);

    std::vector<double> volumes(component.info.size(), 0);
    for (int i = 0; i < int(component.info.size()); ++i) {
        const auto& info = component.info[i];
        const auto found = columns_.find(Footprint{info.x, info.z});
        if (found != columns_.end()) volumes[i] = double(found->second.volume);
    }

    const auto seedIndex = index.find(Footprint{seed->x, seed->z});
    if (seedIndex == index.end()) return finish(ApplyResult::Blocked);
    volumes[seedIndex->second] += double(deposit.volume);

    std::map<int, Volume> assigned;
    Volume overflow = 0;
    if (!solveComponent(component, volumes, cache, work, assigned, overflow))
        return finish(ApplyResult::Retry);

    // Commit atomically. Every discovered column is replaced; snow in
    // columns outside the component is untouched.
    std::map<Footprint, SnowColumn> replacements;
    for (int i = 0; i < int(component.info.size()); ++i) {
        const auto found = assigned.find(i);
        const Volume volume = found == assigned.end() ? 0 : found->second;
        if (volume <= 0) continue;
        const auto& info = component.info[i];
        replacements.emplace(
            Footprint{info.x, info.z},
            SnowColumn{info.x, info.z, info.baseY, info.baseLocal, volume});
    }
    for (const auto& info : component.info) eraseColumn(info.x, info.z);
    for (auto& [key, column] : replacements) columns_.emplace(key, column);
    sink_ += overflow;
    return finish(ApplyResult::Applied);
}

DepositOutcome SnowSettlement::deposit(int x, int z, int fromY, Volume volume,
                                       size_t budget) {
    if (volume <= 0) return DepositOutcome::Settled;
    deposited_ += volume;
    PendingDeposit request;
    request.x = terrain_.wrapX(x);
    request.z = z;
    request.fromY = fromY;
    request.volume = volume;

    const ApplyResult result = tryApply(request, budget);
    if (result == ApplyResult::Applied) return DepositOutcome::Settled;
    if (result == ApplyResult::Blocked) {
        sink_ += volume;
        return DepositOutcome::Sink;
    }
    queue(request);
    return DepositOutcome::Pending;
}

size_t SnowSettlement::settle(size_t budget) {
    if (pending_.empty() || budget == 0) return 0;
    size_t applied = 0;
    size_t remaining = budget;
    for (auto it = pending_.begin(); it != pending_.end() && remaining > 0;) {
        size_t used = 0;
        const auto result = tryApply(it->second, remaining, &used);
        remaining -= std::min(remaining, used);
        if (result == ApplyResult::Applied) {
            it = pending_.erase(it);
            ++applied;
        } else if (result == ApplyResult::Blocked) {
            sink_ += it->second.volume;
            it = pending_.erase(it);
        } else {
            // The entry could not finish within the supplied budget. Leave
            // it in transit for a later retry or a save/reload.
            break;
        }
    }
    return applied;
}

Bytes SnowSettlement::encode() const {
    BinaryWriter writer;
    writer.writeTag("SNOW");
    writer.write<uint32_t>(1);
    writer.write<uint64_t>(deposited_);
    writer.write<uint64_t>(sink_);
    writer.write<uint32_t>(uint32_t(columns_.size()));
    for (const auto& [key, column] : columns_) {
        (void)key;
        writer.write<int32_t>(column.x);
        writer.write<int32_t>(column.z);
        writer.write<int32_t>(column.baseY);
        writer.write<float>(column.baseLocal);
        writer.write<int64_t>(column.volume);
    }
    writer.write<uint32_t>(uint32_t(pending_.size()));
    for (const auto& [key, deposit] : pending_) {
        (void)key;
        writer.write<int32_t>(deposit.x);
        writer.write<int32_t>(deposit.z);
        writer.write<int32_t>(deposit.fromY);
        writer.write<int64_t>(deposit.volume);
    }
    return writer.take();
}

bool SnowSettlement::restore(ByteView bytes) {
    try {
        BinaryReader reader(bytes);
        if (reader.readTag() != "SNOW") return false;
        if (reader.read<uint32_t>() != 1) return false;
        const auto deposited = reader.read<uint64_t>();
        const auto sink = reader.read<uint64_t>();
        const auto columnCount = reader.read<uint32_t>();
        if (columnCount > MaxSerializedColumns) return false;

        std::map<Footprint, SnowColumn> columns;
        for (uint32_t i = 0; i < columnCount; ++i) {
            SnowColumn column;
            column.x = reader.read<int32_t>();
            column.z = reader.read<int32_t>();
            column.baseY = reader.read<int32_t>();
            column.baseLocal = reader.read<float>();
            column.volume = reader.read<int64_t>();
            if (column.volume <= 0 ||
                !(column.baseLocal >= 0 && column.baseLocal <= 1))
                return false;
            if (!columns.emplace(Footprint{column.x, column.z}, column).second)
                return false;
        }

        const auto pendingCount = reader.read<uint32_t>();
        if (pendingCount > MaxSerializedPending) return false;
        std::map<Footprint, PendingDeposit> pending;
        for (uint32_t i = 0; i < pendingCount; ++i) {
            PendingDeposit deposit;
            deposit.x = reader.read<int32_t>();
            deposit.z = reader.read<int32_t>();
            deposit.fromY = reader.read<int32_t>();
            deposit.volume = reader.read<int64_t>();
            if (deposit.volume <= 0) return false;
            if (!pending.emplace(Footprint{deposit.x, deposit.z}, deposit)
                     .second)
                return false;
        }
        if (!reader.atEnd()) return false;

        columns_ = std::move(columns);
        pending_ = std::move(pending);
        deposited_ = Volume(deposited);
        sink_ = Volume(sink);
        return true;
    } catch (const SerializationError&) {
        return false;
    }
}

}  // namespace snow
