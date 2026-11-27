#pragma once

#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/signal.hpp"
#include "fluid_cell.hpp"
#include "local_water_cells.hpp"
#include "serialization/binary_stream.hpp"
#include "tile_pos.hpp"
#include "water_surface.hpp"

class GeneratedHydrology;

using WaterVolume = int64_t;
inline constexpr WaterVolume WaterUnitsPerBlock = 255;
// V1 keeps phase volume as water equivalent. Ice only needs the expansion
// factor when step 19 turns a frozen equivalent into a visible thickness.
inline constexpr double IceSpecificGravity = 0.917;
inline double iceThicknessMetres(WaterVolume frozenWater) {
    return double(frozenWater) / WaterUnitsPerBlock / IceSpecificGravity;
}
inline WaterVolume frozenWaterForIceMetres(double metres) {
    return WaterVolume(
        std::llround(metres * IceSpecificGravity * WaterUnitsPerBlock));
}

class StorageCurve {
   public:
    using Levels = std::array<WaterVolume, WORLD_HEIGHT_Y + 1>;

    explicit StorageCurve(Levels storage);
    static StorageCurve constantArea(WaterVolume area, int floor, int top);
    WaterVolume capacity() const { return storage.back(); }
    WaterVolume volumeAt(int level) const { return storage.at(level); }
    int levelAt(WaterVolume volume) const;
    uint8_t fillAtSurface(WaterVolume volume) const;

   private:
    Levels storage;
};

