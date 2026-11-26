#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>

#include "serialization/binary_stream.hpp"
#include "tile_piece.hpp"
#include "tile_pos.hpp"

// Gravity settlement for deposited snow. Volumes are tracked per one-square-
// metre footprint column and fill the surface-connected free space above the
// column's support. A column that reaches a boundary opening spills into the
// connected receiver, so a ramp toe fills as a clipped wedge and a ledge with
// an open drop delivers its snow to the support below. Free volume comes from
// the actual convex facets, so compound pieces clip the deposit exactly.
//
// V1 resolves communicating columns at a shared horizontal level. A steep
// (45 degrees or more) face drains to the lowest opening because its free
// space is shallow; gentler faces build cell-sized pockets that spill at the
// next sill. Enclosed cavities under an overhang are outside the surface-
// connected model until the exposure work in step 10 and room storage in
// step 16. The X axis follows the world wrap; Z edges are closed and overflow
// above the world top is recorded as an accounted sink. Bulk snow volume is
// stored in cubic centimetres.
namespace snow {

// One cubic centimetre of settled snow. Exact volumes avoid cumulative drift
// between region and world saves.
using Volume = int64_t;
inline constexpr Volume UnitsPerCubicMetre = 1'000'000;
// Settled V1 snow has a fixed density of 200 kg/m³.
inline constexpr double SnowPerWater = 5;

struct SnowColumn {
    int x = 0;
    int z = 0;
    int baseY = 0;
    float baseLocal = 0;
    Volume volume = 0;
};

using SnowKey = std::tuple<int, int, int>;  // canonical x, z, support cell y
using ChunkColumn = std::pair<unsigned, unsigned>;
using ChunkColumns = std::set<ChunkColumn>;

// Terrain queries used by settlement. Cells are one-metre cubes and positions
// are canonical in X. Returning nullopt closes that side of the world.
class SnowTerrain {
   public:
    virtual ~SnowTerrain() = default;
    virtual std::optional<tile_geometry::CellGeometry> cell(TilePos) const = 0;
    virtual int wrapX(int x) const { return x; }
    virtual bool inBounds(int x, int z) const = 0;
    virtual int minY() const = 0;
    virtual int maxY() const = 0;
    // Conservative highest occupied cell, inclusive. Overhead air can be
    // skipped; a missing cell below this bound is still unknown.
    virtual int top(int, int) const { return maxY() - 1; }
};

enum class DepositOutcome {
    Settled,
    Pending,
    Sink,
    Blocked,
};

class SnowSettlement {
   public:
    static constexpr size_t DefaultWorkBudget = 1 << 16;

    explicit SnowSettlement(const SnowTerrain& terrain);

    // Adds volume at a footprint, starting the downward search at fromY.
    // An over-budget solve leaves the volume in the pending queue, where it
    // still counts against the supplied volume until settle() applies it.
    DepositOutcome deposit(int x, int z, int fromY, Volume volume,
                           size_t budget = DefaultWorkBudget);
    // Restrict active-weather settlement to a local neighbourhood. Debug
    // deposits retain the ordinary connected-component search.
    DepositOutcome snowfall(int x, int z, int fromY, Volume volume);
    Volume melt(SnowKey key, Volume volume);
    void supportChanged(TilePos pos);
    // Removes every deposit and pending transfer. Debug and fixture use;
    // regular weather removal goes through melt and excavation steps.
    void clear();

    // Retries pending deposits in deterministic footprint order. Returns the
    // number applied. A budget of zero only reports the queue.
    size_t settle(size_t budget = DefaultWorkBudget);

    // Volume resting in the column at a footprint. Zero when absent.
    Volume volumeAt(int x, int z) const;
    // Snow surface in world metres, or nullopt without a known column.
    std::optional<double> levelAt(int x, int z) const;
    std::optional<double> level(const SnowColumn& column) const;
    const SnowColumn* columnAt(int x, int z, int y) const;

    const std::map<SnowKey, SnowColumn>& columns() const { return columns_; }

    Volume storedVolume() const;
    Volume pendingVolume() const;
    Volume sinkVolume() const { return sink_; }
    Volume depositedVolume() const { return deposited_; }
    double meltwaterCm3() const { return double(melted_) / SnowPerWater; }
    // Awaiting routing at the former snow support. Every unit is 0.2 cm³
    // liquid water, so conversion never rounds away a small melt.
    const std::map<SnowKey, Volume>& meltwater() const { return meltwater_; }
    uint64_t revision() const { return revision_; }
    bool hasSnowInChunkColumn(ChunkColumn column) const;
    ChunkColumns takeProjectionChanges();
    size_t pendingCount() const { return pending_.size(); }

    // Weather cursors and deposits commit in the same snow.dat snapshot.
    // Missing cursors use the world's weather epoch, including first visits.
    void beginWeather(double day);
    void resetWeather(double day);
    double weatherDay(int x, int z) const;
    double weatherHorizon() const { return weatherHorizon_; }
    void checkpointWeather(double day);
    void markWeatherApplied(int x, int z, double day);
    void setWeatherForce(double day, uint8_t force);
    const auto& weatherForces() const { return weatherForces_; }
    std::optional<SnowColumn> weatherSupport(int x, int z, int fromY) const;
    // Snow that fell and melted within one catch-up batch never needs a mesh.
    void recordWeatherMelt(SnowKey key, Volume amount);

    // State includes resting columns, pending transfers, accepted input and
    // accounted sink volume. The version is independent of chunk saves.
    Bytes encode() const;
    bool restore(ByteView bytes);

   private:
    struct PendingDeposit {
        int x = 0;
        int z = 0;
        int fromY = 0;
        Volume volume = 0;
        int radius = 0;
    };
    using Footprint = std::pair<int, int>;

    enum class ApplyResult { Applied, Retry, Blocked };

    ApplyResult tryApply(const PendingDeposit& deposit, size_t budget,
                         size_t* used = nullptr);
    DepositOutcome submit(PendingDeposit request, size_t budget);
    void queue(const PendingDeposit& deposit);
    void invalidateProjection(int x, int z);

    const SnowTerrain& terrain_;
    std::map<SnowKey, SnowColumn> columns_;
    std::map<SnowKey, PendingDeposit> pending_;
    std::map<SnowKey, Volume> meltwater_;
    Volume deposited_ = 0;
    Volume sink_ = 0;
    Volume melted_ = 0;
    uint64_t revision_ = 0;
    ChunkColumns projectionChanges_;
    double weatherEpoch_ = -1;
    double weatherHorizon_ = 0;
    std::map<Footprint, double> weatherApplied_;
    std::map<double, uint8_t> weatherForces_;
};

}  // namespace snow
