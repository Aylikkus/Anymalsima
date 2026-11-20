#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
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

   private:
    Levels storage;
};

struct ReservoirSnapshot {
    std::map<uint32_t, int> levels;
    // Each cell's exact store is authoritative here. Chunk cells are derived.
    std::unordered_map<TilePos, uint8_t> cells;
    std::unordered_map<uint64_t, int> localTops;

    int level(uint32_t body, int generated) const;
    int localTop(int x, int z) const;
    static uint64_t columnKey(int x, int z);

    WaterSurface projectStanding(const GeneratedHydrology* hydrology, int x,
                                 int z, int ground,
                                 WaterSurface baseline) const;
    WaterSurface project(const GeneratedHydrology* hydrology, int x, int z,
                         int ground, WaterSurface baseline) const;
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
    std::optional<TilePos> local;
    bool affects(const GeneratedHydrology* hydrology, int x0, int z0, int x1,
                 int z1, int period) const;
};

// Owner-thread ledger. Workers use the immutable projection snapshot.
class Reservoirs {
   public:
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
    WaterVolume exchanged(ReservoirExchange reason) const;
    uint64_t revision() const { return serial; }

    // Pass zero to close a reach. A receiver override is a confirmed standing
    // outlet. Flow held at a closed reach is credited only after it leaves.
    bool setReachDelivery(uint32_t reach, unsigned percent,
                          uint32_t receiver = 0);
    WaterVolume held(uint32_t reach) const;
    void advanceTo(uint64_t step);
    uint64_t step() const { return budgetStep; }

    Bytes encode() const;
    void restore(ByteView bytes);

    // Used by headless fixtures to exercise exact thresholds.
    void define(uint32_t body, StorageCurve curve);

   private:
    friend class ChunkManager;
    WaterVolume divert(uint32_t body, TilePos recipient, WaterVolume amount);

    struct ReachDelivery {
        unsigned percent = 100;
        uint32_t receiver = 0;
        WaterVolume held = 0;
        unsigned remainder = 0;
    };
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
    std::vector<Route> order;
    Totals totals{};
    uint64_t serial = 0;
    uint64_t budgetStep = 0;
    std::atomic<std::shared_ptr<const ReservoirSnapshot>> view =
        std::make_shared<const ReservoirSnapshot>();
};
