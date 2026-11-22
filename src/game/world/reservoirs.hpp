#pragma once

#include <array>
#include <atomic>
#include <climits>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/signal.hpp"
#include "serialization/binary_stream.hpp"
#include "tile_pos.hpp"
#include "water_surface.hpp"

class GeneratedHydrology;

using WaterVolume = int64_t;
inline constexpr WaterVolume WaterUnitsPerBlock = 255;

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
    uint32_t parent = 0;  // a detached lake compartment masks its old owner
};

struct ReservoirSnapshot {
    std::map<uint32_t, int> levels;
    std::map<uint32_t, uint8_t> surfaceFills;
    // Supply changes relative to the generated steady state. A breach only
    // lowers the channel at and below its inlet head.
    struct RiverStage {
        int head = WORLD_HEIGHT_Y;
        unsigned percent = 100;
        TilePos cut{};
        Vec2i downstream{};
        int periodX = 0;
        bool operator==(const RiverStage&) const = default;
    };
    std::map<uint32_t, RiverStage> riverStages;
    // Each cell's exact store is authoritative here. Chunk cells are derived.
    std::unordered_map<TilePos, uint8_t> cells;
    std::unordered_map<uint64_t, int> localTops;
    // Confirmed portal channels from local cells to a standing body: the
    // pocket drains through them without loading the connecting voxels.
    std::map<TilePos, uint32_t, TilePosLess> portals;
    // Runtime pools. Workers read these without the owner thread.
    std::map<uint32_t, PoolState> pools;

    int level(uint32_t body, int generated) const;
    int localTop(int x, int z) const;
    bool poolIntersects(int x0, int z0, int x1, int z1, int period) const;
    static uint64_t columnKey(int x, int z);

    WaterSurface projectStanding(const GeneratedHydrology* hydrology, int x,
                                 int z, int ground,
                                 WaterSurface baseline) const;
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
    // Pool IDs referenced by this change, for footprint lookups without a
    // snapshot. The owner fills it from poolFootprints at publish time.
    std::shared_ptr<const std::map<uint32_t, std::vector<Vec2i>>> pools;
    bool affects(const GeneratedHydrology* hydrology, int x0, int z0, int x1,
                 int z1, int period) const;
};

// Owner-thread ledger. Workers use the immutable projection snapshot.
class Reservoirs {
   public:
    enum class CellAccess { Open, Blocked, Unknown };
    using CellQuery = std::function<CellAccess(TilePos)>;
    explicit Reservoirs(std::shared_ptr<const GeneratedHydrology> generated,
                        uint64_t initialStep = 0);

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

    // Pass zero to close a reach. A receiver override is a confirmed standing
    // outlet. Flow held at a closed reach is credited only after it leaves.
    bool setReachDelivery(uint32_t reach, unsigned percent,
                          uint32_t receiver = 0);
    bool setSpringOpen(uint32_t reach, bool open);
    struct BasinColumn {
        Vec2i tile;
        int floor;  // geometric floor, inclusive first possible water tile
        int sill;   // lowest connection from the basin seed
        bool operator==(const BasinColumn&) const = default;
    };
    struct BreachSurvey {
        int head;
        std::vector<BasinColumn> columns;
    };
    using SurfaceFloor = std::function<std::optional<int>(Vec2i)>;
    static std::optional<BreachSurvey> surveyBreach(Vec2i opening, int head,
                                                    int periodX,
                                                    const SurfaceFloor& floor);
    bool openBreach(uint32_t reach, TilePos opening, BreachSurvey survey);
    bool openDam(uint32_t reach, TilePos wall, BreachSurvey survey);
    std::optional<TilePos> damAt(uint32_t reach) const;
    bool closeBreach(TilePos opening);
    bool hasBreach(TilePos opening) const;
    WaterVolume held(uint32_t reach) const;
    // Reservoir-owned water at a cell: a standing body or a river reach and
    // its surface boundary.
    struct Sink {
        uint32_t body = 0;
        uint32_t reach = 0;
        int surfaceY = -1;
    };
    using SinkQuery = std::function<std::optional<Sink>(TilePos)>;
    // Moves at most budget locally owned cells once. Unknown cells freeze the
    // boundary; the caller must certify loaded, empty recipients. Local water
    // resting on a reservoir cell (sink) merges into that body or reach.
    size_t flow(size_t budget, const CellQuery& access, int periodX = 0,
                const SinkQuery& sink = {});
    // A verified downhill channel connects a standing body to a lower
    // receiver. Each mouth cell carries up to one block per second while the
    // source surface stands above its threshold (the mouth floor, the path
    // sill and the receiver surface). The ledger moves the volume directly,
    // so the rate is independent of frame rate, game speed and residency.
    static constexpr WaterVolume ChannelUnitsPerMouth = WaterUnitsPerBlock;
    bool setChannelMouth(uint32_t from, Sink to, TilePos mouth, int threshold,
                         std::vector<TilePos> path);
    void blockChannelCell(TilePos cell);
    bool clearChannelMouth(TilePos mouth);
    size_t channelCount() const { return channels.size(); }
    struct RiverIntake {
        uint32_t reach;
        TilePos cell;
        bool blocked = false;  // undiverted delivery stays behind a dam
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
    // Reach IDs come from saved paths. Short or rejected paths leave gaps,
    // and generation order is independent of ID order.
    std::unordered_map<uint32_t, WaterVolume> springRates;
    std::map<uint32_t, StorageCurve> curves;
    std::map<uint32_t, WaterVolume> stores;
    std::map<uint32_t, ReachDelivery> deliveries;
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
    };
    bool openConnection(uint32_t reach, TilePos opening, BreachSurvey survey,
                        bool dam);
    static StorageCurve breachCurve(const BreachSurvey& survey);
    std::map<TilePos, Breach, TilePosLess> breaches;
    std::unordered_set<uint32_t> sealedSprings;
    std::map<uint32_t, Outlet> outlets;
    uint32_t nextOutletId = 0;
    // Runtime pool footprints keyed by body: certification that a level
    // covers these columns.
    std::map<uint32_t, std::vector<Vec2i>> poolFootprints;
    std::shared_ptr<const std::map<uint32_t, std::vector<Vec2i>>>
        publishedFootprints;
    std::vector<Route> order;
    Totals totals{};
    uint64_t serial = 0;
    uint64_t budgetStep = 0;
    size_t flowCursor = 0;
    std::atomic<std::shared_ptr<const ReservoirSnapshot>> view =
        std::make_shared<const ReservoirSnapshot>();
};
