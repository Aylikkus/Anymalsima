#pragma once

#include <bitset>
#include <cmath>
#include <memory>
#include <vector>

#include "core/assets_api.hpp"
#include "flora.hpp"
#include "terrain_sampler.hpp"

// Resolved species rules (docs/handoff/flora.md, sections 3 and 4).
// Immutable and free of asset pointers, so generation jobs share it.
struct FloraSpeciesRule {
    uint8_t id = 0;
    uint32_t seed = 0;  // from the species name, stable across file order
    // Large plants have a tile footprint, block placement when set, and
    // resolve conflicts with each other; small plants share tiles freely.
    bool large = false;
    bool blocksPlacement = false;
    int footprintX = 1, footprintZ = 1;
    float spacing = 0;  // extra clearance around a large footprint, tiles
    int priority = 0;
    bool persistentSites = false;  // placed with the epoch-free seed
    // Jittered candidate grid: `perCell` candidates per cell of `cell`
    // tiles, each kept with probability density * cell² / 100 / perCell.
    int cell = 1;
    int perCell = 1;
    ValueRange density{1};
    int clusterScale = 0;  // 0: no clustering
    float clusterThreshold = 0;
    float maxSlope = 1;
    bool underwater = false;
    std::bitset<256> coverRequired, coverForbidden;
    std::bitset<256> soilAllowed;  // none set: any soil
    ValueRange fertility{0, 1}, retention{0, 1};
    std::vector<SoilPlacement> climate;
    std::vector<VegetationType> vegetation;  // empty: any
    struct Companion {
        uint8_t species;
        float radius;
        float chance;
    };
    std::vector<Companion> companions;
    // Tiles above the anchor the plant fills (placement and removal).
    int heightTiles = 1;
    double maxAgeDays = 365;  // spreads generated plants over this many days
};

// Resolves a placement block (flora or tree species) into rule fields:
// covers, soils, climate, vegetation, slope, density and clustering.
void floraPlacementRule(const FloraRules& placement, const IAssets& assets,
                        FloraSpeciesRule& rule);

class FloraSpeciesTable {
   public:
    FloraSpeciesTable() = default;
    // Empty rules without flora or soil data (headless fixtures).
    explicit FloraSpeciesTable(const IAssets& assets);

    const std::vector<FloraSpeciesRule>& species() const { return rules; }
    const FloraSpeciesRule* find(uint8_t id) const {
        return id && id <= byId.size() && byId[id - 1] >= 0
                   ? &rules[size_t(byId[id - 1])]
                   : nullptr;
    }
    bool empty() const { return rules.empty(); }
    // Large plants within verdict() tiles of a chunk can matter to it (a
    // footprint over it, or companions reaching into it). Deciding one needs
    // its rivals, so candidates are evaluated reach() tiles around, which
    // covers verdict() plus one footprint with clearance.
    int reach() const { return reachTiles; }
    int verdict() const { return verdictTiles; }
    // Tallest species, in tiles above the anchor.
    int maxHeight() const { return maxHeightTiles; }
    // Widest footprint, in tiles.
    int maxFootprint() const { return maxFootprintTiles; }

    struct SoilInfo {
        SoilProperties base, topsoil;
    };
    const std::vector<SoilInfo>& soils() const { return soilTable; }
    // Soil a block belongs to (0: none) and whether it is its topsoil.
    std::pair<uint8_t, bool> soilOfBlock(uint8_t block) const {
        return block < blockSoil.size() ? blockSoil[block]
                                        : std::pair<uint8_t, bool>{0, false};
    }

   private:
    std::vector<FloraSpeciesRule> rules;
    std::vector<int> byId;
    std::vector<SoilInfo> soilTable;                  // index soil id - 1
    std::vector<std::pair<uint8_t, bool>> blockSoil;  // index block id
    int reachTiles = 0;
    int verdictTiles = 0;
    int maxHeightTiles = 0;
    int maxFootprintTiles = 1;
};

