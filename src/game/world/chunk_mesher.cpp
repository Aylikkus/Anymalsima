#include "chunk_mesher.hpp"

#include <cmath>
#include <span>
#include <unordered_map>

#include "items/block.hpp"
#include "items/material.hpp"
#include "structure_voxels.hpp"

PieceProvenance ChunkMeshSnapshot::legacyOrigin(
    uint16_t index, ShapedTile cell, const structure_voxels::Slab* structures,
    const GrownBlockSet& grown) {
    if (cell.tile.isSolid() &&
        (grown.test(cell.tile.block_id) ||
         (structures && structures->tileAt(index) == cell.tile.block_id &&
          structures->shapeAt(index) == cell.shape)))
        return {PieceOrigin::Structure};
    return {};
}

SolidCellSnapshot ChunkMeshSnapshot::solidCellAt(uint16_t index) const {
    const auto base = chunk->cellAt(index % Chunk::SIZE_X,
                                    (index / Chunk::SIZE_X) % Chunk::SIZE_Y,
                                    index / (Chunk::SIZE_X * Chunk::SIZE_Y));
    auto result = chunk->solidCellAt(
        index, legacyOrigin(index, base, structures.get(), grown));
    if (environment) {
        const auto found = environment->find(index);
        if (found != environment->end()) result.environment = found->second;
    }
    return result;
}

std::optional<SolidCellSnapshot> ChunkMesher::Neighbour::solidCell() const {
    if (snapshot) return snapshot->solidCellAt(index);
    if (chunk) {
        auto result = chunk->solidCellAt(index);
        result.environment = environment;
        return result;
    }
    if (!tile) return std::nullopt;
    if (!tile->isSolid() && !environment)
        return SolidCellSnapshot{{*tile, shape}};
    SolidCellSnapshot result{{*tile, shape}};
    result.environment = environment;
    if (tile->isSolid())
        result.simple = CellPiece{{0, tile->block_id, shape}, {}};
    return result;
}

namespace {

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

Face opposite(Face face) { return Face(uint8_t(face) ^ 1); }

// Intersect a convex polygon with the half planes of its occluder in turn.
// Each rejected piece is visible and disjoint from the other pieces. Keeping
// the intersections in 3D preserves the original winding on every axis.
template <typename Emit>
void subtractCoverage(const tile_geometry::Polygon& face,
                      const tile_geometry::Polygon* occluder, Emit emit) {
    if (!occluder) {
        emit(std::span(face.vertices).first(face.count));
        return;
    }
    // A convex polygon gains at most one vertex per half-plane clip.
    struct Piece {
        std::array<Vec3, 8> points{};
        unsigned size = 0;
        void add(Vec3 point) { points[size++] = point; }
    } remaining;
    for (unsigned i = 0; i < face.count; ++i) remaining.add(face.vertices[i]);
    const auto normal = face.plane.normal;
    constexpr float epsilon = 1e-6f;
    for (unsigned i = 0; i < occluder->count && remaining.size >= 3; ++i) {
        const auto start = occluder->vertices[i];
        const auto end = occluder->vertices[(i + 1) % occluder->count];
        // The neighbour winds in the opposite direction to this face.
        const auto inward = cross(end - start, normal);
        const auto distance = [&](Vec3 point) {
            const float d = dot(point - start, inward);
            return std::abs(d) < epsilon ? 0.f : d;
        };
        Piece inside, outside;
        auto prev = remaining.points[remaining.size - 1];
        float prevDist = distance(prev);
        for (unsigned j = 0; j < remaining.size; ++j) {
            const auto point = remaining.points[j];
            const float dist = distance(point);
            if ((prevDist < 0 && dist > 0) || (prevDist > 0 && dist < 0)) {
                const auto cut =
                    prev + (point - prev) * (prevDist / (prevDist - dist));
                inside.add(cut);
                outside.add(cut);
            }
            if (dist >= 0) inside.add(point);
            if (dist <= 0) outside.add(point);
            prev = point;
            prevDist = dist;
        }
        emit(std::span(outside.points).first(outside.size));
        remaining = inside;
    }
}

}  // namespace

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

void ChunkMesher::emitSurface(ChunkGeometry& mesh,
                              const tile_geometry::Polygon& face,
                              const tile_geometry::Polygon* occluder, Vec3 cell,
                              TileShape shape, const FaceKey& key,
                              const CoverClimate& climate) const {
    subtractCoverage(face, occluder, [&](std::span<const Vec3> polygon) {
        emitPolygon(mesh, polygon, face.plane.normal,
                    !tile_geometry::boundaryFace(face), cell, shape, key,
                    climate);
    });
}

