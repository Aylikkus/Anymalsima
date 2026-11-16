#pragma once
#include <array>
#include <functional>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "core/assets_api.hpp"
#include "render/vertex.hpp"

struct ChunkMesh {
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;
};

enum class Face : uint8_t {
    PosX = 0,
    NegX = 1,
    PosY = 2,
    NegY = 3,
    PosZ = 4,
    NegZ = 5,
};

class ChunkMesher {
   public:
    explicit ChunkMesher(const IAssets& assets)
        : assets(assets), blocks(assets.getStorage<Block>()) {}

    using NeighbourFn = std::function<const Tile*(int tx, int ty, int tz)>;

    // Every quad grows by this much on all four sides within its plane. The
    // overlap closes sub-pixel cracks where greedy quads meet in T-junctions
    // (and at wall/floor corners); coplanar overlap is invisible.
    static constexpr float SeamOverlap = 1.f / 512;

    // Vertex positions are chunk-local (0..16, plus SeamOverlap); draw with
    // relativeTransform(camera, chunk origin). Neighbour lookups use world
    // tile coordinates.
    ChunkMesh build(const Chunk& chunk, const ChunkCoord& coord,
                    const NeighbourFn& neighbour) const;

   private:
    const IAssets& assets;
    const DataStorage<Block>& blocks;

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

    // Identifies a mergeable face: same block + texture + tint.
    // A zeroed FaceKey (block == nullptr) means "no face / air".
    struct FaceKey {
        const Block* block = nullptr;
        uint16_t texLayer = 0;
        uint32_t tint = 0;

        bool operator==(const FaceKey& o) const {
            return block == o.block && texLayer == o.texLayer && tint == o.tint;
        }
        bool valid() const { return block != nullptr; }
    };

    bool isFaceVisible(const Tile* nbr) const {
        if (!nbr) return false;     // outside world = invisible
        return nbr->block_id == 0;  // only air exposes a face
    }

    // Emit a single merged quad.
    // origin   – world-space corner of the quad
    // du/dv    – two edge vectors spanning the quad (in world space)
    // face     – which of the 6 canonical face directions
    // key      – texture / tint info
    void emitQuad(ChunkMesh& mesh, float ox, float oy, float oz, float dux,
                  float duy, float duz, float dvx, float dvy, float dvz,
                  Face face, const FaceKey& key) const;
};