// Deterministic portal ordering for saves and iteration.
struct TilePosLess {
    bool operator()(const TilePos& a, const TilePos& b) const {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

// A gameplay-created standing body: a dammed reach's upstream pool, or an
// enclosed pocket after a split. The footprint certifies which columns the
// level covers; the level moves with the pool's storage curve.
struct PoolFootprint {
    // Floor and connection sill; -1 uses the supplied ground for legacy
    // fixed-footprint pools.
    std::unordered_map<uint64_t, std::pair<int, int>> columns;
    int minX = INT_MAX, minZ = INT_MAX, maxX = INT_MIN, maxZ = INT_MIN;
    void add(Vec2i tile, int period, int anchorX, int floor = -1,
             int sill = -1);
    bool intersects(int x0, int z0, int x1, int z1, int period) const;
};

struct PoolState {
    int level = -1;
    uint8_t fill = 255;
    std::shared_ptr<const PoolFootprint> footprint;
    uint32_t parent = 0;   // a detached lake compartment masks its old owner
    int closedCrest = -1;  // no suspended spill sheet after a wall opens
};

// One phase owner: a standing body, or a single local cell. Frozen volume is
// always stored as water equivalent in the same 255-per-cubic-metre units as
// liquid, so a freeze/thaw pair never loses volume to a density conversion.
struct PhaseKey {
    uint32_t body = 0;  // zero for a local cell
    int32_t x = 0;
    int16_t y = 0;
    int32_t z = 0;
    bool local() const { return body == 0; }
    bool operator==(const PhaseKey&) const = default;
    bool operator<(const PhaseKey& other) const {
        return std::tie(body, x, y, z) <
               std::tie(other.body, other.x, other.y, other.z);
    }
};

struct PhaseState {
    WaterVolume frozen = 0;  // water equivalent
    int64_t progress = 0;    // signed residual phase progress
    double appliedDay = 0;   // last dated application
};

// Surface ice is owned per water column so a shallow bank can freeze through
// before a deeper pool forms its first lid. The liquid it converts stays in
// the body, reach or local cell named here; `frozen` is the water equivalent
// already removed from that owner.
struct SurfaceKey {
    int32_t x = 0;
    int32_t z = 0;
    bool operator==(const SurfaceKey&) const = default;
    bool operator<(const SurfaceKey& other) const {
        return std::tie(x, z) < std::tie(other.x, other.z);
    }
};

struct SurfaceIceState {
    uint32_t body = 0;   // standing body, or zero
    uint32_t reach = 0;  // flowing reach, or zero
    int16_t cellY = 0;   // top water tile, for local conversion and walking
    WaterVolume frozen = 0;
    int64_t progress = 0;
    double appliedDay = 0;
    float waterline = 0;    // ice top at first formation
    float thickness = 0;    // visible ice thickness in metres
    uint8_t stage = 0;      // 0 none, 1..6 stage thickness, 7 freeze-through
    bool through = false;   // ice reaches the bed
    bool stranded = false;  // liquid has fallen below a fixed ice sheet
};

// Metadata written with one surface freeze/thaw step. The volume itself is
// applied by the same immutable snapshot, so liquid debit and ice credit
// never disagree.
struct SurfaceIceInput {
    uint32_t body = 0;
    uint32_t reach = 0;
    int16_t cellY = 0;
    float waterline = 0;
    float thickness = 0;
    uint8_t stage = 0;
    int64_t progress = 0;
    bool through = false;
    bool stranded = false;
};

struct ReservoirSnapshot {
    std::map<uint32_t, int> levels;
    std::map<uint32_t, uint8_t> surfaceFills;
    // Supply changes relative to the generated steady state. A breach only
    // lowers the channel at and below its inlet head.
    struct RiverStage {
        static constexpr int DamRadius = 16;
        int head = WORLD_HEIGHT_Y;
        unsigned percent = 100;
        TilePos cut{};
        Vec2 downstream{0, 0};
        int periodX = 0;
        bool belowCut(int x, int z, int surface) const;
        std::optional<uint64_t> damAge{};
        bool isolatesLake(uint32_t body, int x, int z) const;
        uint32_t outletLake = 0;
        Vec2i spring{};
        uint64_t recessionAge = 0;
        uint64_t recoveryAge = 0;
        bool receding = false;
        bool recovering = false;
        unsigned lakeSupplyPercent = 100;
        struct Backwater {
            Vec2i tile;
            int head;
            uint32_t reach;
            bool operator==(const Backwater&) const = default;
        };
        std::vector<Backwater> banks;
        int backwaterAt(int x, int z, int bottom, int full,
                        uint32_t* source = nullptr,
                        Vec2* direction = nullptr) const;
        std::shared_ptr<const std::vector<uint64_t>> damUpstream{};
        struct Inlet {
            Vec2i tile;
            Vec2i downstream;
            int height;
            unsigned percent;
            bool operator==(const Inlet&) const = default;
        };
        std::vector<Inlet> inlets{};
        bool operator==(const RiverStage&) const = default;
    };
    std::map<uint32_t, RiverStage> riverStages;
    // Each cell's exact store is authoritative here. Chunk cells are derived.
    LocalWaterCells cells;
    // Confirmed portal channels from local cells to a standing body: the
    // pocket drains through them without loading the connecting voxels.
    std::map<TilePos, uint32_t, TilePosLess> portals;
    // Runtime pools. Workers read these without the owner thread.
    std::map<uint32_t, PoolState> pools;
    // A flowing sheet is the visible transit of a certified transfer.
    // Its cells never enter local storage.
    struct StreamColumn {
        int bottom, top;
        Vec2 flow;
    };
    std::unordered_map<uint64_t, StreamColumn> streams;
    // Authoritative freeze state, published with the water volume it changes.
    std::map<PhaseKey, PhaseState> phases;
    // Per-column surface ice, keyed by the water column it covers.
    std::map<SurfaceKey, SurfaceIceState> surfaceIce;

    const PhaseState* phase(const PhaseKey& key) const;
    const SurfaceIceState* surfaceIceAt(int x, int z) const;
    WaterVolume frozen(const PhaseKey& key) const;
    WaterVolume frozenSurfaceTotal() const;

    int level(uint32_t body, int generated) const;
    int localTop(int x, int z) const;
    bool poolIntersects(int x0, int z0, int x1, int z1, int period) const;
    static uint64_t columnKey(int x, int z);

    WaterSurface projectStanding(const GeneratedHydrology* hydrology, int x,
                                 int z, int ground, WaterSurface baseline,
                                 bool includeBackwater = true) const;
    WaterSurface projectLakeInflow(const GeneratedHydrology* hydrology, int x,
                                   int z, int ground, uint32_t body) const;
    WaterSurface project(const GeneratedHydrology* hydrology, int x, int z,
                         int ground, WaterSurface baseline) const;
    WaterSurface projectLocal(int x, int z, int ground, WaterSurface baseline,
                              int* poolGround = nullptr) const;
};

enum class ReservoirExchange : uint8_t {
    Input,
    Disposal,
    Evaporation,
    River,
    Count
};

struct ReservoirChange {
    std::vector<uint32_t> bodies;
    std::vector<uint32_t> reaches;
    std::optional<TilePos> local;
    std::vector<TilePos> locals;
    // Phase owners whose ice or progress changed without a liquid top change.
    std::vector<PhaseKey> phaseOwners;
    // Water columns whose surface ice changed.
    std::vector<SurfaceKey> surfaceOwners;
    // Pool IDs referenced by this change, for footprint lookups without a
    // snapshot. The owner fills it from poolFootprints at publish time.
    std::shared_ptr<const std::map<uint32_t, std::vector<Vec2i>>> pools;
    bool affects(const GeneratedHydrology* hydrology, int x0, int z0, int x1,
                 int z1, int period) const;
};

// A dated precipitation or meltwater input. `interval` is a monotonic dated
// key: a weather interval index for rain, or the exact applied day in
// microseconds for meltwater. A repeated key for the same receiver is ignored,
// so a catch-up replay and the local path cannot both apply it.
struct WaterInput {
    enum class Kind : uint8_t { Rain, Meltwater, Runoff };
    int64_t interval = 0;
    TilePos cell{};
    uint32_t body = 0;
    uint32_t reach = 0;
    WaterVolume amount = 0;  // liquid water units (255 per cubic metre)
    Kind kind = Kind::Rain;
};

struct WaterInputResult {
    WaterVolume accepted = 0;
    WaterVolume overflow = 0;
    bool duplicate = false;
};

// Owner-thread ledger. Workers use the immutable projection snapshot.
class Reservoirs {
   public:
    enum class CellAccess { Open, Blocked, Unknown };
    using CellQuery = std::function<CellAccess(TilePos)>;
    // A cell's free-space profile. Partial solids return a reduced capacity
    // and per-face openings; ordinary open cells may return nullopt (uniform)
    // and fully solid cells are reported as Blocked by the CellQuery.
    using ProfileQuery = std::function<std::optional<fluid::Profile>(TilePos)>;
    explicit Reservoirs(std::shared_ptr<const GeneratedHydrology> generated,
                        uint64_t initialStep = 0);
    void setProfileQuery(ProfileQuery query) {
        profileQuery = std::move(query);
    }
    // Old ledgers treated every partial solid as a whole-cell fluid barrier.
    // Keep that rule on migration so opening a saved dam cannot drain it.
    bool partialCellWater() const { return partialCells; }
    void setPartialCellWater(bool enabled) { partialCells = enabled; }

    std::shared_ptr<const ReservoirSnapshot> snapshot() const {
        return view.load();
    }
    Signal<const ReservoirChange&> changed;

    WaterVolume volume(uint32_t body);
    // Returns the accepted signed change, clamped to stored volume and
    // capacity. Positive input and negative sinks use explicit reasons.
    WaterVolume transfer(uint32_t body, WaterVolume amount,
                         ReservoirExchange reason);
    WaterVolume creditLocal(TilePos recipient, WaterVolume amount,
                            ReservoirExchange reason);
    WaterVolume exchanged(ReservoirExchange reason) const;
    uint64_t revision() const { return serial; }

    // Liquid/ice conversions. A body's frozen volume is debited from its
    // store and credited to the phase entry in one published revision. Thaw
    // that cannot fit back into a full body or cell is an accounted outflow.
    WaterVolume freeze(uint32_t body, WaterVolume amount, double day = -1);
    WaterVolume thaw(uint32_t body, WaterVolume amount, double day = -1);
    WaterVolume freezeLocal(TilePos cell, WaterVolume amount, double day = -1);
    WaterVolume thawLocal(TilePos cell, WaterVolume amount, double day = -1);
    // Per-column surface ice. The owner (body, reach or local cell) is debited
    // or credited in the same snapshot as the ice state. Reach ice is supplied
    // by flowing water and is booked as an accounted input.
    WaterVolume freezeSurface(const SurfaceKey& key, WaterVolume amount,
                              const SurfaceIceInput& input, double day = -1);
    WaterVolume thawSurface(const SurfaceKey& key, WaterVolume amount,
                            const SurfaceIceInput& input, double day = -1);
    bool setSurfaceIce(const SurfaceKey& key, const SurfaceIceInput& input,
                       double day = -1);
    bool clearSurfaceIce(const SurfaceKey& key);
    // Drop surface ice records matching a predicate in one immutable
    // snapshot. Used when generated feature ice takes over a region.
    size_t clearSurfaceIceWhere(const std::function<bool(int, int)>& predicate);
    const SurfaceIceState* surfaceIceAt(int x, int z) const;
    // Legacy independent water lives in chunk tiles with no ledger owner.
    // Adopt it before a phase change so the conversion cannot lose or
    // duplicate the cell. Adoption is not an external input.
    WaterVolume adoptLocal(TilePos cell, uint8_t fill);
    WaterVolume frozen(uint32_t body) const;
    WaterVolume frozenLocal(TilePos cell) const;
    WaterVolume frozenTotal() const;
    // Sum of liquid in defined bodies (stored or at capacity) and local
    // cells. Intended for closed-basin audits.
    WaterVolume liquidTotal() const;
    // Volume waiting in reservoir and reach transfers.
    WaterVolume pendingTransfers() const;
    // Ice removed by excavation. Kept separate from liquid outflow so the
    // phase audit can show where the frozen volume went.
    WaterVolume removedIce() const { return removedIce_; }
    void setPhaseProgress(const PhaseKey& key, int64_t progress,
                          double day = -1);
    bool clearPhase(const PhaseKey& key);
    // Remove a local cell and its phase state after excavation. The liquid
    // becomes disposal and the ice becomes removedIce().
    bool clearLocal(TilePos cell);

    // Dated input boundary for rain and credited snowmelt. Amounts are in
    // liquid water units; addMeltwater/addRain perform the exact conversion
    // from centimetres and keep the fractional remainder in the ledger.
    static constexpr int64_t InputRemainderDenominator = 5'000'000;
    WaterInputResult applyInput(const WaterInput& input);
    WaterVolume addMeltwater(TilePos cell, uint32_t body, uint32_t reach,
                             int64_t bulkSnowCm3, int64_t interval);
    WaterVolume addRain(TilePos cell, uint32_t body, uint32_t reach,
                        int64_t liquidCm3, int64_t interval);
    int64_t inputRemainder() const { return inputRemainder_; }

    // Pass zero to close a reach. A receiver override is a confirmed standing
    // outlet. Flow held at a closed reach is credited only after it leaves.
    bool setReachDelivery(uint32_t reach, unsigned percent,
                          uint32_t receiver = 0);
    bool setSpringOpen(uint32_t reach, bool open);
    // Reservoir-owned water at a cell: a standing body or a river reach and
    // its surface boundary.
    struct Sink {
        uint32_t body = 0;
        uint32_t reach = 0;
        int surfaceY = -1;
    };
    using SinkQuery = std::function<std::optional<Sink>(TilePos)>;
    struct ValleyOutlet {
        Sink receiver;
        std::vector<TilePos> path;  // from the receiving river back to the pond
    };
    struct BasinColumn {
        Vec2i tile;
        int floor;  // geometric floor, inclusive first possible water tile
        int sill;   // lowest connection from the basin seed
        bool operator==(const BasinColumn&) const = default;
    };
    struct BreachSurvey {
        int head;
        std::vector<BasinColumn> columns;
        // Sorted column keys reached from the source inside the dam survey.
        std::shared_ptr<const std::vector<uint64_t>> damUpstream{};
    };
    using SurfaceFloor = std::function<std::optional<int>(Vec2i)>;
    static std::optional<BreachSurvey> surveyBreach(
        Vec2i opening, int head, int periodX, const SurfaceFloor& floor,
        std::vector<TilePos>* route = nullptr, ValleyOutlet* outlet = nullptr,
        const SinkQuery& receiver = {});
    bool connectValley(uint32_t source, TilePos gate, TilePos intake,
                       BreachSurvey survey, std::vector<TilePos> route = {},
                       ValleyOutlet outlet = {{0, 0, -1}, {}});
    bool openBreach(uint32_t reach, TilePos opening, BreachSurvey survey);
    bool openDam(uint32_t reach, TilePos wall, BreachSurvey survey,
                 const std::function<bool(TilePos)>& solid = {});
    bool hasLakeOutletFlow(WaterSurface source) const;
    std::optional<size_t> lakeOutletIndex(uint32_t body) const;
    std::optional<TilePos> damAt(uint32_t reach) const;
    std::vector<std::pair<uint32_t, TilePos>> damSites() const;
    bool closeBreach(TilePos opening);
    bool hasBreach(TilePos opening) const;
    WaterVolume held(uint32_t reach) const;
    // Moves at most budget locally owned cells once. Unknown cells freeze the
    // boundary; the caller must certify loaded, empty recipients. Local water
    // resting on a reservoir cell (sink) merges into that body or reach.
    // An edit or newly loaded slab can open a face beside resting water.
    void wakeLocalBox(TilePos lo, TilePos hi, int periodX);
    size_t flow(size_t budget, const CellQuery& access, int periodX = 0,
                const SinkQuery& sink = {});
    // A verified downhill channel connects a standing body to a lower
    // receiver (neither body nor reach means discharge onto open terrain).
    // It runs only above the threshold (mouth, path and tailwater sill).
    // The ledger moves volume directly, independently of chunk residency.
    static constexpr WaterVolume ChannelUnitsPerMouth = WaterUnitsPerBlock;
    bool setChannelMouth(uint32_t from, Sink to, TilePos mouth, int threshold,
                         std::vector<TilePos> path);
    bool hasChannelMouth(TilePos mouth) const;
    bool feedsPool(TilePos mouth) const;
    void blockChannelCell(TilePos cell);
    bool clearChannelMouth(TilePos mouth);
    size_t channelCount() const { return channels.size(); }
    struct RiverIntake {
        uint32_t reach;
        TilePos cell;
        bool blocked = false;  // undiverted delivery stays behind a dam
        WaterSurface head{WORLD_HEIGHT_Y};
        bool belowDam = false;
    };
    void advanceTo(uint64_t step, std::span<const RiverIntake> intakes = {},
                   const CellQuery& access = {}, int periodX = 0);
    uint64_t step() const { return budgetStep; }

    // Runtime standing water registered by gameplay: a dammed reach's
    // upstream pool, or an enclosed pocket after a split. Pool footprints
    // certify which columns a level change covers; the curve stores the
    // volume (constant-area until a survey refines it).
    bool definePool(uint32_t body, const StorageCurve& curve,
                    std::vector<Vec2i> footprint);
    // Accepts a confirmed dam crest: the reach's held delivery fills the
    // upstream pool up to the crest height, and the surplus spills into the
    // receiver (a standing body, the old downstream body, or another sink)
    // at a bounded rate. poolBody may equal receiver for a terminal pool.
    bool setSpillRoute(uint32_t reach, uint32_t poolBody, uint32_t receiver,
                       int height, WaterVolume ratePerStep);
    std::optional<int> spillHeight(uint32_t reach) const;
    // Registers an exposed aquifer outlet from the terrain edit path, keyed
    // to the stable generated rock site so reopening another face or
    // reloading a chunk cannot duplicate the source. Sealing the face closes
    // the outlet while the key persists. registerOutlet adopts an existing
    // outlet at the same rock and moves its flow to the new face.
    uint32_t registerOutlet(uint32_t reachId, Vec2i rockSite, int rockY,
                            TilePos face, WaterVolume ratePerStep);
    bool closeOutlet(uint32_t outletId);
    // Seals every outlet registered at this rock site (the key persists, so
    // reopening any face of the same rock reuses the outlet).
    bool closeAtRock(int x, int z, int rockY);
    WaterVolume outletRate(uint32_t outlet) const;
    // A certified connection between a local cell and a standing body: the
    // pocket drains through the ledger without loading the voxels between.
    // When the body's level rises above the cell's column top the exchange
    // reverses and the pocket refills from the reservoir.
    bool setPortal(TilePos cell, uint32_t body);
    bool clearPortal(TilePos cell);
    // Applies portal exchanges and spill deliveries once. Called on the
    // owner thread between fixed steps.
    void reconcile();

    Bytes encode() const;
    void restore(ByteView bytes);

    // Used by headless fixtures to exercise exact thresholds.
    void define(uint32_t body, StorageCurve curve);

   private:
    friend class ChunkManager;
    WaterVolume divert(uint32_t body, TilePos recipient, WaterVolume amount);
    bool needsFixedSteps() const;
    bool detachLake(uint32_t parent, TilePos seed, BreachSurvey survey,
                    WaterVolume trapped);
    void rejoinLake(uint32_t body);
    WaterVolume drainCut(uint32_t body, TilePos mouth, uint64_t elapsed,
                         const CellQuery& access, int periodX);

    struct ReachDelivery {
        unsigned percent = 100;
        uint32_t receiver = 0;
        WaterVolume held = 0;
        unsigned remainder = 0;
    };
    // Confirmed dam crest: held delivery fills the pool, surplus spills.
    struct Spill {
        uint32_t pool = 0;  // upstream pool behind the dam
        uint32_t body = 0;  // receiver below the crest
        int height = 0;     // crest elevation
        WaterVolume rate = 0;
        unsigned remainder = 0;
    };
    // A runtime aquifer outlet keyed to a generated rock site. One rock, one
    // outlet, even across faces and chunk reloads.
    struct Outlet {
        uint32_t id = 0;
        uint32_t reach = 0;  // generated reach whose budget this outlet feeds
        Vec2i rock;          // stable rock site key
        TilePos face;        // the exposed cell the flow enters
        int rockY = -1;
        WaterVolume rate = 0;
        WaterVolume held = 0;
        unsigned remainder = 0;
        bool open = false;
    };
    struct Channel {
        uint32_t from = 0;
        Sink to;
        struct Mouth {
            int threshold;
            std::vector<TilePos> path;
            bool operator==(const Mouth&) const = default;
        };
        std::map<TilePos, Mouth, TilePosLess> mouths;
    };
    using ChannelKey = std::tuple<uint32_t, uint32_t, uint32_t>;
    std::map<ChannelKey, Channel> channels;
    std::set<TilePos, TilePosLess> flowingMouths;
    bool channelPathsChanged = false;
    // Arrivals at a full body that leave over its spill on the next budget
    // step (or are disposed of at a terminal body).
    std::map<uint32_t, WaterVolume> pendingInflow;
    void receive(Sink to, WaterVolume amount,
                 std::shared_ptr<ReservoirSnapshot>& next,
                 ReservoirChange& change);
    void drainChannels(uint64_t elapsed,
                       std::shared_ptr<ReservoirSnapshot>& next,
                       ReservoirChange& change);
    using Totals = std::array<WaterVolume, size_t(ReservoirExchange::Count)>;
    const StorageCurve* curve(uint32_t body);
    void updateLevel(ReservoirSnapshot& next, const StorageCurve& curve,
                     uint32_t body, ReservoirChange& change);
    void publish(std::shared_ptr<ReservoirSnapshot> next,
                 const ReservoirChange& change);
    using Node = uint64_t;
    struct Route {
        Node node;
        Node next;
    };
    std::optional<std::vector<Route>> routes(
        const std::map<uint32_t, ReachDelivery>& state) const;
    void buildNaturalBudget();
    void updateRiverStage(uint32_t reach,
                          std::shared_ptr<ReservoirSnapshot>& next,
                          ReservoirChange& change);
    void applyBudget(uint32_t body, WaterVolume amount, WaterVolume loss,
                     bool outlet, WaterVolume& overflow,
                     std::shared_ptr<ReservoirSnapshot>& next,
                     ReservoirChange& change);

    std::shared_ptr<const GeneratedHydrology> generated;
    ProfileQuery profileQuery;
    bool partialCells = true;
    // Reach IDs come from saved paths. Short or rejected paths leave gaps,
    // and generation order is independent of ID order.
    std::unordered_map<uint32_t, WaterVolume> springRates;
    std::unordered_map<uint32_t, size_t> reachIndices;
    std::unordered_map<uint32_t, std::vector<size_t>> tributaries;
    struct JunctionColumn {
        Vec2i tile;
        int ground;
        WaterSurface water;
    };
    const std::vector<JunctionColumn>& junctionBanks(uint32_t reach);
    std::unordered_map<uint32_t, std::vector<JunctionColumn>> junctionColumns;
    std::unordered_map<uint32_t, WaterVolume> naturalReachRates;
    std::map<uint32_t, StorageCurve> curves;
    std::map<uint32_t, WaterVolume> stores;
    std::map<uint32_t, ReachDelivery> deliveries;
    std::unordered_set<uint32_t> groundwaterOutlets;
    std::map<uint32_t, WaterVolume> lossRates;
    std::map<uint32_t, Spill> spills;
    struct Breach {
        uint32_t reach;
        uint32_t pool;
        BreachSurvey survey;
        bool open = true;
        bool oddUnit = false;
        bool dam = false;
        uint32_t parent = 0;
        uint64_t closedAt = 0;
    };
    bool openConnection(uint32_t reach, TilePos opening, BreachSurvey survey,
                        bool dam);
    WaterVolume breachLimit(const Breach& breach) const;
    static StorageCurve breachCurve(const BreachSurvey& survey);
    std::map<TilePos, Breach, TilePosLess> breaches;
    std::unordered_set<uint32_t> sealedSprings;
    std::map<uint32_t, uint64_t> springClosedAt;
    std::map<uint32_t, uint64_t> springReopenedAt;
    std::map<uint32_t, uint32_t> lakeOutletReaches;
    std::map<uint32_t, Outlet> outlets;
    uint32_t nextOutletId = 0;
    // Runtime pool footprints keyed by body: certification that a level
    // covers these columns.
    std::map<uint32_t, std::vector<Vec2i>> poolFootprints;
    std::shared_ptr<const std::map<uint32_t, std::vector<Vec2i>>>
        publishedFootprints;
    // Dated input deduplication: the last applied interval for one receiver
    // and kind. Catching up and nearby detail share the same cursor.
    struct InputKey {
        int32_t x = 0;
        int32_t z = 0;
        uint32_t body = 0;
        uint32_t reach = 0;
        uint8_t kind = 0;
        bool operator<(const InputKey& other) const {
            return std::tie(x, z, body, reach, kind) <
                   std::tie(other.x, other.z, other.body, other.reach,
                            other.kind);
        }
    };
    std::map<InputKey, int64_t> inputCursors;
    // Exact units denominator for cm3 -> water units. One bulk snow cm3 is
    // 0.2 liquid cm3, and one water unit is 1e6/255 cm3.
    int64_t inputRemainder_ = 0;
    WaterVolume removedIce_ = 0;
    WaterVolume acceptInput(const WaterInput& input, WaterVolume amount);
    std::vector<Route> order;
    Totals totals{};
    uint64_t serial = 0;
    uint64_t budgetStep = 0;
    struct FlowOrder {
        bool operator()(TilePos a, TilePos b) const {
            return std::tie(a.y, a.z, a.x) < std::tie(b.y, b.z, b.x);
        }
    };
    std::set<TilePos, FlowOrder> activeCells;
    std::optional<TilePos> flowCursor;
    std::unordered_map<TilePos, uint64_t> filmSince;
    std::atomic<std::shared_ptr<const ReservoirSnapshot>> view =
        std::make_shared<const ReservoirSnapshot>();
};
