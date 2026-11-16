#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/signal.hpp"
#include "items/light_emission.hpp"
#include "tile_pos.hpp"

// A placed or carried light: an emission definition at a world position.
//
// Block-owned sources mirror every emissive tile. Their near field is
// flood-filled from the tiles themselves; the registry adds what must work
// beyond loaded chunks: the per-pixel far term of far-reaching lights, the
// distant terrain glow of small lights, glare sprites, and shadow maps.
//
// Entity-owned sources (carried torches, rotatable spotlights) are reserved
// for items: they are not tiles, so BlockLightPropagator also reads them to
// build their near field. Their direction may change freely.
struct LightSource {
    enum class Owner : uint8_t { Block, Entity };
    // Face bits in BlockLightPropagator step order: +x, -x, +y, -y, +z, -z.
    static constexpr uint8_t AllFaces = 0x3f;

    Owner owner = Owner::Block;
    TilePos tile;         // Block: the emitting tile
    uint8_t block = 0;    // Block: its block id
    uint32_t entity = 0;  // Entity: owner handle (reserved)
    LightEmission emission;
    std::array<double, 3> position{0, 0, 0};   // world tiles, light centre
    std::array<float, 3> direction{0, -1, 0};  // world forward (cones)
    // Block: faces whose neighbor tile lets light out (not opaque, or not
    // loaded yet). Entities sit in open space.
    uint8_t openFaces = AllFaces;

    // Faces light may leave through: all for omnidirectional lights, the
    // front hemisphere for spotlights.
    static uint8_t frontFaces(const LightEmission& emission,
                              const std::array<float, 3>& direction);
    // No usable face is open: the light is walled in and emits nothing.
    bool sealed() const {
        return (openFaces & frontFaces(emission, direction)) == 0;
    }
};

// Main-thread index of light sources, addressed by id, bucketed by region
// (128 tiles) so thousands of torches stay cheap to query.
class LightSourceRegistry {
   public:
    using Id = uint64_t;
    static constexpr int BucketSize = 128;

    Id add(LightSource source);
    void remove(Id id);
    // Moves or turns an entity light.
    void place(Id id, std::array<double, 3> position,
               std::array<float, 3> direction);
    const LightSource* find(Id id) const;

    // Block lights, keyed by tile: added, replaced or removed together with
    // their tile.
    void setBlockLight(TilePos tile, uint8_t block,
                       const LightEmission& emission,
                       uint8_t openFaces = LightSource::AllFaces);
    void removeBlockLight(TilePos tile);
    const LightSource* blockLight(TilePos tile) const;
    // Block lights inside [min, max) tile bounds (canonical X).
    std::vector<TilePos> blockLightsIn(TilePos min, TilePos max) const;

    template <typename F>
    void forEach(F&& fn) const {
        for (const auto& [id, source] : sources) fn(id, source);
    }
    template <typename F>
    void forEachEntity(F&& fn) const {
        for (const Id id : entities) fn(id, sources.at(id));
    }
    // Sources whose bucket lies within `range` tiles of (x, z) horizontally;
    // callers test exact distances. periodX: world width when X wraps.
    void forEachNear(
        double x, double z, double range, int periodX,
        const std::function<void(Id, const LightSource&)>& fn) const;
    size_t size() const { return sources.size(); }
    size_t entityCount() const { return entities.size(); }
    // Increments on every change; caches (distant glow) rebuild on it.
    uint64_t version() const { return revision; }

    // Block lights changed since the last save of light_sources.dat.
    bool dirty = false;
    // Emitted with the old and the new position of an entity light whenever
    // it is added, moved or removed, so near-field light can be rebuilt.
    Signal<const LightSource&> entityChanged;

   private:
    static int64_t bucketOf(double v) {
        return int64_t(std::floor(v / BucketSize));
    }
    static uint64_t bucketKey(int64_t bx, int64_t bz) {
        return (uint64_t(uint32_t(bx)) << 32) | uint32_t(bz);
    }
    void link(Id id, const LightSource& source);
    void unlink(Id id, const LightSource& source);

    std::unordered_map<Id, LightSource> sources;
    std::unordered_map<TilePos, Id> byTile;
    std::unordered_set<Id> entities;
    std::unordered_map<uint64_t, std::vector<Id>> buckets;
    Id next = 1;
    uint64_t revision = 0;
};

// A far light as seen from the eye: its copy nearest the eye when X wraps.
struct FarLightView {
    const LightSource* source = nullptr;
    LightSourceRegistry::Id id = 0;
    std::array<double, 3> position{0, 0, 0};
    double distance = 0;  // eye to light
};

// Far lights (not sealed) within viewRange of the eye, the ones reaching
// closest to the eye first (ties: nearer first). Lights beyond the view range
// are skipped: their surroundings are not loaded to light or shadow. periodX:
// world width in tiles when X wraps, 0 otherwise.
std::vector<FarLightView> visibleFarLights(const LightSourceRegistry& registry,
                                           std::array<double, 3> eye,
                                           double viewRange, int periodX);
