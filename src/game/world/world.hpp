#pragma once

#include <vector>

#include "core/geometry.hpp"
#include "enums.hpp"
#include "region.hpp"
#include "render/vertex.hpp"
#include "core/assets_api.hpp"


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