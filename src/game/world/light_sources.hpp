#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/signal.hpp"
#include "items/light_emission.hpp"
#include "tile_pos.hpp"

// A placed or carried light: an emission definition at a world position.
//
// Block-owned sources mirror emissive tiles whose light reaches beyond the
// voxel near field (LightEmission::isFar). Their near field is flood-filled
// from the tiles themselves; the registry adds the per-pixel far term, the
// glare sprite and visibility while their chunks are unloaded.
//
// Entity-owned sources (carried torches, rotatable spotlights) are reserved
// for items: they are not tiles, so BlockLightPropagator also reads them to
// build their near field. Their direction may change freely.
struct LightSource {
    enum class Owner : uint8_t { Block, Entity };

    Owner owner = Owner::Block;
    TilePos tile;         // Block: the emitting tile
    uint8_t block = 0;    // Block: its block id
    uint32_t entity = 0;  // Entity: owner handle (reserved)
    LightEmission emission;
    std::array<double, 3> position{0, 0, 0};   // world tiles, light centre
    std::array<float, 3> direction{0, -1, 0};  // world forward (cones)
};

// Main-thread index of light sources, addressed by id.
//
// The far-light count is expected to stay small (lighthouses, beacons), so
// queries scan linearly; a spatial index can replace that without changing
// callers.
class LightSourceRegistry {
   public:
    using Id = uint64_t;

    Id add(LightSource source);
    void remove(Id id);
    // Moves or turns an entity light.
    void place(Id id, std::array<double, 3> position,
               std::array<float, 3> direction);
    const LightSource* find(Id id) const;

    // Block lights, keyed by tile: added, replaced or removed together with
    // their tile. setBlockLight keeps only far emissions.
    void setBlockLight(TilePos tile, uint8_t block,
                       const LightEmission& emission);
    void removeBlockLight(TilePos tile);
    const LightSource* blockLight(TilePos tile) const;
    // Block lights inside [min, max) tile bounds (canonical X).
    std::vector<TilePos> blockLightsIn(TilePos min, TilePos max) const;

    template <typename F>
    void forEach(F&& fn) const {
        for (const auto& [id, source] : sources) fn(id, source);
    }
    size_t size() const { return sources.size(); }

    // Block lights changed since the last save of light_sources.dat.
    bool dirty = false;
    // Emitted with the old and the new position of an entity light whenever
    // it is added, moved or removed, so near-field light can be rebuilt.
    Signal<const LightSource&> entityChanged;

   private:
    std::unordered_map<Id, LightSource> sources;
    std::unordered_map<TilePos, Id> byTile;
    Id next = 1;
};

// A far light as seen from the eye: its copy nearest the eye when X wraps.
struct FarLightView {
    const LightSource* source = nullptr;
    std::array<double, 3> position{0, 0, 0};
    double distance = 0;  // eye to light
};

// Far lights whose radius reaches within viewRange of the eye, the ones
// reaching closest to the eye first (ties: nearer first). periodX: world
// width in tiles when X wraps, 0 otherwise.
std::vector<FarLightView> visibleFarLights(const LightSourceRegistry& registry,
                                           std::array<double, 3> eye,
                                           double viewRange, int periodX);
