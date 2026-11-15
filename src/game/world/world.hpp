#pragma once

#include <vector>

#include "core/assets_api.hpp"
#include "core/geometry.hpp"
#include "enums.hpp"
#include "region.hpp"
#include "render/vertex.hpp"

struct MeshChunk {
    std::vector<TerrainVertex> lod0;
    std::vector<TerrainVertex> lod1;
    std::vector<TerrainVertex> lod2;
    std::vector<TerrainVertex> lod3;
    std::vector<TerrainVertex> lod4;
    std::vector<TerrainVertex> lod5;
    std::vector<TerrainVertex> lod6;
    std::vector<TerrainVertex> lod7;

    Rect bounds;  // for culling
};

struct RegionMesh {
    Vec2i chunk_size = {32, 32};

    int chunks_x;
    int chunks_y;

    std::vector<MeshChunk> chunks;

    MeshChunk& getChunk(int cx, int cy) { return chunks[cy * chunks_x + cx]; }

    const MeshChunk& getChunk(int cx, int cy) const {
        return chunks[cy * chunks_x + cx];
    }
};

struct RiverChunk {
    std::vector<RiverVertex> vertices;
    Rect bounds;
};

struct RiverMesh {
    Vec2i chunk_size = {32, 32};

    int chunks_x;
    int chunks_y;

    std::vector<RiverChunk> chunks;

    const RiverChunk& getChunk(int cx, int cy) const {
        return chunks[cy * chunks_x + cx];
    }

    RiverChunk& getChunk(int cx, int cy) { return chunks[cy * chunks_x + cx]; }
};

struct RiverPolyline {
    std::vector<std::pair<Vec2, float>> path;
    std::vector<RiverVertex> mesh;  // replaces std::vector<Vec2>
    Rect bounds;
};

struct WorldGenSettings {
    // Master seed identifying the world; the sub-seeds below derive from it.
    uint64_t seed = 0;
    int width = 500;
    int height = 300;

    int height_seed;
    int humidity_seed;
    int temperature_seed;
    int feature_seed;

    float resourceRichness;  // don't touch it yet
    float energyLevel;       // don't touch it yet
    float min_temp_c = -40.0f;
    float max_temp_c = 50.0f;

    // Deterministically derives every sub-seed from one master seed.
    static WorldGenSettings fromSeed(uint64_t seed) {
        auto next = [state = seed]() mutable {
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return static_cast<int>(static_cast<uint32_t>(z ^ (z >> 31)));
        };
        WorldGenSettings settings;
        settings.seed = seed;
        settings.height_seed = next();
        settings.humidity_seed = next();
        settings.temperature_seed = next();
        settings.feature_seed = next();
        settings.resourceRichness = 0;
        settings.energyLevel = 0;
        return settings;
    }
};

class World {
   public:
    std::vector<Region> regions;
    std::vector<RegionMesh> regionMeshes;
    std::vector<RiverPolyline> rivers;
    RiverMesh riverMesh;
    WorldGenSettings genSettings;
    void update();

    // Location& loadLocation(const RegionCoord& coord);
    // void unloadLocation(const RegionCoord& coord);

    int width;
    int height;
    float min_temp_c;
    float max_temp_c;

   private:
    int64_t makeKey(int x, int y);
};