#pragma once
#include <array>
#include <functional>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "core/assets_api.hpp"
#include "items/ground_cover.hpp"
#include "render/vertex.hpp"
#include "water_surface.hpp"

namespace structure_voxels {
struct Slab;
}

// The center and each loaded face neighbour own these inputs for the job.
struct ChunkMeshSnapshot {
    std::shared_ptr<const Chunk> chunk;
    std::shared_ptr<const structure_voxels::Slab> structures;
    GrownBlockSet grown;
    // Environmental solids projected over this chunk (snow). Never saved.
    std::shared_ptr<const ChunkEnvironment> environment;

    static PieceProvenance legacyOrigin(
        uint16_t index, ShapedTile cell,
        const structure_voxels::Slab* structures, const GrownBlockSet& grown);
    SolidCellSnapshot solidCellAt(uint16_t index) const;
};

struct ChunkGeometry {
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;
};

struct ChunkMesh : ChunkGeometry {
    ChunkGeometry water;
    LakeMeshSurface lakeSurface;
    std::vector<float>
        waterTops;  // highest exposed surface per column, chunk-local
};

// Climate at the four horizontal corners of a chunk (index x + 2 * z, in
// chunk corners (0, 0), (16, 0), (0, 16), (16, 16)), interpolated for the
// ground-cover tint. Defaults to a mild, moderately humid climate.
struct CoverClimate {
    std::array<float, 4> humidity{0.5f, 0.5f, 0.5f, 0.5f};
    std::array<float, 4> temperature_c{15, 15, 15, 15};

    std::pair<float, float> at(float fx, float fz) const {
        const auto bilinear = [&](const std::array<float, 4>& v) {
            const float top = v[0] + (v[1] - v[0]) * fx;
            const float bottom = v[2] + (v[3] - v[2]) * fx;
            return top + (bottom - top) * fz;
        };
        return {bilinear(humidity), bilinear(temperature_c)};
    }
};

class ChunkMesher {
   public:
    explicit ChunkMesher(const IAssets& assets);

    struct Neighbour {
        const Tile* tile;
        TileShape shape;
        const ChunkMeshSnapshot* snapshot = nullptr;
        const Chunk* chunk = nullptr;
        uint16_t index = 0;
        EnvironmentCell environment{};

        Neighbour(const Tile* tile, TileShape shape = {})
            : tile(tile), shape(shape) {}
        Neighbour(const ChunkMeshSnapshot& snapshot, int x, int y, int z)
            : tile(&snapshot.chunk->tiles[z][y][x]),
              shape(snapshot.chunk->shapeAt(Chunk::cell(x, y, z))),
              snapshot(&snapshot),
              index(Chunk::cell(x, y, z)),
              environment(snapshot.environment ? [&] {
                  const auto found =
                      snapshot.environment->find(Chunk::cell(x, y, z));
                  return found == snapshot.environment->end()
                             ? EnvironmentCell{}
                             : found->second;
              }()
                                               : EnvironmentCell{}) {}
        Neighbour(const Chunk& chunk, int x, int y, int z)
            : tile(&chunk.tiles[z][y][x]),
              shape(chunk.shapeAt(Chunk::cell(x, y, z))),
              chunk(&chunk),
              index(Chunk::cell(x, y, z)) {}
        Neighbour(const Chunk& chunk, int x, int y, int z,
                  EnvironmentCell environment)
            : Neighbour(chunk, x, y, z) {
            this->environment = std::move(environment);
        }
        bool hasSolid() const {
            return (tile && tile->isSolid()) || environment;
        }
        bool isCompound() const {
            if (environment) return true;
            const Chunk* owner = snapshot ? snapshot->chunk.get() : chunk;
            return owner && owner->hasCompoundAt(index);
        }
        std::optional<SolidCellSnapshot> solidCell() const;
    };
    using NeighbourFn = std::function<Neighbour(int tx, int ty, int tz)>;
    using WaterHaloFn = std::function<const Tile*(int tx, int ty, int tz)>;

