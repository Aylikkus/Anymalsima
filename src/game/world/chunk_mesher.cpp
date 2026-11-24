#include "chunk_mesher.hpp"

#include <cmath>
#include <span>

#include "items/block.hpp"
#include "items/material.hpp"
ChunkMesher::ChunkMesher(const IAssets& assets)
    : assets(assets), blocks(assets.getStorage<Block>()) {
    if (!assets.hasStorage<GroundCover>()) return;
    const auto& storage = assets.getStorage<GroundCover>();
    covers.resize(storage.items.size() + 1);
    for (const auto& cover : storage.items) {
        auto& info = covers[cover.id];
        info.valid = true;
        info.top = cover.faces.top;
        info.sides = cover.faces.sides;
        info.layer =
            float(assets.getMaterialAlbedoLayer(cover.overlay_material));
        info.fringe =
            cover.faces.sides ? packFringe(cover.faces.side_fringe_m) : 0;
        info.tint = cover.tint_gradient;
    }
}

float ChunkMesher::packFringe(const ValueRange& metres) {
    const auto cm = [](float m) {
        return std::clamp(std::round(m * 100.f), 0.f, 999.f);
    };
    return cm(metres.min) + 1000.f * cm(metres.max);
}

// ---------------------------------------------------------------------------
// Face descriptors (identical to the original — geometry is unchanged).
// Normal, tangent, and 4 CCW corners from the outside.
// ---------------------------------------------------------------------------
const std::array<ChunkMesher::FaceDesc, 6> ChunkMesher::FACES = {{
    // PosX
    {1, 0, 0, 0, 0, -1, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}},
    // NegX
    {-1, 0, 0, 0, 0, 1, {{{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}}}},
    // PosY
    {0, 1, 0, 1, 0, 0, {{{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}}}},
    // NegY
    {0, -1, 0, 1, 0, 0, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}},
    // PosZ
    {0, 0, 1, 1, 0, 0, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
    // NegZ
    {0, 0, -1, -1, 0, 0, {{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}},
}};

// ---------------------------------------------------------------------------
// emitQuad
//
// Emits a single axis-aligned rectangle as two triangles.
//
//  origin (ox,oy,oz)  — world-space position of corner 0
//  du                 — first edge vector  (spans 'width'  of the merged strip)
//  dv                 — second edge vector (spans 'height' of the merged strip)
//
// Winding: 0,1,2, 0,2,3  (counter-clockwise from outside, same as before).
// UV corners are scaled to [0, width] x [0, height] so textures tile
// naturally across merged quads.
// ---------------------------------------------------------------------------
void ChunkMesher::emitQuad(ChunkGeometry& mesh, float ox, float oy, float oz,
                           float dux, float duy, float duz, float dvx,
                           float dvy, float dvz, Face face, const FaceKey& key,
                           const CoverClimate& climate) const {
    const FaceDesc& fd = FACES[static_cast<int>(face)];

    // Derive the UV extents from the edge vector magnitudes so textures
    // tile across multi-block quads (integer steps = 1 tile each).
    float uMax = std::sqrt(dux * dux + duy * duy + duz * duz);
    float vMax = std::sqrt(dvx * dvx + dvy * dvy + dvz * dvz);

    // Liquid tops use the exact tile footprint at the LOD handoff.
    if (key.texLayer != UINT16_MAX) {
        const float e = SeamOverlap;
        const float ux = dux / uMax, uy = duy / uMax, uz = duz / uMax;
        const float vx = dvx / vMax, vy = dvy / vMax, vz = dvz / vMax;
        ox -= e * (ux + vx);
        oy -= e * (uy + vy);
        oz -= e * (uz + vz);
        dux += 2 * e * ux;
        duy += 2 * e * uy;
        duz += 2 * e * uz;
        dvx += 2 * e * vx;
        dvy += 2 * e * vy;
        dvz += 2 * e * vz;
    }

    struct Corner {
        float x, y, z, u, v;
    };
    const Corner corners[4] = {
        {ox, oy, oz, 0.f, 0.f},
        {ox + dux, oy + duy, oz + duz, uMax, 0.f},
        {ox + dux + dvx, oy + duy + dvy, oz + duz + dvz, uMax, vMax},
        {ox + dvx, oy + dvy, oz + dvz, 0.f, vMax},
    };

    uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const CoverInfo* cover = coverInfo(key.cover);

    for (const auto& c : corners) {
        BlockVertex v;
        v.x = c.x;
        v.y = c.y;
        v.z = c.z;
        v.nx = fd.nx;
        v.ny = fd.ny;
        v.nz = fd.nz;
        v.tx = fd.tx;
        v.ty = fd.ty;
        v.tz = fd.tz;
        // Shaders texture faces triplanarly from world positions, so u/v
        // carry the ground cover instead of texture coordinates.
        v.u = -1;
        v.v = 0;
        v.layer = key.texLayer == UINT16_MAX ? -2.f : float(key.texLayer);
        v.tint = key.tint;
        if (cover) {
            v.u = cover->layer;
            v.v = face == Face::PosY ? 0.f : cover->fringe;
            const auto [humidity, temperature] =
                climate.at(std::clamp(c.x / Chunk::SIZE_X, 0.f, 1.f),
                           std::clamp(c.z / Chunk::SIZE_Z, 0.f, 1.f));
            v.tint = cover->tint.at(humidity, temperature).packed();
        }
        mesh.vertices.push_back(v);
    }

    mesh.indices.insert(mesh.indices.end(), {base + 0, base + 1, base + 2,
                                             base + 0, base + 2, base + 3});
}