// The tiles a plant occupies: its footprint (one tile for small plants)
// from the tile above its anchor up to its height.
struct FloraVolume {
    int64_t minX, minZ;  // world tiles (canonical frame of its chunk)
    int sizeX, sizeZ;
    int anchorY;  // world Y of the supporting tile
    int height;   // tiles above the anchor
    bool contains(int64_t x, int y, int64_t z) const {
        return x >= minX && x < minX + sizeX && z >= minZ && z < minZ + sizeZ &&
               y > anchorY && y <= anchorY + height;
    }
    bool supportedBy(int64_t x, int y, int64_t z) const {
        return y == anchorY && x >= minX && x < minX + sizeX && z >= minZ &&
               z < minZ + sizeZ;
    }
};
inline FloraVolume floraVolume(const FloraInstance& p, const ChunkCoord& c,
                               const FloraSpeciesRule& r) {
    const double cx = double(c.x) * Chunk::SIZE_X + p.localX();
    const double cz = double(c.z) * Chunk::SIZE_Z + p.localZ();
    const int sx = r.large ? r.footprintX : 1, sz = r.large ? r.footprintZ : 1;
    return {int64_t(std::floor(cx - sx * 0.5 + (r.large ? 0 : 0.5))),
            int64_t(std::floor(cz - sz * 0.5 + (r.large ? 0 : 0.5))),
            sx,
            sz,
            int(c.y) * Chunk::SIZE_Y + p.y,
            r.heightTiles};
}

// The ground a plant would grow on.
struct FloraSite {
    int surfaceY = 0;  // world Y of the supporting tile
    double slope = 0;
    uint8_t cover = 0, coverDensity = 0;
    uint8_t soil = 0;  // Soil id, 0 = none (rock)
    bool topsoil = false;
    const Region* region = nullptr;  // climate
    bool submerged = false;
};
// Whether a species grows on a site: soil, ground cover, slope, water and
// climate rules. Shared by generation and spreading.
bool floraSuits(const FloraSpeciesTable& table, const FloraSpeciesRule& rule,
                const FloraSite& site);

// Where a species clusters: 0 outside its patches to 1 inside them, from
// placement.cluster (1 without clustering). worldSeed: the flora seed of
// the world (ChunkManager::floraSeed). Generation and spreading share it, so
// grown populations keep the generated patches.
double floraClusterMask(const FloraSpeciesRule& rule, uint32_t worldSeed,
                        double x, double z, int periodX);

// Rough height of a species' mature plant in metres, from its recipe (the
// largest part extent), used for placement blocking and removal.
float floraHeightEstimate(const Flora& flora);

// A tree trunk's footprint on the ground: plants never grow inside it.
// x, z in the same frame as the chunk's world tiles (nearest X image).
struct TreeDisc {
    double x = 0, z = 0, radius = 0;
    bool contains(double px, double pz, double pad = 0) const {
        const double dx = px - x, dz = pz - z;
        return dx * dx + dz * dz < (radius + pad) * (radius + pad);
    }
};

struct FloraGenerationInput {
    const FloraSpeciesTable* rules = nullptr;
    const TerrainNeighborhood* terrain = nullptr;
    // Generated columns of this chunk column (TerrainSampler::columns).
    const ChunkColumnInputs* columns = nullptr;
    ChunkCoord coord{};
    uint32_t seed = 0;
    // Decoded tiles of a saved chunk: plants whose anchor tile is no longer
    // solid with air above are dropped. Null for freshly generated chunks.
    const Chunk* tiles = nullptr;
    // The world's flora seed without the epoch salt (0 = `seed`): it places
    // cluster patches (habitat) and species with persistent sites, which
    // stay fixed while `seed` varies by epoch.
    uint32_t clusterSeed = 0;
    // Tree trunks near the chunk (docs/handoff/trees.md); plants whose
    // anchor or footprint falls inside one are not placed. Must hold every
    // trunk within reach() of the chunk, so neighbours agree.
    const std::vector<TreeDisc>* treeDiscs = nullptr;
};

// Plants anchored in one chunk. Deterministic: a pure function of the world
// seed, the species rules and world coordinates, so neighbouring chunks
// agree on plants near their border without communicating, whatever the
// job order, and the pattern repeats across the X seam.
FloraLayer generateFlora(const FloraGenerationInput& input);
