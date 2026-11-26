#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "consts.hpp"
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
inline constexpr int MaximumDepth = 16;
inline constexpr Volume MaximumVolume = MaximumDepth * UnitsPerCubicMetre;
static_assert(WORLD_HEIGHT_Y * UnitsPerCubicMetre <= INT32_MAX);
// Heights in millionths of a metre, equal to cm³ over a full footprint.
// All legal overworld heights fit in 32 bits.
struct PackRun {
    int32_t bottom = 0;
    int32_t top = 0;
    bool operator==(const PackRun&) const = default;
    Volume volume() const { return Volume(top) - bottom; }
};
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

// Sparse exterior overrides. Zero means a completely excavated or melted
// pack. Unvisited columns derive their stock from the world generator.
using Footprints = std::set<std::pair<int, int>>;
using PackTops = std::map<std::pair<int, int>, int32_t>;
inline constexpr Volume PackProjectionStep = 50'000;  // 5 cm
inline int32_t projectedPackTop(PackRun run) {
    // Keep a nonempty run visible and never extend it across a tile boundary.
    return int32_t(std::max(Volume(run.bottom) + 1,
                            run.top / PackProjectionStep * PackProjectionStep));
}

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
    virtual std::optional<PackRun> initialPack(int, int) const {
        return std::nullopt;
    }
    virtual Volume initialPackCapacity(int, int) const { return MaximumVolume; }
    virtual bool mayHaveInitialPack(int, int) const { return false; }
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
    // The undisturbed interval is reproducible. A footprint is materialized
    // only when its pack changes. Its ceiling is anchored at original ground.
    static constexpr tile_geometry::PieceId PackPieceBase = 0x50000000u;
    std::span<const PackRun> packRuns(int x, int z, PackRun& baseline) const;
    std::optional<PackRun> surfacePack(int x, int z) const;
    tile_geometry::CellGeometry packGeometry(
        TilePos pos, const tile_geometry::CellGeometry& substrate,
        uint8_t snowBlock) const;
    bool excavatePack(TilePos pos, tile_geometry::PieceId id);
    bool excavateLoose(TilePos pos, tile_geometry::PieceId id);
    static constexpr tile_geometry::PieceId LoosePieceBase = 0x40000000u;
    static tile_geometry::PieceId loosePieceId(int baseY, uint16_t cell);
    Volume packHeadroom(int x, int z, PackRun surface) const;
    void applyPackWeather(int x, int z, PackRun surface, Volume melted,
                          Volume fresh, Volume rejected);
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

    // Ledger for loose snow, meltwater and overflow. Implicit pack stock
    // is measured by summing packRuns over the footprint of interest.
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
    ChunkColumns takePackChanges();
    Footprints takeFootprintChanges();
    bool initialPacksEnabled() const { return initialPacksEnabled_; }
    // A v5 ledger can contain old automatic ground deposits. They predate the
    // dated seasonal model and are rebased once on load; roofs, tunnels and
    // cuts keep their records.
    bool needsGroundRebase() const {
        return loadedVersion_ == 5 && localWeather_.empty() &&
               initialPacksEnabled_;
    }
    size_t dropGroundLoose(
        const std::function<bool(int x, int z, int baseY)>& ground);
    // Drops matching loose columns of one chunk column (resident rebase).
    size_t dropLoose(ChunkColumn column,
                     const std::function<bool(int x, int z, int baseY)>& match);
    // Ground support cell of a footprint that can hold an established pack.
    std::optional<int> groundBase(int x, int z) const;
    // Pinned pack tops (only footprints with an edit) and the tops of loose
    // snow resting on the generated ground, for the distant height field.
    void updatePackTops(PackTops& pack, PackTops& loose,
                        ChunkColumn column) const;
    size_t pendingCount() const { return pending_.size(); }

    // Weather cursors and deposits commit in the same snow.dat snapshot.
    // Missing cursors use the world's weather epoch, including first visits.
    void beginWeather(double day);
    void migrateUnvisitedWeather(double savedDay);
    void resetWeather(double day);
    double weatherDay(int x, int z) const;
    double weatherHorizon() const { return weatherHorizon_; }
    int64_t firstCurrentWeatherInterval() const {
        return firstCurrentWeatherInterval_;
    }
    int modelForWeatherInterval(int64_t interval) const;
    void checkpointWeather(double day);
    void markWeatherApplied(int x, int z, double day);
    void trackLocalWeather(int x, int z, double day);
    bool tracksLocalWeather(int x, int z) const;
    bool hasLocalWeather() const { return !localWeather_.empty(); }
    void setWeatherForce(double day, uint8_t force);
    const auto& weatherForces() const { return weatherForces_; }
    std::optional<SnowColumn> weatherSupport(int x, int z, int fromY) const;
    // Snow that fell and melted within one catch-up batch never needs a mesh.
    void recordWeatherMelt(SnowKey key, Volume amount);
    void recordWeatherOverflow(Volume amount);
    Volume weatherCapacity(const SnowColumn& support) const;

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
    struct PackEdit {
        int32_t ceiling;
        std::vector<PackRun> runs;
    };
    PackEdit& editPack(int x, int z);

    const SnowTerrain& terrain_;
    std::map<SnowKey, SnowColumn> columns_;
    std::map<SnowKey, PendingDeposit> pending_;
    std::map<SnowKey, Volume> meltwater_;
    std::map<Footprint, PackEdit> packEdits_;
    bool initialPacksEnabled_ = true;
    Volume deposited_ = 0;
    Volume sink_ = 0;
    Volume melted_ = 0;
    uint64_t revision_ = 0;
    ChunkColumns projectionChanges_;
    ChunkColumns packChanges_;
    Footprints footprintChanges_;
    double weatherEpoch_ = -1;
    double weatherHorizon_ = 0;
    std::optional<double> unvisitedWeatherDay_;
    int64_t firstCurrentWeatherInterval_ = 0;
    std::map<Footprint, double> weatherApplied_;
    int loadedVersion_ = 0;
    Footprints localWeather_;
    std::map<double, uint8_t> weatherForces_;
};

}  // namespace snow