// ---------------------------------------------------------------------------
// build  —  greedy meshing
//
// Algorithm overview (per axis, per slab, per face direction):
//
//   The chunk has dimensions [SIZE_XY, SIZE_XY, SIZE_Z] in local (lx,ly,lz).
//   World mapping: wx = originX+lx,  wy = originY+lz,  wz = originZ+ly
//   (same coord remapping as the original mesher).
//
//   We iterate over the 3 axes (d = 0,1,2) and for each axis over all
//   parallel slabs.  For axis d, the two perpendicular axes are u and v.
//   Within a slab we build a 2-D "face mask": each cell stores the FaceKey
//   for the face that should be emitted there (or an invalid key for none).
//
//   Two face directions exist per axis: +d and -d.  We process both in the
//   same slab pass to avoid iterating twice.
//
//   Greedy merge:
//     Scan the mask row by row (along u).  When we find a valid, un-merged
//     cell, extend as far right (u) as possible while keys match, then
//     extend downward (v) for the same width.  Emit one quad for the whole
//     rectangle and mark those cells as consumed (block = nullptr).
// ---------------------------------------------------------------------------
ChunkMesh ChunkMesher::build(const Chunk& chunk, const ChunkCoord& coord,
                             const NeighbourFn& neighbour,
                             const CoverClimate& climate,
                             const NeighbourFn& generatedWaterHalo,
                             const WaterColumnFn& waterColumn) const {
    ChunkMesh mesh;

    // World-space origin of this chunk (tile units).
    const int originX = coord.x * Chunk::SIZE_X;
    const int originY = coord.y * Chunk::SIZE_Y;
    const int originZ = coord.z * Chunk::SIZE_Z;

    // Chunk dimensions indexed as [d0][d1][d2].
    // We re-interpret axes below, but we need a way to address tiles by
    // any permutation of (x, y, z).  We work entirely in world tile coords
    // and call neighbour() for adjacency — that handles cross-chunk borders.

    // Axis loop: d = primary (normal) axis; u,v = the two tangent axes.
    // dims[0] = SIZE_XY (x), dims[1] = SIZE_XY (y/z), dims[2] = SIZE_Z (z/y)
    // We express everything in world coordinates to keep the neighbour()
    // call simple.

    // World-space extents
    const int W = Chunk::SIZE_X;
    const int H = Chunk::SIZE_Y;
    const int D = Chunk::SIZE_Z;

    const int dims[3] = {W, H, D};

    auto getNeighbourTile = [&](int x, int y, int z) -> const Tile* {
        // inside current chunk
        if ((unsigned)x < W && (unsigned)y < H && (unsigned)z < D) {
            return &chunk.tiles[z][y][x];
        }

        // outside chunk -> fallback
        return neighbour(originX + x, originY + y, originZ + z);
    };

    // For each of the 3 axes we sweep slabs perpendicular to that axis.
    for (int d = 0; d < 3; ++d) {
        // The two tangent axis indices
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        const int slabCount = dims[d];
        const int uCount = dims[u];
        const int vCount = dims[v];

        // face mask for this slab: [v][u], two sides (+d/-d)
        std::vector<FaceKey> maskPos(uCount * vCount);
        std::vector<FaceKey> maskNeg(uCount * vCount);

        // Helper: world position from (d_val, u_val, v_val) given axis d
        auto worldPos = [&](int dVal, int uVal, int vVal, int out[3]) {
            out[d] = dVal;
            out[u] = uVal;
            out[v] = vVal;
            // out is now [wx_offset, wy_offset, wz_offset] relative to origin
        };

        auto localTile = [&](int x, int y, int z) -> const Tile* {
            if ((unsigned)x >= W || (unsigned)y >= H || (unsigned)z >= D)
                return nullptr;

            return &chunk.tiles[z][y][x];
        };

        for (int s = 0; s < slabCount; ++s) {
            // Build the face masks for this slab
            for (int vi = 0; vi < vCount; ++vi) {
                for (int ui = 0; ui < uCount; ++ui) {
                    // Compute world tile coordinates of the current cell
                    int local[3];
                    worldPos(s, ui, vi, local);

                    // Retrieve the tile at this world position via neighbour()
                    const Tile* tile = localTile(local[0], local[1], local[2]);
                    if (!tile || tile->block_id == 0) continue;

                    const Block& block = blocks.get(tile->block_id);
                    uint16_t texLayer =
                        assets.getMaterialAlbedoLayer(block.material);

                    // Light is sampled from the chunk's light volume in the
                    // shader, so faces merge regardless of light levels.
                    FaceKey key{&block, texLayer, NeutralTint};
                    // Ground cover shows on the top face and, when enabled,
                    // on side faces; never on the bottom.
                    const CoverInfo* cover = coverInfo(tile->cover);
                    const auto covered = [&](bool posDir) {
                        FaceKey k = key;
                        if (cover &&
                            (d == 1 ? posDir && cover->top : cover->sides))
                            k.cover = tile->cover;
                        return k;
                    };

                    // +d neighbour
                    {
                        int nlocal[3] = {local[0], local[1], local[2]};
                        nlocal[d] += 1;

                        const Tile* nbr =
                            getNeighbourTile(nlocal[0], nlocal[1], nlocal[2]);

                        if (isFaceVisible(nbr))
                            maskPos[vi * uCount + ui] = covered(true);
                    }

                    // -d neighbour
                    {
                        int nlocal[3] = {local[0], local[1], local[2]};
                        nlocal[d] -= 1;

                        const Tile* nbr =
                            getNeighbourTile(nlocal[0], nlocal[1], nlocal[2]);

                        if (isFaceVisible(nbr))
                            maskNeg[vi * uCount + ui] = covered(false);
                    }
                }
            }

            // ---------------------------------------------------------------
            // Greedy merge pass — run once for +d, once for -d
            // ---------------------------------------------------------------
            auto greedyPass = [&](std::vector<FaceKey>& mask, bool posDir) {
                // Face enum index: axis d, direction posDir
                // Axis 0: PosX=0, NegX=1 | Axis 1: PosY=2, NegY=3 | Axis 2:
                // PosZ=4, NegZ=5
                Face faceEnum = static_cast<Face>(d * 2 + (posDir ? 0 : 1));

                for (int vi = 0; vi < vCount; ++vi) {
                    for (int ui = 0; ui < uCount;) {
                        const FaceKey& startKey = mask[vi * uCount + ui];
                        if (!startKey.valid()) {
                            ++ui;
                            continue;
                        }

                        // Extend in the u direction
                        int w = 1;
                        while (ui + w < uCount &&
                               mask[vi * uCount + ui + w] == startKey)
                            ++w;

                        // Extend in the v direction
                        int h = 1;
                        bool canExtend = true;
                        while (vi + h < vCount && canExtend) {
                            for (int k = 0; k < w; ++k) {
                                if (!(mask[(vi + h) * uCount + ui + k] ==
                                      startKey)) {
                                    canExtend = false;
                                    break;
                                }
                            }
                            if (canExtend) ++h;
                        }

                        // ------------------------------------------------
                        // Compute world-space origin and edge vectors for
                        // the merged quad.
                        //
                        // The quad lives on the face of the slab at depth s.
                        // For +d face the surface is at s+1; for -d at s.
                        // ------------------------------------------------
                        float qOrigin[3], qDu[3], qDv[3];

                        // Start corner in world space (float)
                        {
                            int cell[3];
                            worldPos(s, ui, vi, cell);
                            // Chunk-local: the draw transform adds the origin.
                            qOrigin[0] = (float)cell[0];
                            qOrigin[1] = (float)cell[1];
                            qOrigin[2] = (float)cell[2];

                            // For the +d face, push the surface out by 1 tile
                            if (posDir) qOrigin[d] += 1.0f;
                        }

                        // Edge vectors: w tiles along u, h tiles along v
                        qDu[0] = qDu[1] = qDu[2] = 0.f;
                        qDu[u] = (float)w;
                        qDv[0] = qDv[1] = qDv[2] = 0.f;
                        qDv[v] = (float)h;

                        // For -d face, flip the u edge so winding stays CCW
                        // when viewed from outside (same convention as FACES).
                        if (!posDir) {
                            qDu[u] = -(float)w;
                            qOrigin[u] += (float)
                                w;  // shift origin to keep quad in bounds
                        }

                        emitQuad(mesh, qOrigin[0], qOrigin[1], qOrigin[2],
                                 qDu[0], qDu[1], qDu[2], qDv[0], qDv[1], qDv[2],
                                 faceEnum, startKey, climate);

                        // Mark merged cells as consumed
                        for (int dv = 0; dv < h; ++dv)
                            for (int du = 0; du < w; ++du)
                                mask[(vi + dv) * uCount + ui + du] = {};

                        ui += w;
                    }
                }
            };

            greedyPass(maskPos, true);
            greedyPass(maskNeg, false);

            // Clear masks for next slab
            std::fill(maskPos.begin(), maskPos.end(), FaceKey{});
            std::fill(maskNeg.begin(), maskNeg.end(), FaceKey{});
        }
    }

    const auto emitWater = [&](float x, float y, float z, float ux, float uy,
                               float uz, float vx, float vy, float vz,
                               Face face, Vec2 flow, bool movingRiver = true) {
        const size_t base = mesh.water.vertices.size();
        emitQuad(mesh.water, x, y, z, ux, uy, uz, vx, vy, vz, face,
                 {nullptr, UINT16_MAX, NeutralTint}, climate);
        if (flow.x || flow.y)
            for (size_t i = base; i < mesh.water.vertices.size(); ++i) {
                auto& vertex = mesh.water.vertices[i];
                vertex.layer = movingRiver ? -3 : -4;
                vertex.u = flow.x;
                vertex.v = flow.y;
            }
    };
    static constexpr std::array<Vec2i, 4> sides{
        {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    static constexpr std::array<Face, 4> faces{Face::PosX, Face::NegX,
                                               Face::PosZ, Face::NegZ};
    for (int z = 0; z < Chunk::SIZE_Z; ++z)
        for (int x = 0; x < Chunk::SIZE_X; ++x) {
            const int wx = originX + x, wz = originZ + z;
            const auto at = [&](int dx, int y, int dz) -> const Tile* {
                const int xx = x + dx, zz = z + dz;
                if (xx >= 0 && xx < Chunk::SIZE_X && zz >= 0 &&
                    zz < Chunk::SIZE_Z && y >= 0 && y < Chunk::SIZE_Y)
                    return &chunk.tiles[zz][y][xx];
                return neighbour(wx + dx, originY + y, wz + dz);
            };
            std::pair<int, WaterSurface> generated{-1, {}};
            std::array<std::pair<int, WaterSurface>, 4> adjacent;
            if (waterColumn) generated = waterColumn(wx, wz);
            bool sampled = false;
            for (int y = 0; y < Chunk::SIZE_Y; ++y) {
                const auto& tile = chunk.tiles[z][y][x];
                const bool fluid =
                    tile.liquid == LiquidType::Water && !tile.block_id;
                const auto [ground, water] = generated;
                const bool river =
                    water.reach && tile.waterOrigin != WaterOrigin::Local;
                const int wy = originY + y;
                // A generated sheet can run down the lip's exposed rock
                // underneath the upstream water cells.
                const bool cliff = water.reach && wy <= ground &&
                                   wy < water.surfaceY && tile.block_id;
                if (!fluid && !cliff) continue;
                if (tile.waterOrigin != WaterOrigin::Reservoir || cliff)
                    mesh.lakeSurface.mixed = true;
                else
                    mesh.lakeSurface.include(water, wx, wz);
                if (waterColumn && !sampled) {
                    for (size_t i = 0; i < sides.size(); ++i)
                        adjacent[i] =
                            waterColumn(wx + sides[i].x, wz + sides[i].y);
                    sampled = true;
                }
                const float height = fluid ? WaterSurface::cellHeight(tile) : 1;
                const Tile* above = at(0, y + 1, 0);
                if (!above && generatedWaterHalo)
                    above = generatedWaterHalo(wx, originY + y + 1, wz);
                // Adjacent free surfaces share corner heights. A thin local
                // film would otherwise draw a grid of vertical faces between
                // cells differing by just one unit.
                const bool localTop =
                    sampled && tile.waterOrigin == WaterOrigin::Local &&
                    above && above->isAir() && water.topTile() == wy &&
                    !water.body && !water.reach && water.edited;
                const auto joinsLocal = [&](const WaterSurface& s) {
                    return localTop && s.edited && !s.body && !s.reach &&
                           s.topTile() == wy;
                };
                std::array<float, 4> cornerHeight;
                cornerHeight.fill(height);
                if (localTop) {
                    for (int corner = 0; corner < 4; ++corner) {
                        const int dx = (corner & 1) ? 1 : -1;
                        const int dz = (corner & 2) ? 1 : -1;
                        float sum = height;
                        int count = 1;
                        for (const auto& s :
                             {adjacent[dx > 0 ? 0 : 1].second,
                              adjacent[dz > 0 ? 2 : 3].second,
                              waterColumn(wx + dx, wz + dz).second}) {
                            if (joinsLocal(s)) {
                                sum += s.topFill / 255.f;
                                ++count;
                            }
                        }
                        cornerHeight[corner] = sum / count;
                    }
                }
                const auto topAt = [&](const BlockVertex& v) {
                    return y + cornerHeight[int(v.x > x + .5f) +
                                            2 * int(v.z > z + .5f)];
                };
                if (fluid && above && above->liquid == LiquidType::None &&
                    !above->block_id) {
                    const float top = y + height;
                    if (mesh.waterTops.empty())
                        mesh.waterTops.resize(Chunk::SIZE_X * Chunk::SIZE_Z,
                                              -1);
                    mesh.waterTops[z * Chunk::SIZE_X + x] = top;
                    emitWater(x, top, z, 0, 0, 1, 1, 0, 0, Face::PosY,
                              tile.waterOrigin == WaterOrigin::Local
                                  ? Vec2{}
                                  : water.flow,
                              river);
                    float depth = height;
                    for (int h = y - 1; depth < 6 && h >= -1; --h) {
                        const auto* lower = at(0, h, 0);
                        if (!lower || lower->liquid != LiquidType::Water) break;
                        depth += 1;
                    }
                    if (tile.waterOrigin == WaterOrigin::Reservoir &&
                        water.wet())
                        depth = water.height() - ground - 1;
                    for (auto& vertex :
                         std::span(mesh.water.vertices).last(4)) {
                        if (localTop) vertex.y = topAt(vertex);
                        vertex.tint = WaterSurface::depthTint(depth);
                        if (!sampled) continue;
                        // Red encodes distance from the edge hit by a fall.
                        // Interpolation fades the churn across the landing
                        // tile.
                        for (size_t i = 0; i < sides.size(); ++i) {
                            const auto& upstream = adjacent[i].second;
                            const auto [dx, dz] = sides[i];
                            if (upstream.height() < wy + height + 1 ||
                                upstream.flow.x * dx + upstream.flow.y * dz >=
                                    0)
                                continue;
                            if ((dx && (vertex.x > x) == (dx > 0)) ||
                                (dz && (vertex.z > z) == (dz > 0)))
                                vertex.tint &= 0xffffff00u;
                        }
                    }
                    // Four neighboring channel centers share a corner. Keep
                    // distinct waterfall lips and detached water levels sharp.
                    if (water.reach && wy == water.topTile() &&
                        tile.waterOrigin != WaterOrigin::Local) {
                        for (auto& vertex :
                             std::span(mesh.water.vertices).last(4)) {
                            const int dx = vertex.x > x ? 1 : -1;
                            const int dz = vertex.z > z ? 1 : -1;
                            float sum = 0;
                            int count = 1;
                            for (const auto& sample :
                                 {adjacent[dx > 0 ? 0 : 1].second,
                                  adjacent[dz > 0 ? 2 : 3].second,
                                  waterColumn(wx + dx, wz + dz).second}) {
                                const float diff =
                                    sample.height() - water.height();
                                if (sample.reach == water.reach &&
                                    std::abs(diff) < 1.f) {
                                    sum += diff;
                                    ++count;
                                }
                            }
                            vertex.y += sum / count;
                        }
                    }
                }

                if (!sampled) continue;
                for (size_t i = 0; i < sides.size(); ++i) {
                    const auto [dx, dz] = sides[i];
                    const auto [bank, lower] = adjacent[i];
                    const Tile* other = at(dx, y, dz);
                    if (other && (other->block_id ||
                                  (other->waterOrigin == WaterOrigin::Local &&
                                   joinsLocal(lower))))
                        continue;
                    const bool drop = water.wet() && wy < water.surfaceY &&
                                      wy >= water.dropBottom(lower, bank);
                    // Any air beside a water cell exposes a face: a lower
                    // lake, a drained neighbor or a dug pocket. Liquid
                    // neighbors only need the part above their own fill.
                    if (!drop && !(fluid && other)) continue;
                    const float bottom = other && other->isLiquid()
                                             ? WaterSurface::cellHeight(*other)
                                             : 0;
                    if (bottom >= height) continue;
                    float sheetBottom = bottom;
                    if (fluid && tile.waterOrigin == WaterOrigin::Local &&
                        tile.liquidLevel >= 20 && other && other->isAir() &&
                        above && above->isAir()) {
                        // Trace the exposed face beneath a dug spillway until
                        // the neighboring column meets a ledge or tailwater.
                        const auto* support = at(0, y - 1, 0);
                        if (support && support->isSolid()) {
                            for (int h = y - 1; originY + h >= 0; --h) {
                                const auto* falling = at(dx, h, dz);
                                if (!falling || falling->isSolid()) break;
                                sheetBottom = float(h - y);
                                if (falling->isLiquid()) {
                                    sheetBottom +=
                                        WaterSurface::cellHeight(*falling);
                                    break;
                                }
                            }
                        }
                    }
                    const float length = height - sheetBottom;
                    const Vec2 flow =
                        water.reach ? water.flow : Vec2(float(dx), float(dz));
                    // Match the outward winding of FACES.
                    // Move the sheet just in front of the solid face.
                    const float offset = cliff && drop ? 2 * SeamOverlap : 0.f;
                    emitWater(x + (dx > 0 || dz > 0) + dx * offset,
                              y + sheetBottom,
                              z + (dz > 0 || dx < 0) + dz * offset, 0, length,
                              0, -dz, 0, dx, faces[i], flow, river);
                    if (localTop) {
                        auto side = std::span(mesh.water.vertices).last(4);
                        side[1].y = topAt(side[1]);
                        side[2].y = topAt(side[2]);
                    }
                }
            }
        }

    return mesh;
}