void ChunkMesher::emitPolygon(ChunkGeometry& mesh,
                              std::span<const Vec3> polygon, Vec3 normal,
                              bool inset, Vec3 cell, TileShape shape,
                              const FaceKey& key,
                              const CoverClimate& climate) const {
    const auto* cover = coverInfo(key.cover);
    static_assert(Chunk::SIZE_X == 16 && Chunk::SIZE_Y == 16 &&
                  Chunk::SIZE_Z == 16);  // block.vert decodes the owner index
    if (polygon.size() < 3) return;
    const auto base = uint32_t(mesh.vertices.size());
    const auto firstIndex = mesh.indices.size();
    for (unsigned i = 1; i + 1 < polygon.size(); ++i) {
        const auto area =
            cross(polygon[i] - polygon[0], polygon[i + 1] - polygon[0]);
        if (dot(area, normal) > 1e-7f)
            mesh.indices.insert(mesh.indices.end(),
                                {base, base + i, base + i + 1});
    }
    if (mesh.indices.size() == firstIndex) return;
    for (const auto point : polygon) {
        const auto pos = cell + point;
        BlockVertex vertex{pos.x,    pos.y,    pos.z, normal.x,
                           normal.y, normal.z, 0,     0,
                           0,        -1,       0,     float(key.texLayer),
                           key.tint};
        // The block shader uses a tagged tangent slot for the owner
        // index and sloping side's rim depth, with no extra vertex bytes.
        vertex.tx = float(BlockVertex::ExplicitCellTag +
                          Chunk::cell(int(cell.x), int(cell.y), int(cell.z)));
        vertex.tz = inset ? 1.f : 0.f;
        if (cover) {
            vertex.u = cover->layer;
            if (normal.y == 0) {
                const auto top = tile_geometry::supportHeight(
                    shape, std::clamp(point.x, 0.f, 1.f),
                    std::clamp(point.z, 0.f, 1.f));
                vertex.ty = std::max(0.f, top.value_or(1.f) - point.y);
                vertex.v = cover->fringe;
            }
            const auto [humidity, temperature] =
                climate.at(pos.x / Chunk::SIZE_X, pos.z / Chunk::SIZE_Z);
            vertex.tint = cover->tint.at(humidity, temperature).packed();
        }
        mesh.vertices.push_back(vertex);
    }
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
                             const WaterHaloFn& generatedWaterHalo,
                             const WaterColumnFn& waterColumn,
                             const ChunkEnvironment* environment) const {
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

    const bool hasShapes = chunk.hasShapes();
    const bool hasCompounds = chunk.hasCompounds();
    // A dense cell lookup keeps the greedy sweep and border clipping from
    // binary-searching the sorted sparse table for every visited cell. Only
    // chunks that actually carry shapes pay for it.
    std::array<TileShape, Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z>
        shapeGrid{};
    if (hasShapes)
        chunk.forEachShape(
            [&](uint16_t index, TileShape shape) { shapeGrid[index] = shape; });
    const auto localShape = [&](int x, int y, int z) {
        return hasShapes ? shapeGrid[Chunk::cell(x, y, z)] : TileShape{};
    };
    const auto localEnvironment = [&](uint16_t index) -> EnvironmentCell {
        if (!environment) return {};
        const auto found = environment->find(index);
        return found == environment->end() ? EnvironmentCell{} : found->second;
    };
    const auto getNeighbour = [&](int x, int y, int z) -> Neighbour {
        // inside current chunk
        if ((unsigned)x < W && (unsigned)y < H && (unsigned)z < D) {
            const auto index = Chunk::cell(x, y, z);
            const auto env = localEnvironment(index);
            if (env || (hasCompounds && chunk.hasCompoundAt(index)))
                return {chunk, x, y, z, env};
            return {&chunk.tiles[z][y][x], localShape(x, y, z)};
        }

        // outside chunk -> fallback
        return neighbour(originX + x, originY + y, originZ + z);
    };

    const auto keyFor = [&](const Tile& tile, const Block& block, float ny) {
        FaceKey key{&block, assets.getMaterialAlbedoLayer(block.material),
                    NeutralTint};
        if (const auto* cover = coverInfo(tile.cover);
            cover && (ny > 0 ? cover->top : ny == 0 && cover->sides))
            key.cover = tile.cover;
        return key;
    };

    // The same compound geometry supplies its own faces and the border
    // clipping of every adjacent tile in this build.
    std::unordered_map<const Tile*, tile_geometry::CellGeometry>
        compoundGeometry;
    const auto geometryOf = [&](const Tile* tile, auto solid) -> const auto& {
        auto [it, inserted] = compoundGeometry.try_emplace(tile);
        if (inserted) it->second = solid().geometry();
        return it->second;
    };
    // Deep packs share the geometry of their whole cells across slabs.
    // Test each shared volume once before clipping faces against it.
    std::unordered_map<const tile_geometry::CellGeometry*, bool>
        fullEnvironments;
    const auto fillsCell = [&](const EnvironmentCell& geometry) {
        if (!geometry) return false;
        auto [it, inserted] = fullEnvironments.try_emplace(geometry.get());
        if (inserted) it->second = geometry->isFull();
        return it->second;
    };

    // Clip a boundary against the actual open polygons of its neighbour.
    const auto emitBoundary = [&](std::span<const Vec3> polygon, Face face,
                                  Neighbour neighbour, Vec3 cell,
                                  TileShape shape, const FaceKey& key) {
        if (!neighbour.tile) return;
        const auto normal = tile_geometry::boundaryFace({}, face)->plane.normal;
        if (!neighbour.hasSolid()) {
            emitPolygon(mesh, polygon, normal, false, cell, shape, key,
                        climate);
            return;
        }
        if (fillsCell(neighbour.environment)) return;
        if (!neighbour.isCompound() && !neighbour.shape.code()) return;
        const auto openings =
            neighbour.isCompound()
                ? geometryOf(neighbour.tile,
                             [&] { return *neighbour.solidCell(); })
                      .boundaryOpenings(opposite(face))
                : neighbour.solidCell()->geometry().boundaryOpenings(
                      opposite(face));
        for (auto opening : openings) {
            for (auto& point : opening) point = point + normal;
            const auto visible =
                tile_geometry::clipToFace(polygon, opening, normal * -1);
            emitPolygon(mesh, visible, normal, false, cell, shape, key,
                        climate);
        }
    };
    std::vector<uint16_t> compoundCells;
    if (hasCompounds)
        chunk.forEachPieceCell(
            [&](uint16_t index, const SolidCellSnapshot& solid) {
                if (solid.compound) compoundCells.push_back(index);
            });
    if (environment)
        for (const auto& [index, geometry] : *environment) {
            (void)geometry;
            compoundCells.push_back(index);
        }
    std::sort(compoundCells.begin(), compoundCells.end());
    compoundCells.erase(std::unique(compoundCells.begin(), compoundCells.end()),
                        compoundCells.end());
    for (const uint16_t index : compoundCells) {
        const int x = index % W, y = (index / W) % H;
        const int z = index / (W * H);
        auto solid = chunk.solidCellAt(index);
        solid.environment = localEnvironment(index);
        if (solid.pieces().empty() && !solid.hasEnvironment()) continue;
        if (fillsCell(solid.environment)) {
            const auto covered = [&](int dx, int dy, int dz) {
                const auto adjacent = getNeighbour(x + dx, y + dy, z + dz);
                return fillsCell(adjacent.environment) ||
                       (adjacent.hasSolid() && !adjacent.isCompound() &&
                        !adjacent.shape.code());
            };
            if (covered(-1, 0, 0) && covered(1, 0, 0) && covered(0, -1, 0) &&
                covered(0, 1, 0) && covered(0, 0, -1) && covered(0, 0, 1))
                continue;
        }
        const Vec3 cell{float(x), float(y), float(z)};
        const tile_geometry::TilePiece* first =
            solid.pieces().empty() ? nullptr : &solid.pieces().front().geometry;
        const auto legacy = first ? first->legacyShape() : std::nullopt;
        for (const auto& polygon : solid.geometry().exteriorFaces()) {
            const auto normal = polygon.plane.normal;
            const auto& block = blocks.get(polygon.blockId);
            const bool covered = first && legacy && polygon.id == first->id;
            const auto key =
                keyFor(covered ? solid.base.tile : Tile{}, block, normal.y);
            const auto shape = covered ? *legacy : TileShape{};
            if (const auto face = tile_geometry::boundaryFace(polygon.plane))
                emitBoundary(polygon.vertices, *face,
                             getNeighbour(x + int(normal.x), y + int(normal.y),
                                          z + int(normal.z)),
                             cell, shape, key);
            else
                emitPolygon(mesh, polygon.vertices, normal, true, cell, shape,
                            key, climate);
        }
    }

    // Emit each shaped cell once. Inset surfaces have no shared boundary;
    // boundary surfaces are clipped by the occupied part of the neighbour.
    chunk.forEachShape([&](uint16_t index, TileShape shape) {
        if (hasCompounds && chunk.hasCompoundAt(index)) return;
        if (localEnvironment(index)) return;
        const int x = index % W, y = (index / W) % H;
        const int z = index / (W * H);
        const auto& tile = chunk.tiles[z][y][x];
        const auto& block = blocks.get(tile.block_id);
        const Vec3 cell{float(x), float(y), float(z)};
        const auto surface = tile_geometry::surface(shape);
        for (const auto& polygon :
             std::span(surface.faces).first(surface.count)) {
            const auto normal = polygon.plane.normal;
            std::optional<tile_geometry::Polygon> occluder;
            if (const auto face = tile_geometry::boundaryFace(polygon)) {
                const int axis = uint8_t(*face) / 2;
                int adjacent[] = {x, y, z};
                const int step = uint8_t(*face) % 2 ? -1 : 1;
                adjacent[axis] += step;
                const auto nbr =
                    getNeighbour(adjacent[0], adjacent[1], adjacent[2]);
                if (!nbr.tile) continue;
                if (nbr.isCompound()) {
                    emitBoundary(
                        std::span(polygon.vertices).first(polygon.count), *face,
                        nbr, cell, shape, keyFor(tile, block, normal.y));
                    continue;
                }
                if (nbr.tile->isSolid()) {
                    if (!nbr.shape.code()) continue;
                    occluder =
                        tile_geometry::boundaryFace(nbr.shape, opposite(*face));
                    Vec3 shift{};
                    if (axis == 0) shift.x = float(step);
                    if (axis == 1) shift.y = float(step);
                    if (axis == 2) shift.z = float(step);
                    if (occluder)
                        for (auto& point : occluder->vertices)
                            point = point + shift;
                }
            }
            emitSurface(mesh, polygon, occluder ? &*occluder : nullptr, cell,
                        shape, keyFor(tile, block, normal.y), climate);
        }
    });

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
                    if (!tile || tile->block_id == 0 ||
                        (hasCompounds && chunk.hasCompoundAt(Chunk::cell(
                                             local[0], local[1], local[2]))) ||
                        (hasShapes &&
                         localShape(local[0], local[1], local[2]).code()) ||
                        localEnvironment(
                            Chunk::cell(local[0], local[1], local[2])))
                        continue;

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

                    const auto faceKey = [&](bool posDir) -> FaceKey {
                        int nlocal[3] = {local[0], local[1], local[2]};
                        nlocal[d] += posDir ? 1 : -1;
                        const auto nbr =
                            getNeighbour(nlocal[0], nlocal[1], nlocal[2]);
                        if (!nbr.tile) return {};
                        if (!nbr.hasSolid()) return covered(posDir);
                        const auto face = Face(d * 2 + (posDir ? 0 : 1));
                        if (nbr.isCompound()) {
                            const auto square =
                                tile_geometry::boundaryFace({}, face);
                            emitBoundary(square->vertices, face, nbr,
                                         {float(local[0]), float(local[1]),
                                          float(local[2])},
                                         {}, covered(posDir));
                            return {};
                        }
                        if (!nbr.tile->isSolid() || !nbr.shape.code())
                            return {};
                        auto occluder = tile_geometry::boundaryFace(
                            nbr.shape, opposite(face));
                        if (!occluder) return covered(posDir);
                        const auto square =
                            tile_geometry::boundaryFace({}, face);
                        for (auto& point : occluder->vertices)
                            point = point + square->plane.normal;
                        const Vec3 cell{float(local[0]), float(local[1]),
                                        float(local[2])};
                        emitSurface(mesh, *square, &*occluder, cell, {},
                                    covered(posDir), climate);
                        return {};
                    };
                    maskPos[vi * uCount + ui] = faceKey(true);
                    maskNeg[vi * uCount + ui] = faceKey(false);
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
                return neighbour(wx + dx, originY + y, wz + dz).tile;
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
                    if (cliff && (!drop || !water.fallsToward(lower, dx, dz)))
                        continue;
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
