#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/signal.hpp"
#include "items/thermal_emission.hpp"
#include "tile_pos.hpp"

// A placed block or carried/dropped item that warms or cools nearby space.
//
// Block-owned sources mirror every thermal tile in loaded chunks and are
// rebuilt when those chunks stream in. Item-owned sources outlive chunk
// residency and are saved in thermal_sources.dat. Both owners use the same
// definition and query, so a block or item behaves identically once placed.
//
// The optional `key` is a stable producer key. When a large source has both a
// detailed local representation and a dated regional summary (for example a
// future large fire), both must use the same key. A registered detailed
// source wins, so the regional summary never contributes twice.
struct ThermalSource {
    enum class Owner : uint8_t { Block, Item };

    Owner owner = Owner::Block;
    TilePos tile;       // Block: the emitting tile
    uint8_t block = 0;  // Block: its block id
    uint8_t item = 0;   // Item: ThermalItem id
    ThermalEmission emission;
    std::array<double, 3> position{0, 0, 0};  // world metres
    uint64_t key = 0;  // stable coarse/detailed handover key
    // Effective output. Fixed sources keep this equal to emission.deltaC;
    // the room simulation updates a target (thermostat) source from the
    // measured room air temperature. Runtime-only, but persisted indirectly
    // by the room state that reproduces it on the next tick.
    float commandDeltaC = 0;
    bool thermostatSaturated = false;

    float effectiveDeltaC() const { return commandDeltaC; }
    float offsetAt(float distance) const {
        return emission.offsetAt(distance, commandDeltaC);
    }
};

// Main-thread index of thermal sources, bucketed by 32 tiles so a local query
// touches only nearby buckets. The bucket walk wraps on X when the world does.
class ThermalSourceRegistry {
   public:
    using Id = uint64_t;
    static constexpr int BucketSize = 32;

    Id add(ThermalSource source);
    void remove(Id id);
    // Moves an item source. Block sources are re-registered from their tile.
    void place(Id id, std::array<double, 3> position);
    // Sets a thermostat's current output. Fixed sources keep theirs from the
    // definition. The room simulation calls this once per update.
    void setCommand(Id id, float deltaC, bool saturated);
    const ThermalSource* find(Id id) const;

    // Block sources are owned by the tiles in loaded chunks.
    void setBlockThermal(TilePos tile, uint8_t block,
                         const ThermalEmission& emission);
    void removeBlockThermal(TilePos tile);
    const ThermalSource* blockThermal(TilePos tile) const;
    std::vector<TilePos> blockThermalsIn(TilePos min, TilePos max) const;

    template <typename F>
    void forEach(F&& fn) const {
        for (const auto& [id, source] : sources) fn(id, source);
    }
    template <typename F>
    void forEachItem(F&& fn) const {
        for (const Id id : items) fn(id, sources.at(id));
    }
    // Sources whose bucket lies within `range` metres of (x, z) horizontally;
    // callers test exact distances. periodX: world width in tiles, 0 without
    // wrapping.
    void forEachNear(
        double x, double z, double range, int periodX,
        const std::function<void(Id, const ThermalSource&)>& fn) const;

    // A detailed source with this producer key is registered.
    bool containsKey(uint64_t key) const {
        const auto it = keyCounts.find(key);
        return it != keyCounts.end() && it->second != 0;
    }
    size_t size() const { return sources.size(); }
    size_t itemCount() const { return items.size(); }
    uint64_t version() const { return revision; }

    // Item sources changed since thermal_sources.dat was written. Block
    // sources are derived from chunks and never set this.
    bool dirty = false;
    // Emitted after an item source is added, moved or removed.
    Signal<const ThermalSource&> itemChanged;

    static uint64_t blockKey(TilePos tile);

   private:
    static int64_t bucketOf(double v) {
        return int64_t(std::floor(v / BucketSize));
    }
    static uint64_t bucketKey(int64_t bx, int64_t bz) {
        return (uint64_t(uint32_t(bx)) << 32) | uint32_t(bz);
    }
    void link(Id id, const ThermalSource& source);
    void unlink(Id id, const ThermalSource& source);

    std::unordered_map<Id, ThermalSource> sources;
    std::unordered_map<TilePos, Id> byTile;
    std::unordered_set<Id> items;
    std::unordered_map<uint64_t, std::vector<Id>> buckets;
    std::unordered_map<uint64_t, size_t> keyCounts;
    Id next = 1;
    uint64_t revision = 0;
};