    using WaterColumnFn = std::function<std::pair<int, WaterSurface>(int, int)>;

    // Greedy solid quads grow by this much on all four sides. The
    // overlap closes sub-pixel cracks where greedy quads meet in T-junctions
    // (and at wall/floor corners); coplanar overlap is invisible.
    // shaders/light_shadow/light_shadow.frag relies on this value.
    static constexpr float SeamOverlap = 1.f / 512;

    // Vertex positions are chunk-local (0..16, plus SeamOverlap); draw with
    // relativeTransform(camera, chunk origin). Neighbour lookups use world
    // tile coordinates.
    //
    // Ground cover (Tile::cover) is not geometry: faces that show it carry
    // the overlay in otherwise unused attributes, read by block.vert/frag:
    //   u    = overlay material layer, or -1 without cover
    //   v    = side fringe range: min_cm + 1000 * max_cm (0 on top faces)
    //   tint = cover tint from the climate (white without cover)
    // Cover density is not per vertex; LightVolumeManager uploads it.
    ChunkMesh build(const Chunk& chunk, const ChunkCoord& coord,
                    const NeighbourFn& neighbour,
                    const CoverClimate& climate = {},
                    const WaterHaloFn& generatedWaterHalo = {},
                    const WaterColumnFn& waterColumn = {},
                    const ChunkEnvironment* environment = nullptr) const;

    // Packs the side-fringe range for the v attribute (see build).
    static float packFringe(const ValueRange& metres);

   private:
    const IAssets& assets;
    const DataStorage<Block>& blocks;

    // Resolved ground covers, indexed by GroundCover id (0 = none).
    struct CoverInfo {
        bool valid = false;
        bool top = false;
        bool sides = false;
        float layer = -1;
        float fringe = 0;
        GroundCoverTint tint;
    };
    std::vector<CoverInfo> covers;
    const CoverInfo* coverInfo(uint8_t id) const {
        return id < covers.size() && covers[id].valid ? &covers[id] : nullptr;
    }

    // Describes a single face's geometry: normal direction and 4 corners.
    struct FaceDesc {
        float nx, ny, nz;  // face normal
        float tx, ty, tz;  // tangent (used to derive bitangent if needed)
        std::array<std::array<float, 3>, 4> corners;  // CCW from outside
    };

    static const std::array<FaceDesc, 6> FACES;

    // Vertex tint is a plain multiplier reserved for per-block coloring;
    // lighting comes from the GPU light volume (see light_volume.hpp).
    static constexpr uint32_t NeutralTint = 0xFFFFFFFFu;

    // Identifies a mergeable face: same block + texture + tint + ground
    // cover. A zeroed FaceKey (block == nullptr) means "no face / air".
    struct FaceKey {
        const Block* block = nullptr;
        uint16_t texLayer = 0;
        uint32_t tint = 0;
        uint8_t cover = 0;

        bool operator==(const FaceKey& o) const {
            return block == o.block && texLayer == o.texLayer &&
                   tint == o.tint && cover == o.cover;
        }
        bool valid() const { return block != nullptr; }
    };

    void emitSurface(ChunkGeometry& mesh, const tile_geometry::Polygon& face,
                     const tile_geometry::Polygon* occluder, Vec3 cell,
                     TileShape shape, const FaceKey& key,
                     const CoverClimate& climate) const;

    void emitPolygon(ChunkGeometry& mesh, std::span<const Vec3> polygon,
                     Vec3 normal, bool inset, Vec3 cell, TileShape shape,
                     const FaceKey& key, const CoverClimate& climate) const;

    // Emit a single merged quad.
    // origin   – world-space corner of the quad
    // du/dv    – two edge vectors spanning the quad (in world space)
    // face     – which of the 6 canonical face directions
    // key      – texture / tint info
    void emitQuad(ChunkGeometry& mesh, float ox, float oy, float oz, float dux,
                  float duy, float duz, float dvx, float dvy, float dvz,
                  Face face, const FaceKey& key,
                  const CoverClimate& climate) const;
};
