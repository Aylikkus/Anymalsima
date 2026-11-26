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

#include "chunk.hpp"
#include "weather_history.hpp"

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
constexpr size_t MaxWeatherRecords = 1 << 26;
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
    const int top = std::min(fromY, terrain.top(x, z));
    for (int y = top; y >= terrain.minY(); --y) {
        if (!cache.budget.spend()) return std::nullopt;
        const auto& cell = cache.at({x, y, z});
        if (!cell) return std::nullopt;
        if (cell->empty()) continue;
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
        const float local = float(std::min(level - double(y), 1.0));
        const float baseLocal = (y == column.baseY) ? column.baseLocal : 0.f;
        if (cell->empty())
            total +=
                std::max(0.0, std::min(level - double(y), 1.0) - baseLocal);
        else if (cell->isFull())
            break;
        else if (local > baseLocal)
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
                      TerrainCache& cache, double heightTolerance = 0) {
    if (columns.empty()) return 0;
    double low = std::numeric_limits<double>::infinity();
    for (int index : columns) low = std::min(low, info[index].baseWorld());
    if (volume <= VolumeTolerance) return low;

    // The top of the world is a closed edge; any excess is accounted as sink
    // by the caller. The bracket stays bounded even for very large volumes.
    const double ceiling = double(cache.terrain.maxY());
    const double flat = low + volume / (UnitsPerCubicMetre * columns.size());
    if (flat <= ceiling &&
        std::abs(basinCapacityUnits(columns, info, flat, cache) - volume) <=
            VolumeTolerance)
        return flat;
    double high = std::min(ceiling, low + 1);
    while (high < ceiling && !cache.budget.exhausted() &&
           basinCapacityUnits(columns, info, high, cache) < volume) {
        high = std::min(ceiling, low + (high - low) * 2);
    }
    for (int i = 0; i < LevelIterations && high - low > heightTolerance &&
                    !cache.budget.exhausted();
         ++i) {
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
              Component& out, int radius) {
    std::map<Footprint, int> index;
    std::set<std::pair<int, int>> edgeKeys;
    std::deque<std::pair<int, int>> queue;

    index.emplace(Footprint{seed.x, seed.z}, 0);
    out.info.push_back(seed);
    queue.emplace_back(0, 0);

    while (!queue.empty()) {
        const auto [current, distance] = queue.front();
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
                if (radius && distance == radius) continue;
                auto base = findBase(nx, nz, cache.terrain.maxY(), cache);
                if (!base) {
                    if (budget.exhausted()) return false;
                    continue;
                }
                other = int(out.info.size());
                otherInfo = *base;
                index.emplace(Footprint{nx, nz}, other);
                out.info.push_back(otherInfo);
                queue.emplace_back(other, distance + 1);
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
    const auto* column = columnAt(x, z, terrain_.maxY());
    return column ? column->volume : 0;
}

std::optional<double> SnowSettlement::levelAt(int x, int z) const {
    const auto* column = columnAt(x, z, terrain_.maxY());
    return column ? level(*column) : std::nullopt;
}

const SnowColumn* SnowSettlement::columnAt(int x, int z, int y) const {
    x = terrain_.wrapX(x);
    auto found = columns_.upper_bound({x, z, y});
    if (found == columns_.begin()) return nullptr;
    --found;
    const auto& column = found->second;
    return column.x == x && column.z == z ? &column : nullptr;
}

std::optional<double> SnowSettlement::level(const SnowColumn& column) const {
    WorkBudget budget{DefaultWorkBudget};
    TerrainCache cache(terrain_, budget);
    const auto base = findBase(column.x, column.z, column.baseY, cache);
    if (!base || base->baseY != column.baseY ||
        base->baseLocal != column.baseLocal)
        return std::nullopt;
    const std::vector<ColumnInfo> info{
        {column.x, column.z, column.baseY, column.baseLocal}};
    const std::array<int, 1> single{0};
    // Projection and inspection only need the precision used by the
    // clipping planes. Settlement keeps its full precision for the ledger.
    constexpr double ProjectionTolerance = 1e-6;
    return levelForVolume(single, info, double(column.volume), cache,
                          ProjectionTolerance);
}

void SnowSettlement::clear() {
    for (const auto& [key, column] : columns_) {
        (void)key;
        invalidateProjection(column.x, column.z);
    }
    columns_.clear();
    pending_.clear();
    meltwater_.clear();
    deposited_ = 0;
    sink_ = 0;
    melted_ = 0;
    ++revision_;
}

bool SnowSettlement::hasSnowInChunkColumn(ChunkColumn column) const {
    if (columns_.empty()) return false;
    const int x0 = int(column.first) * Chunk::SIZE_X;
    const int z0 = int(column.second) * Chunk::SIZE_Z;
    for (int x = x0; x < x0 + Chunk::SIZE_X; ++x) {
        const auto found = columns_.lower_bound({x, z0, terrain_.minY()});
        if (found != columns_.end() && found->second.x == x &&
            found->second.z < z0 + Chunk::SIZE_Z)
            return true;
    }
    return false;
}

ChunkColumns SnowSettlement::takeProjectionChanges() {
    return std::exchange(projectionChanges_, {});
}

void SnowSettlement::invalidateProjection(int x, int z) {
    projectionChanges_.emplace(unsigned(x / Chunk::SIZE_X),
                               unsigned(z / Chunk::SIZE_Z));
}

void SnowSettlement::queue(const PendingDeposit& deposit) {
    const SnowKey key{deposit.x, deposit.z, deposit.fromY};
    auto [found, inserted] = pending_.try_emplace(key, deposit);
    if (!inserted) found->second.volume += deposit.volume;
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
    if (!seed)
        return finish(deposit.radius ? ApplyResult::Retry
                                     : ApplyResult::Blocked);

    Component component;
    if (!discover(*seed, cache, work, component, deposit.radius))
        return finish(work.exhausted() ? ApplyResult::Retry
                                       : ApplyResult::Blocked);
    if (component.info.empty()) return finish(ApplyResult::Blocked);

    std::map<Footprint, int> index;
    for (int i = 0; i < int(component.info.size()); ++i)
        index.emplace(Footprint{component.info[i].x, component.info[i].z}, i);

    std::vector<double> volumes(component.info.size(), 0);
    for (int i = 0; i < int(component.info.size()); ++i) {
        const auto& info = component.info[i];
        const auto found = columns_.find({info.x, info.z, info.baseY});
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
    for (int i = 0; i < int(component.info.size()); ++i) {
        const auto found = assigned.find(i);
        const Volume volume = found == assigned.end() ? 0 : found->second;
        const auto& info = component.info[i];
        const SnowKey key{info.x, info.z, info.baseY};
        const auto existing = columns_.find(key);
        if (volume > 0) {
            if (existing != columns_.end() &&
                existing->second.volume == volume &&
                existing->second.baseLocal == info.baseLocal)
                continue;
            columns_.insert_or_assign(
                key,
                SnowColumn{info.x, info.z, info.baseY, info.baseLocal, volume});
        } else if (existing != columns_.end()) {
            columns_.erase(existing);
        } else {
            continue;
        }
        invalidateProjection(info.x, info.z);
    }
    sink_ += overflow;
    ++revision_;
    return finish(ApplyResult::Applied);
}

DepositOutcome SnowSettlement::deposit(int x, int z, int fromY, Volume volume,
                                       size_t budget) {
    return submit({terrain_.wrapX(x), z, fromY, volume}, budget);
}

DepositOutcome SnowSettlement::snowfall(int x, int z, int fromY,
                                        Volume volume) {
    if (fromY == terrain_.maxY()) {
        const auto top = terrain_.cell({x, fromY - 1, z});
        if (volume > 0 && top && top->isFull()) {
            deposited_ += volume;
            sink_ += volume;
            ++revision_;
            return DepositOutcome::Sink;
        }
    }
    return submit({terrain_.wrapX(x), z, fromY, volume, 2}, DefaultWorkBudget);
}

void SnowSettlement::supportChanged(TilePos pos) {
    pos.x = terrain_.wrapX(pos.x);
    for (const int y : {pos.y, pos.y + 1}) {
        const auto found = columns_.find({pos.x, pos.z, y});
        if (found == columns_.end()) continue;
        const auto column = found->second;
        WorkBudget work{DefaultWorkBudget};
        TerrainCache cache(terrain_, work);
        const auto base = findBase(pos.x, pos.z, y, cache);
        if (base && base->baseY == y && base->baseLocal == column.baseLocal)
            continue;

        columns_.erase(found);
        ++revision_;
        invalidateProjection(pos.x, pos.z);
        const PendingDeposit deposit{pos.x, pos.z, y, column.volume, 2};
        if (tryApply(deposit, DefaultWorkBudget) != ApplyResult::Applied)
            queue(deposit);
    }
}

Volume SnowSettlement::melt(SnowKey key, Volume volume) {
    const auto found = columns_.find(key);
    if (found == columns_.end() || volume <= 0) return 0;
    const Volume removed = std::min(found->second.volume, volume);
    found->second.volume -= removed;
    if (found->second.volume == 0) columns_.erase(found);
    meltwater_[key] += removed;
    melted_ += removed;
    ++revision_;
    invalidateProjection(std::get<0>(key), std::get<1>(key));
    return removed;
}

DepositOutcome SnowSettlement::submit(PendingDeposit request, size_t budget) {
    if (request.volume <= 0) return DepositOutcome::Settled;
    const Volume volume = request.volume;
    deposited_ += volume;
    ++revision_;
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
        remaining -= std::min(remaining, std::max(size_t(1), used));
        if (result == ApplyResult::Applied) {
            it = pending_.erase(it);
            ++applied;
        } else if (result == ApplyResult::Blocked) {
            sink_ += it->second.volume;
            it = pending_.erase(it);
            ++revision_;
        } else {
            // The entry could not finish within the supplied budget. Leave
            // it in transit for a later retry or a save/reload.
            ++it;
        }
    }
    return applied;
}

void SnowSettlement::beginWeather(double day) {
    if (weatherEpoch_ < 0) resetWeather(day);
}

void SnowSettlement::resetWeather(double day) {
    weatherEpoch_ = weatherHorizon_ = day;
    weatherApplied_.clear();
    weatherForces_.clear();
    ++revision_;
}

double SnowSettlement::weatherDay(int x, int z) const {
    const auto found = weatherApplied_.find({terrain_.wrapX(x), z});
    return found == weatherApplied_.end() ? weatherEpoch_ : found->second;
}

void SnowSettlement::checkpointWeather(double day) {
    if (day <= weatherHorizon_) return;
    weatherHorizon_ = day;
    ++revision_;
}

void SnowSettlement::markWeatherApplied(int x, int z, double day) {
    weatherApplied_.insert_or_assign({terrain_.wrapX(x), z}, day);
    weatherHorizon_ = std::max(weatherHorizon_, day);
    ++revision_;
}

void SnowSettlement::setWeatherForce(double day, uint8_t force) {
    const auto after = weatherForces_.upper_bound(day);
    if ((after == weatherForces_.begin() ? 0 : std::prev(after)->second) ==
        force)
        return;
    // Several UI clicks at the same paused date collapse to one transition.
    auto at = weatherForces_.lower_bound(day);
    const auto prior = at == weatherForces_.begin() ? 0 : std::prev(at)->second;
    if (force == prior) {
        if (at != after) weatherForces_.erase(at);
    } else {
        weatherForces_.insert_or_assign(day, force);
    }
    weatherHorizon_ = std::max(weatherHorizon_, day);
    ++revision_;
}

std::optional<SnowColumn> SnowSettlement::weatherSupport(int x, int z,
                                                         int fromY) const {
    WorkBudget work{DefaultWorkBudget};
    TerrainCache cache(terrain_, work);
    const auto base = findBase(x, z, fromY, cache);
    if (!base) return std::nullopt;
    return SnowColumn{x, z, base->baseY, base->baseLocal, 0};
}

void SnowSettlement::recordWeatherMelt(SnowKey key, Volume amount) {
    if (amount <= 0) return;
    deposited_ += amount;
    melted_ += amount;
    meltwater_[key] += amount;
    ++revision_;
}

Bytes SnowSettlement::encode() const {
    BinaryWriter writer;
    writer.writeTag("SNOW");
    writer.write<uint32_t>(3);
    writer.write<uint64_t>(deposited_);
    writer.write<uint64_t>(sink_);
    writer.write<int64_t>(melted_);
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
        writer.write<int32_t>(deposit.radius);
    }
    writer.write<uint32_t>(uint32_t(meltwater_.size()));
    for (const auto& [key, volume] : meltwater_) {
        const auto [x, z, y] = key;
        writer.write<int32_t>(x);
        writer.write<int32_t>(z);
        writer.write<int32_t>(y);
        writer.write<int64_t>(volume);
    }
    writer.write<uint32_t>(weather_history::SimulationVersion);
    writer.write<uint32_t>(world_weather::ModelVersion);
    writer.write<double>(weatherEpoch_);
    writer.write<double>(weatherHorizon_);
    writer.write<uint32_t>(uint32_t(weatherApplied_.size()));
    for (const auto& [footprint, day] : weatherApplied_) {
        writer.write<int32_t>(footprint.first);
        writer.write<int32_t>(footprint.second);
        writer.write<double>(day);
    }
    writer.write<uint32_t>(uint32_t(weatherForces_.size()));
    for (const auto& [day, force] : weatherForces_) {
        writer.write<double>(day);
        writer.write<uint8_t>(force);
    }
    return writer.take();
}

bool SnowSettlement::restore(ByteView bytes) {
    try {
        BinaryReader reader(bytes);
        if (reader.readTag() != "SNOW") return false;
        const auto version = reader.read<uint32_t>();
        if (version < 1 || version > 3) return false;
        const auto deposited = reader.read<uint64_t>();
        const auto sink = reader.read<uint64_t>();
        const Volume melted = version >= 2 ? reader.read<int64_t>() : 0;
        if (deposited > uint64_t(std::numeric_limits<Volume>::max()) ||
            sink > deposited || melted < 0 ||
            uint64_t(melted) > deposited - sink)
            return false;
        auto remaining = Volume(deposited - sink) - melted;
        const auto columnCount = reader.read<uint32_t>();
        if (columnCount > MaxSerializedColumns) return false;

        std::map<SnowKey, SnowColumn> columns;
        for (uint32_t i = 0; i < columnCount; ++i) {
            SnowColumn column;
            column.x = reader.read<int32_t>();
            column.z = reader.read<int32_t>();
            column.baseY = reader.read<int32_t>();
            column.baseLocal = reader.read<float>();
            column.volume = reader.read<int64_t>();
            if (column.volume <= 0 || column.volume > remaining ||
                column.baseY < terrain_.minY() ||
                column.baseY >= terrain_.maxY() ||
                !terrain_.inBounds(column.x, column.z) ||
                column.x != terrain_.wrapX(column.x) ||
                !(column.baseLocal >= 0 && column.baseLocal <= 1))
                return false;
            remaining -= column.volume;
            if (!columns
                     .emplace(SnowKey{column.x, column.z, column.baseY}, column)
                     .second)
                return false;
        }

        const auto pendingCount = reader.read<uint32_t>();
        if (pendingCount > MaxSerializedPending) return false;
        std::map<SnowKey, PendingDeposit> pending;
        for (uint32_t i = 0; i < pendingCount; ++i) {
            PendingDeposit deposit;
            deposit.x = reader.read<int32_t>();
            deposit.z = reader.read<int32_t>();
            deposit.fromY = reader.read<int32_t>();
            deposit.volume = reader.read<int64_t>();
            if (version >= 2) deposit.radius = reader.read<int32_t>();
            if (deposit.volume <= 0 || deposit.volume > remaining ||
                (deposit.radius != 0 && deposit.radius != 2) ||
                !terrain_.inBounds(deposit.x, deposit.z) ||
                deposit.x != terrain_.wrapX(deposit.x))
                return false;
            remaining -= deposit.volume;
            if (!pending
                     .emplace(SnowKey{deposit.x, deposit.z, deposit.fromY},
                              deposit)
                     .second)
                return false;
        }
        if (remaining != 0) return false;

        std::map<SnowKey, Volume> meltwater;
        auto remainingMelt = melted;
        if (version >= 2) {
            const auto count = reader.read<uint32_t>();
            if (count > MaxSerializedColumns) return false;
            for (uint32_t i = 0; i < count; ++i) {
                const int x = reader.read<int32_t>();
                const int z = reader.read<int32_t>();
                const int y = reader.read<int32_t>();
                const auto volume = reader.read<int64_t>();
                if (volume <= 0 || volume > remainingMelt ||
                    !terrain_.inBounds(x, z) || x != terrain_.wrapX(x) ||
                    y < terrain_.minY() || y >= terrain_.maxY() ||
                    !meltwater.emplace(SnowKey{x, z, y}, volume).second)
                    return false;
                remainingMelt -= volume;
            }
        }
        double epoch = -1, horizon = 0;
        std::map<Footprint, double> applied;
        std::map<double, uint8_t> forces;
        if (version >= 3) {
            if (reader.read<uint32_t>() != weather_history::SimulationVersion ||
                reader.read<uint32_t>() != world_weather::ModelVersion)
                return false;
            epoch = reader.read<double>();
            horizon = reader.read<double>();
            if (!std::isfinite(epoch) || !std::isfinite(horizon) ||
                (epoch < 0 && (epoch != -1 || horizon != 0)) ||
                horizon < epoch || horizon < 0 ||
                horizon >= double(std::numeric_limits<int64_t>::max() /
                                  weather_history::IntervalsPerDay))
                return false;
            const auto validDay = [&](double day) {
                return epoch >= 0 && day >= epoch && day <= horizon;
            };
            const auto count = reader.read<uint32_t>();
            if (count > MaxWeatherRecords) return false;
            for (uint32_t i = 0; i < count; ++i) {
                const int x = reader.read<int32_t>(),
                          z = reader.read<int32_t>();
                const auto day = reader.read<double>();
                if (!validDay(day) || !terrain_.inBounds(x, z) ||
                    x != terrain_.wrapX(x) ||
                    !applied.emplace(Footprint{x, z}, day).second)
                    return false;
            }
            const auto forceCount = reader.read<uint32_t>();
            if (forceCount > MaxWeatherRecords) return false;
            for (uint32_t i = 0; i < forceCount; ++i) {
                const auto day = reader.read<double>();
                const auto force = reader.read<uint8_t>();
                if (!validDay(day) || force > 4 ||
                    !forces.emplace(day, force).second)
                    return false;
            }
        }
        if (!reader.atEnd() || remainingMelt != 0) return false;

        for (const auto& [key, column] : columns_) {
            (void)key;
            invalidateProjection(column.x, column.z);
        }
        columns_ = std::move(columns);
        for (const auto& [key, column] : columns_) {
            (void)key;
            invalidateProjection(column.x, column.z);
        }
        pending_ = std::move(pending);
        meltwater_ = std::move(meltwater);
        deposited_ = Volume(deposited);
        sink_ = Volume(sink);
        melted_ = melted;
        weatherEpoch_ = epoch;
        weatherHorizon_ = horizon;
        weatherApplied_ = std::move(applied);
        weatherForces_ = std::move(forces);
        ++revision_;
        return true;
    } catch (const SerializationError&) {
        return false;
    }
}

}  // namespace snow
