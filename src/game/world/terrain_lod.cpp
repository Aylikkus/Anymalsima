#include "terrain_lod.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace TerrainLod {
namespace {
float distance(const Patch& p, Vec3 eye) {
    return std::hypot(
        std::max({float(p.x) - eye.x, 0.f, eye.x - p.x - p.size()}),
        std::max({float(p.z) - eye.z, 0.f, eye.z - p.z - p.size()}));
}
void children(const Patch& p, const std::function<void(Patch)>& fn) {
    for (const auto& child : TerrainLod::children(p)) fn(child);
}
struct Point {
    float x, y, z;
};
Point terrainPoint(const Patch&, SurfaceSampler& sampler, float x, float z) {
    return {x, float(sampler.height(int(x), int(z)) + 1), z};
}
int channelSpacing(const Patch& p) {
    if (p.spacing >= RootSpacing) return 8;
    if (p.spacing >= 16) return 4;
    return p.spacing >= 4 ? 2 : 1;
}

Point riverTerrainPoint(const Patch& p, SurfaceSampler& sampler, float x,
                        float z) {
    auto point = terrainPoint(p, sampler, x, z);
    const int level =
        sampler.channelFloorLevel(int(x), int(z), channelSpacing(p));
    if (level > 0)
        point.y = std::clamp(float(level - 1), point.y - 96.f, point.y);
    return point;
}
// Corners of one tessellation cell, counter-clockwise from (x, z).
std::array<Point, 4> terrainRing(const Patch& p, SurfaceSampler& sampler,
                                 float x, float z, float x1, float z1) {
    return {terrainPoint(p, sampler, x, z), terrainPoint(p, sampler, x, z1),
            terrainPoint(p, sampler, x1, z1), terrainPoint(p, sampler, x1, z)};
}
// top >= 0: the world Y of the column top above a stepped wall (fringe of
// ground cover); -1 elsewhere. Stored in u, which the block program reads as
// "no cover" when negative. The terrain program reads v as the edge kind:
// 0 ordinary face, 1 smooth skirt, 2 stepped boundary wall.
void triangle(Geometry& mesh, Point a, Point b, Point c, float layer,
              float top = -1, float edgeKind = 0, uint32_t tint = 0xffffffffu) {
    const Point u{b.x - a.x, b.y - a.y, b.z - a.z};
    const Point v{c.x - a.x, c.y - a.y, c.z - a.z};
    Point n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
            u.x * v.y - u.y * v.x};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < .00001f) return;
    n = {n.x / length, n.y / length, n.z / length};
    const auto base = uint32_t(mesh.vertices.size());
    for (const auto& p : {a, b, c}) {
        mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, 1, 0, 0, top,
                                 edgeKind, layer, tint});
        mesh.bounds.minY = std::min(mesh.bounds.minY, p.y);
        mesh.bounds.maxY = std::max(mesh.bounds.maxY, p.y);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}
void quad(Geometry& mesh, Point a, Point b, Point c, Point d, float layer,
          float top = -1, float edgeKind = 0, uint32_t tint = 0xffffffffu) {
    triangle(mesh, a, b, c, layer, top, edgeKind, tint);
    triangle(mesh, a, c, d, layer, top, edgeKind, tint);
}
void waterTile(Mesh& patch, SurfaceSampler& sampler, int x, int z,
               WaterSurface water, bool wrapX, bool top = true) {
    if (!water.wet()) return;
    patch.lakeSurface.include(water, x, z);
    auto& mesh = patch.water;
    const float a = x, b = x + 1, c = z, d = z + 1;
    const float y = water.height();
    if (top)
        quad(mesh, {a, y, c}, {a, y, d}, {b, y, d}, {b, y, c},
             (water.flow.x || water.flow.y) ? (water.reach ? -3 : -4) : -2,
             (water.flow.x || water.flow.y) ? water.flow.x : -1, water.flow.y,
             WaterSurface::depthTint(y - sampler.height(x, z) - 1));
    // Sides wherever the neighbor stands lower: river steps, edits and two
    // standing bodies at different levels. A natural shore is higher ground.
    for (const auto& [dx, dz] :
         std::array<Vec2i, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
        if (z + dz < 0 || z + dz >= sampler.depth() ||
            (!wrapX && (x + dx < 0 || x + dx >= sampler.width())))
            continue;
        const auto other = sampler.waterAt(x + dx, z + dz);
        if (other.height() >= water.height()) continue;
        const float bottom =
            std::max(other.height(), float(sampler.height(x + dx, z + dz) + 1));
        if (bottom >= y) continue;
        const Vec2 flow = water.reach ? water.flow : Vec2(float(dx), float(dz));
        const float sx = a + (dx > 0 || dz > 0);
        const float sz = c + (dz > 0 || dx < 0);
        quad(mesh, {sx, bottom, sz}, {sx, y, sz}, {sx - dz, y, sz + dx},
             {sx - dz, bottom, sz + dx}, water.reach ? -3 : -4, flow.x, flow.y);
    }
}

void channelWater(Mesh& patch, SurfaceSampler& sampler, int x, int z, int x1,
                  int z1, bool wrapX) {
    constexpr int Side = 8;
    std::array<WaterSurface, Side * Side> samples;
    const auto at = [&](int xx, int zz) -> WaterSurface& {
        return samples[(zz - z) * Side + xx - x];
    };
    for (int zz = z; zz < z1; ++zz)
        for (int xx = x; xx < x1; ++xx) {
            auto& sample = at(xx, zz);
            sample = sampler.waterAt(xx, zz);
            waterTile(patch, sampler, xx, zz, sample, wrapX, false);
        }
    const auto same = [](WaterSurface a, WaterSurface b) {
        return a.height() == b.height() && a.reach == b.reach &&
               a.flow == b.flow;
    };
    for (int zz = z; zz < z1; ++zz)
        for (int xx = x; xx < x1; ++xx) {
            const auto water = at(xx, zz);
            if (!water.wet()) continue;
            int endX = xx + 1, endZ = zz + 1;
            while (endX < x1 && same(water, at(endX, zz))) ++endX;
            for (; endZ < z1; ++endZ) {
                int probe = xx;
                while (probe < endX && same(water, at(probe, endZ))) ++probe;
                if (probe != endX) break;
            }
            const float a = xx, b = endX, c = zz, d = endZ, y = water.height();
            quad(patch.water, {a, y, c}, {a, y, d}, {b, y, d}, {b, y, c},
                 (water.flow.x || water.flow.y) ? (water.reach ? -3 : -4) : -2,
                 (water.flow.x || water.flow.y) ? water.flow.x : -1,
                 water.flow.y,
                 WaterSurface::depthTint(y - sampler.height(xx, zz) - 1));
            for (int iz = zz; iz < endZ; ++iz)
                for (int ix = xx; ix < endX; ++ix) at(ix, iz) = {};
        }
}

// Shared vertices on adjoining adaptive cells keep the banks closed even
// where only one side of an edge needs tile-resolution terrain.
struct TerrainCells {
    struct Cell {
        int x0, z0, x1, z1;
        bool channel;
    };
    std::vector<Cell> leaves;
    std::unordered_map<int, std::vector<int>> xs, zs;

    void add(SurfaceSampler& sampler, int x, int z, int x1, int z1, int step) {
        const bool channel = sampler.hasChannel(x - 1, z - 1, x1 + 1, z1 + 1);
        if (channel && (x1 - x > step || z1 - z > step)) {
            const int mx = std::max(x + 1, (x + x1) / 2);
            const int mz = std::max(z + 1, (z + z1) / 2);
            add(sampler, x, z, mx, mz, step);
            if (mx < x1) add(sampler, mx, z, x1, mz, step);
            if (mz < z1) add(sampler, x, mz, mx, z1, step);
            if (mx < x1 && mz < z1) add(sampler, mx, mz, x1, z1, step);
            return;
        }
        leaves.push_back({x, z, x1, z1, channel});
        for (int u : {x, x1}) xs[u].insert(xs[u].end(), {z, z1});
        for (int v : {z, z1}) zs[v].insert(zs[v].end(), {x, x1});
    }

    void sortEdges() {
        const auto sort = [](auto& edges) {
            for (auto& [_, positions] : edges) {
                std::sort(positions.begin(), positions.end());
                positions.erase(std::unique(positions.begin(), positions.end()),
                                positions.end());
            }
        };
        sort(xs);
        sort(zs);
    }

    template <typename PointAt>
    void ring(Cell cell, const PointAt& point, std::vector<Point>& out) const {
        out.clear();
        const auto [x, z, x1, z1, channel] = cell;
        const auto append = [&](const auto& list, int begin, int end,
                                auto position) {
            if (begin < end) {
                for (auto i = std::lower_bound(list.begin(), list.end(), begin);
                     i != list.end() && *i < end; ++i)
                    out.push_back(position(*i));
            } else {
                auto i = std::upper_bound(list.begin(), list.end(), begin);
                while (i != list.begin() && *--i > end)
                    out.push_back(position(*i));
            }
        };
        append(xs.at(x), z, z1, [&](int a) { return point(x, a); });
        append(zs.at(z1), x, x1, [&](int a) { return point(a, z1); });
        append(xs.at(x1), z1, z, [&](int a) { return point(x1, a); });
        append(zs.at(z), x1, x, [&](int a) { return point(a, z); });
    }
};
}  // namespace

std::array<Patch, 4> children(const Patch& p) {
    const int half = p.size() / 2, spacing = p.spacing / 2;
    return {Patch{p.x, p.z, spacing}, Patch{p.x + half, p.z, spacing},
            Patch{p.x, p.z + half, spacing},
            Patch{p.x + half, p.z + half, spacing}};
}
Patch parent(const Patch& p) {
    const int size = p.size() * 2;
    return {p.x / size * size, p.z / size * size, p.spacing * 2};
}

bool shareEdge(const Patch& a, const Patch& b, int periodX) {
    const auto at = [&](int shift) {
        const int bx = b.x + shift;
        const bool xOverlap = a.x < bx + b.size() && bx < a.x + a.size();
        const bool zOverlap = a.z < b.z + b.size() && b.z < a.z + a.size();
        return (zOverlap && (a.x + a.size() == bx || bx + b.size() == a.x)) ||
               (xOverlap && (a.z + a.size() == b.z || b.z + b.size() == a.z));
    };
    return at(0) || (periodX > 0 && (at(periodX) || at(-periodX)));
}

std::vector<Patch> select(int width, int depth, Vec3 eye, float viewportHeight,
                          const ErrorFn& error, bool coarse, float pixelError,
                          float exactRadius, std::optional<Vec3> detailFocus,
                          float viewDistance, int periodX) {
    std::vector<Patch> result;
    // A wrapping world is selected in the eye's frame, where patches may lie
    // past either X end; they are made canonical at the end.
    const bool wrap = periodX > 0;
    if (wrap && !wrapsExactly(periodX, viewDistance))
        throw std::invalid_argument("LOD wrap period does not fit the lattice");
    if (wrap && detailFocus)
        detailFocus->x =
            float(WorldWrap::deltaf(detailFocus->x, eye.x, periodX) + eye.x);
    const auto visible = [&](const Patch& p) {
        return (wrap || (p.x < width && p.x + p.size() > 0)) && p.z < depth &&
               p.z + p.size() > 0 && distance(p, eye) < viewDistance;
    };
    std::function<void(Patch)> visit = [&](Patch p) {
        if (!visible(p)) return;
        const float range = std::max(32.f, distance(p, eye));
        const bool exact = distance(p, detailFocus.value_or(eye)) < exactRadius;
        const float projected =
            (error ? error(p) : 0.f) * viewportHeight * .8660254f /
            std::hypot(range, std::max(0.f, eye.y - HeightProfile::PlainsTopY));
        if (!coarse && p.spacing > 1 && (exact || projected > pixelError))
            children(p, visit);
        else
            result.push_back(p);
    };
    constexpr int root = Cells * RootSpacing;
    const int x0Unbounded =
        int(std::floor((eye.x - viewDistance) / root)) * root;
    const int x0 = wrap ? x0Unbounded : std::max(0, x0Unbounded);
    const float x1 = wrap ? eye.x + viewDistance
                          : std::min(float(width), eye.x + viewDistance);
    const int z0 =
        std::max(0, int(std::floor((eye.z - viewDistance) / root)) * root);
    for (int z = z0; z < std::min(float(depth), eye.z + viewDistance);
         z += root)
        for (int x = x0; x < x1; x += root) visit(Patch{x, z, RootSpacing});
    // A small spatial ownership grid avoids quadratic neighbor scans on the
    // main thread. Its cells represent minimum-size patches, not voxels.
    struct Grid {
        int x = 0, z = 0, width = 0, depth = 0;
        std::vector<int> owners;
        int at(int px, int pz) const {
            px -= x;
            pz -= z;
            return px < 0 || pz < 0 || px >= width || pz >= depth
                       ? -1
                       : owners[pz * width + px];
        }
    };
    const auto gridFor = [&]() {
        Grid grid;
        if (result.empty()) return grid;
        int x1 = 0, z1 = 0;
        grid.x = result[0].x / Cells;
        grid.z = result[0].z / Cells;
        for (const auto& p : result) {
            grid.x = std::min(grid.x, p.x / Cells);
            grid.z = std::min(grid.z, p.z / Cells);
            x1 = std::max(x1, p.x / Cells + p.spacing);
            z1 = std::max(z1, p.z / Cells + p.spacing);
        }
        grid.width = x1 - grid.x;
        grid.depth = z1 - grid.z;
        grid.owners.assign(grid.width * grid.depth, -1);
        for (size_t i = 0; i < result.size(); ++i) {
            const auto& p = result[i];
            for (int z = p.z / Cells; z < p.z / Cells + p.spacing; ++z)
                for (int x = p.x / Cells; x < p.x / Cells + p.spacing; ++x)
                    grid.owners[(z - grid.z) * grid.width + x - grid.x] =
                        int(i);
        }
        return grid;
    };
    const auto eachNeighbor = [&](const Grid& grid, const Patch& p,
                                  const auto& fn) {
        const int x = p.x / Cells, z = p.z / Cells, n = p.spacing;
        for (int t = 0; t < n; ++t) {
            fn(0, grid.at(x - 1, z + t));
            fn(1, grid.at(x + n, z + t));
            fn(2, grid.at(x + t, z - 1));
            fn(3, grid.at(x + t, z + n));
        }
    };
    // Enforce 2:1 balance in batches, at most one pass per tree level.
    for (;;) {
        const auto grid = gridFor();
        std::vector<bool> split(result.size(), false);
        bool changed = false;
        for (size_t i = 0; i < result.size(); ++i)
            eachNeighbor(grid, result[i], [&](int, int neighbor) {
                if (neighbor >= 0 &&
                    result[i].spacing > result[neighbor].spacing * 2)
                    split[i] = changed = true;
            });
        if (!changed) break;
        std::vector<Patch> balanced;
        for (size_t i = 0; i < result.size(); ++i)
            if (split[i])
                children(result[i], [&](Patch p) {
                    if (visible(p)) balanced.push_back(p);
                });
            else
                balanced.push_back(result[i]);
        result = std::move(balanced);
    }
    // The period is a whole number of roots, so every patch stays aligned.
    if (wrap)
        for (auto& p : result) p.x = int(WorldWrap::mod(p.x, periodX));
    std::sort(result.begin(), result.end());
    return result;
}

float skirtDepth(const Patch& p, float error) {
    return std::clamp(2 * error + p.spacing, 4.f, 256.f);
}

float meshHeight(const Patch& p, SurfaceSampler& sampler, float x, float z) {
    if (p.spacing == 1) return sampler.height(int(x), int(z)) + 1;
    const int endX = std::min(p.x + p.size(), sampler.width());
    const int endZ = std::min(p.z + p.size(), sampler.depth());
    const int step = channelSpacing(p);
    const bool channel = sampler.hasChannel(
        int(x) - step, int(z) - step, int(x) + step + 1, int(z) + step + 1);
    const int spacing = channel ? step : p.spacing;
    const int ix =
        std::clamp(int((x - p.x) / spacing), 0, (endX - p.x - 1) / spacing);
    const int iz =
        std::clamp(int((z - p.z) / spacing), 0, (endZ - p.z - 1) / spacing);
    const float x0 = p.x + ix * spacing, z0 = p.z + iz * spacing;
    const float x1 = std::min(int(x0) + spacing, endX),
                z1 = std::min(int(z0) + spacing, endZ);
    const auto point = [&](float xx, float zz) {
        return channel ? riverTerrainPoint(p, sampler, xx, zz)
                       : terrainPoint(p, sampler, xx, zz);
    };
    const auto center = point((x0 + x1) * .5f, (z0 + z1) * .5f);
    const std::array ring{point(x0, z0), point(x0, z1), point(x1, z1),
                          point(x1, z0)};
    const float error = std::abs(center.y - (ring[0].y + ring[2].y) * .5f);
    const auto interpolate = [&](Point a, Point b,
                                 Point c) -> std::optional<float> {
        const float det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(det) < .00001f) return std::nullopt;
        const float u =
            ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / det;
        const float v =
            ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / det;
        const float w = 1 - u - v;
        if (u < -.0001f || v < -.0001f || w < -.0001f) return std::nullopt;
        return u * a.y + v * b.y + w * c.y;
    };
    if (ring.size() == 4 && error <= 1) {
        if (const auto h = interpolate(ring[0], ring[1], ring[2])) return *h;
        if (const auto h = interpolate(ring[0], ring[2], ring[3])) return *h;
    } else
        for (size_t i = 0; i < ring.size(); ++i)
            if (const auto h =
                    interpolate(center, ring[i], ring[(i + 1) % ring.size()]))
                return *h;
    throw std::out_of_range("Point outside LOD surface");
}

Mesh joinDetails(std::span<const Patch> layout,
                 std::span<const SurfaceBand> columns, SurfaceSampler& sampler,
                 const DetailedFn& detailed, int periodX) {
    Mesh result;
    const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
    for (const auto& band : columns)
        for (int edge = 0; edge < 4; ++edge) {
            const int outsideX = band.x + dx[edge],
                      outsideZ = band.z + dz[edge];
            // X has no edge in a wrapping world.
            if (detailed(outsideX, outsideZ) || outsideZ < 0 ||
                outsideZ * 16 >= sampler.depth() ||
                (periodX <= 0 &&
                 (outsideX < 0 || outsideX * 16 >= sampler.width())))
                continue;
            for (int at = 0; at < 16; ++at) {
                const int ix = band.x * 16 + (edge == 1  ? 15
                                              : edge < 2 ? 0
                                                         : at);
                const int iz = band.z * 16 + (edge == 3   ? 15
                                              : edge >= 2 ? 0
                                                          : at);
                const float x = ix + (edge == 1 ? 1 : 0),
                            z = iz + (edge == 3 ? 1 : 0);
                const float x1 = x + (edge >= 2 ? 1 : 0),
                            z1 = z + (edge < 2 ? 1 : 0);
                const float oxFrame = (x + x1) * .5f + dx[edge] * .5f;
                const float oz = (z + z1) * .5f + dz[edge] * .5f;
                // Canonical layout: find the patch at the wrapped point and
                // query its surface in its own frame.
                const float ox = float(WorldWrap::modf(oxFrame, periodX));
                const float shift = oxFrame - ox;
                const auto patch = std::find_if(
                    layout.begin(), layout.end(), [&](const Patch& p) {
                        return ox >= p.x && ox < p.x + p.size() && oz >= p.z &&
                               oz < p.z + p.size();
                    });
                if (patch == layout.end() || patch->spacing == 1) continue;
                const auto inside = sampler.column(ix, iz);
                const float top = inside.surfaceY + 1;
                const float y0 = meshHeight(*patch, sampler, x - shift, z);
                const float y1 = meshHeight(*patch, sampler, x1 - shift, z1);
                if (std::abs(top - y0) < .0001f && std::abs(top - y1) < .0001f)
                    continue;
                const float sign = top >= (y0 + y1) * .5f ? 1 : -1;
                const float layer = inside.material;
                const float water = inside.water.height();
                const uint32_t base = uint32_t(result.vertices.size());
                for (const auto& point : std::array<Point, 4>{{{x, top, z},
                                                               {x1, top, z1},
                                                               {x1, y1, z1},
                                                               {x, y0, z}}})
                    result.vertices.push_back(
                        {point.x, point.y, point.z, dx[edge] * sign, 0,
                         dz[edge] * sign, 1, 0, 0, -2 - water, 3, layer,
                         0xffffffff});
                result.indices.insert(
                    result.indices.end(),
                    {base, base + 1, base + 2, base, base + 2, base + 3});
            }
        }
    return result;
}

ColumnBands sampleBands(int cx, int cz, SurfaceSampler& sampler) {
    constexpr int side = Chunk::SIZE_X, slab = Chunk::SIZE_Y;
    int lowGround = WORLD_HEIGHT_Y, highGround = 0;
    int lowWater = WORLD_HEIGHT_Y, highWater = -1;
    for (int z = -1; z <= side; ++z)
        for (int x = -1; x <= side; ++x) {
            const int tx = cx * side + x, tz = cz * side + z;
            const int ground = sampler.height(tx, tz);
            lowGround = std::min(lowGround, ground);
            highGround = std::max(highGround, ground);
            if (x < 0 || x >= side || z < 0 || z >= side) continue;
            const auto water = sampler.waterAt(tx, tz);
            if (water.wet()) {
                lowWater = std::min(lowWater, water.topTile());
                // Side faces reach down to a lower neighbor. Sample ground
                // only where the neighboring water stands lower.
                for (const auto& [dx, dz] :
                     std::array<Vec2i, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                    if (tz + dz < 0 || tz + dz >= sampler.depth()) continue;
                    const auto other = sampler.waterAt(tx + dx, tz + dz);
                    if (other.surfaceY >= water.surfaceY) continue;
                    lowWater = std::min(
                        lowWater, water.dropBottom(
                                      other, sampler.height(tx + dx, tz + dz)));
                }
                highWater = std::max(highWater, water.topTile());
            }
        }
    ColumnBands result{{cx, cz, lowGround / slab, highGround / slab}, {}};
    if (highWater >= 0)
        result.water = SurfaceBand{cx, cz, lowWater / slab, highWater / slab};
    return result;
}

Mesh build(const Patch& p, SurfaceSampler& sampler,
           const std::atomic_bool& cancelled, int materialMap, bool wrapX,
           bool waterOnly) {
    Mesh mesh;
    const int endX = std::min(p.x + p.size(), sampler.width());
    const int endZ = std::min(p.z + p.size(), sampler.depth());
    if (endX <= p.x || endZ <= p.z) return mesh;
    mesh.provisionalWater = sampler.hasUnrefinedWater(p.x, p.z, endX, endZ);
    mesh.bounds = {float(p.x), float(Y_CHUNKS * Chunk::SIZE_Y),
                   float(p.z), float(endX),
                   0,          float(endZ)};
    mesh.water.bounds = mesh.bounds;
    const bool stepped = p.spacing == 1;
    // Refine the shoreline at up to eight subdivisions per terrain cell.
    // Stepped and nearby patches can still represent each individual tile.
    const int waterSpacing = std::max(1, p.spacing / 8);
    const int step = channelSpacing(p);
    const bool riverPatch =
        !stepped &&
        sampler.hasChannel(p.x - step, p.z - step, endX + step, endZ + step);
    std::unordered_map<uint64_t, float> corrected;
    const auto point = [&](float x, float z) {
        if (!riverPatch) return terrainPoint(p, sampler, x, z);
        const uint64_t key = (uint64_t(unsigned(x)) << 32) | unsigned(z);
        const auto [it, inserted] = corrected.try_emplace(key);
        if (inserted) it->second = riverTerrainPoint(p, sampler, x, z).y;
        return Point{x, it->second, z};
    };
    TerrainCells cells;
    for (int z = p.z; z < endZ; z += p.spacing) {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        for (int x = p.x; x < endX; x += p.spacing) {
            const int x1 = std::min(x + p.spacing, endX),
                      z1 = std::min(z + p.spacing, endZ);
            if (riverPatch)
                cells.add(sampler, x, z, x1, z1, step);
            else
                cells.leaves.push_back({x, z, x1, z1, false});
        }
    }
    if (riverPatch) cells.sortEdges();
    std::vector<Point> ring;
    for (const auto cell : cells.leaves) {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        const auto [x, z, endCellX, endCellZ, channel] = cell;
        const float x1 = endCellX, z1 = endCellZ;
        if (stepped) {
            if (waterOnly) {
                waterTile(mesh, sampler, x, z, sampler.waterAt(x, z), wrapX);
                continue;
            }
            const auto column = sampler.column(x, z);
            const float y = column.surfaceY + 1;
            quad(mesh, {float(x), y, float(z)}, {float(x), y, z1}, {x1, y, z1},
                 {x1, y, float(z)}, column.material);
            const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
            for (int edge = 0; edge < 4; ++edge) {
                const bool boundary = edge == 0   ? x == p.x
                                      : edge == 1 ? x1 == endX
                                      : edge == 2 ? z == p.z
                                                  : z1 == endZ;
                const int nx = x + dx[edge], nz = z + dz[edge];
                const bool outside =
                    nz < 0 || nz >= sampler.depth() ||
                    (!wrapX && (nx < 0 || nx >= sampler.width()));
                // Boundary walls reach below the lower column so they
                // overlap any neighbour patch's surface or skirt.
                const int neighbour = sampler.height(nx, nz) + 1;
                const int bottom =
                    outside    ? 0
                    : boundary ? std::max(0, std::min(neighbour, int(y)) -
                                                 SteppedSkirt)
                               : neighbour;
                const auto wallLayer = [&](int tileY) {
                    // The top of an overlap wall is a continuation of the
                    // surface. Preserve the strata below it and on cliffs.
                    return boundary && !outside && tileY >= int(y) - 2
                               ? column.material
                               : sampler.materialAt(x, tileY, z);
                };
                for (int y0 = bottom; y0 < int(y);) {
                    const float layer = wallLayer(y0);
                    int nextY = y0 + 1;
                    while (nextY < int(y) && wallLayer(nextY) == layer) ++nextY;
                    const float low = y0, high = nextY;
                    y0 = nextY;
                    if (edge == 0)
                        quad(mesh, {float(x), low, float(z)},
                             {float(x), low, z1}, {float(x), high, z1},
                             {float(x), high, float(z)}, layer, y,
                             boundary ? 2.f : 0.f);
                    if (edge == 1)
                        quad(mesh, {x1, low, z1}, {x1, low, float(z)},
                             {x1, high, float(z)}, {x1, high, z1}, layer, y,
                             boundary ? 2.f : 0.f);
                    if (edge == 2)
                        quad(mesh, {x1, low, float(z)},
                             {float(x), low, float(z)},
                             {float(x), high, float(z)}, {x1, high, float(z)},
                             layer, y, boundary ? 2.f : 0.f);
                    if (edge == 3)
                        quad(mesh, {float(x), low, z1}, {x1, low, z1},
                             {x1, high, z1}, {float(x), high, z1}, layer, y,
                             boundary ? 2.f : 0.f);
                }
            }
            waterTile(mesh, sampler, x, z, column.water, wrapX);
            continue;
        }
        if (channel && waterOnly) {
            channelWater(mesh, sampler, x, z, endCellX, endCellZ, wrapX);
            continue;
        }
        const auto center = point((x + x1) * .5f, (z + z1) * .5f);
        if (riverPatch)
            cells.ring(cell, point, ring);
        else {
            const auto corners = terrainRing(p, sampler, x, z, x1, z1);
            ring.assign(corners.begin(), corners.end());
        }
        const float error =
            std::abs(center.y - (ring[0].y + point(x1, z1).y) * .5f);
        if (!channel) mesh.error = std::max(mesh.error, error);
        const bool flat = ring.size() == 4 && error <= 1.f;
        if (!waterOnly) {
            if (flat)
                quad(mesh, ring[0], ring[1], ring[2], ring[3], -1);
            else
                for (size_t i = 0; i < ring.size(); ++i)
                    triangle(mesh, center, ring[i], ring[(i + 1) % ring.size()],
                             -1);
        }
        if (channel) {
            channelWater(mesh, sampler, x, z, endCellX, endCellZ, wrapX);
            continue;
        }
        const auto water = [&](auto&& self, int ax, int az, int bx,
                               int bz) -> void {
            const int mx = (ax + bx) / 2, mz = (az + bz) / 2;
            const auto sample = sampler.waterAt(mx, mz);
            if (bx - ax > waterSpacing || bz - az > waterSpacing) {
                // Refine shore cells and any edited footprint. Broad areas
                // of deep sea and high land keep the coarse tessellation.
                if (!sampler.uniformWater(ax, az, bx, bz, sample)) {
                    const int sx = std::max(ax + 1, mx);
                    const int sz = std::max(az + 1, mz);
                    self(self, ax, az, sx, sz);
                    if (sx < bx) self(self, sx, az, bx, sz);
                    if (sz < bz) self(self, ax, sz, sx, bz);
                    if (sx < bx && sz < bz) self(self, sx, sz, bx, bz);
                    return;
                }
            }
            if (!sample.wet()) return;
            mesh.lakeSurface.include(sample, mx, mz);
            const float level = sample.height();
            const auto cap = [&](Point a, Point b, Point c) {
                if (std::min({a.y, b.y, c.y}) >= level ||
                    std::max({a.x, b.x, c.x}) <= ax ||
                    std::min({a.x, b.x, c.x}) >= bx ||
                    std::max({a.z, b.z, c.z}) <= az ||
                    std::min({a.z, b.z, c.z}) >= bz)
                    return;
                std::array<Point, 8> polygon{a, b, c};
                size_t count = 3;
                const auto clip = [&](auto distance) {
                    std::array<Point, 8> next;
                    size_t n = 0;
                    for (size_t i = 0; i < count; ++i) {
                        const auto u = polygon[i], v = polygon[(i + 1) % count];
                        const float du = distance(u), dv = distance(v);
                        if (du >= 0) next[n++] = u;
                        if ((du < 0) != (dv < 0) && du != 0 && dv != 0) {
                            const float t = du / (du - dv);
                            next[n++] = {std::lerp(u.x, v.x, t),
                                         std::lerp(u.y, v.y, t),
                                         std::lerp(u.z, v.z, t)};
                        }
                    }
                    std::copy_n(next.begin(), n, polygon.begin());
                    count = n;
                };
                clip([&](Point v) { return level - v.y; });
                clip([&](Point v) { return v.x - ax; });
                clip([&](Point v) { return bx - v.x; });
                clip([&](Point v) { return v.z - az; });
                clip([&](Point v) { return bz - v.z; });
                for (size_t i = 1; i + 1 < count; ++i) {
                    const std::array points{polygon[0], polygon[i],
                                            polygon[i + 1]};
                    const auto surface = [level](Point p) {
                        p.y = level;
                        return p;
                    };
                    const size_t first = mesh.water.vertices.size();
                    triangle(mesh.water, surface(points[0]), surface(points[1]),
                             surface(points[2]), -2);
                    for (size_t v = first; v < mesh.water.vertices.size(); ++v)
                        mesh.water.vertices[v].tint = WaterSurface::depthTint(
                            level - points[v - first].y);
                }
            };
            if (flat) {
                cap(ring[0], ring[1], ring[2]);
                cap(ring[0], ring[2], ring[3]);
            } else
                for (size_t i = 0; i < ring.size(); ++i)
                    cap(center, ring[i], ring[(i + 1) % ring.size()]);
        };
        water(water, x, z, int(x1), int(z1));
    }
    if (!waterOnly) {
        // Smooth patches: skirts on inner edges hide cracks against neighbours
        // of any resolution; walls down to the floor close finite-world edges.
        for (int edge = 0; edge < 4; ++edge) {
            const bool outer = edge == 0   ? !wrapX && p.x == 0
                               : edge == 1 ? !wrapX && endX == sampler.width()
                               : edge == 2 ? p.z == 0
                                           : endZ == sampler.depth();
            if (stepped) continue;
            const float skirt = skirtDepth(p, mesh.error);
            const int length = edge < 2 ? endZ - p.z : endX - p.x;
            std::vector<int> offsets;
            if (riverPatch) {
                const auto& edgeVertices =
                    edge < 2 ? cells.xs.at(edge == 0 ? p.x : endX)
                             : cells.zs.at(edge == 2 ? p.z : endZ);
                const int start = edge < 2 ? p.z : p.x;
                for (int coordinate : edgeVertices)
                    offsets.push_back(coordinate - start);
            } else {
                for (int at = 0; at < length; at += p.spacing)
                    offsets.push_back(at);
                offsets.push_back(length);
            }
            for (size_t i = 1; i < offsets.size(); ++i) {
                const int at = offsets[i - 1];
                if (cancelled.load(std::memory_order_relaxed)) return {};
                const int next = offsets[i];
                const float x = edge < 2 ? (edge == 0 ? p.x : endX) : p.x + at;
                const float z = edge < 2 ? p.z + at : (edge == 2 ? p.z : endZ);
                const float xx = edge < 2 ? x : p.x + next;
                const float zz = edge < 2 ? p.z + next : z;
                const auto a = point(x, z), b = point(xx, zz);
                const float bottom =
                    outer ? 0 : std::max(0.f, std::min(a.y, b.y) - skirt);
                const Point lowA{x, bottom, z}, lowB{xx, bottom, zz};
                const float edgeKind = outer ? 0.f : 1.f;
                // Outward normals are used to test detailed coverage on the
                // owning side, even when the skirt is shaded as top terrain.
                if (edge == 0 || edge == 3)
                    quad(mesh, b, a, lowA, lowB, -1, -1, edgeKind);
                else
                    quad(mesh, a, b, lowB, lowA, -1, -1, edgeKind);
            }
        }
        if (stepped) {
            for (int cz = p.z / 16; cz < (endZ + 15) / 16; ++cz)
                for (int cx = p.x / 16; cx < (endX + 15) / 16; ++cx) {
                    mesh.bands.push_back(sampleBands(cx, cz, sampler));
                }
        }
        // Ground cover: one texel per tile on stepped patches, so the hand-over
        // to detailed chunks matches; the material map's grid elsewhere.
        mesh.coverSize = stepped ? p.size() : std::max(1, materialMap);
        mesh.covers.resize(size_t(mesh.coverSize) * mesh.coverSize * 4);
        for (int z = 0; z < mesh.coverSize; ++z) {
            if (cancelled.load(std::memory_order_relaxed)) return {};
            for (int x = 0; x < mesh.coverSize; ++x) {
                const auto c =
                    sampler.cover(p.x + (x + .5) * p.size() / mesh.coverSize,
                                  p.z + (z + .5) * p.size() / mesh.coverSize);
                const size_t i = (size_t(z) * mesh.coverSize + x) * 4;
                mesh.covers[i] = c.layer;
                mesh.covers[i + 1] = c.density;
                mesh.covers[i + 2] = uint8_t(c.tint & 255);
                mesh.covers[i + 3] = uint8_t(c.tint >> 8);
            }
        }
        mesh.mapSize = stepped ? 1 : std::max(1, materialMap);
        mesh.materials.resize(mesh.mapSize * mesh.mapSize * 4);
        for (int z = 0; z < mesh.mapSize; ++z) {
            if (cancelled.load(std::memory_order_relaxed)) return {};
            for (int x = 0; x < mesh.mapSize; ++x) {
                const auto c =
                    sampler.column(p.x + (x + .5) * p.size() / mesh.mapSize,
                                   p.z + (z + .5) * p.size() / mesh.mapSize);
                const int i = (z * mesh.mapSize + x) * 4;
                mesh.materials[i] = c.material & 255;
                mesh.materials[i + 1] = c.material >> 8;
                mesh.materials[i + 2] = c.rockMaterial & 255;
                mesh.materials[i + 3] = c.rockMaterial >> 8;
            }
        }
    }
    mesh.waterMapSize = stepped ? p.size() : std::max(1, materialMap);
    mesh.waterLevels.resize(size_t(mesh.waterMapSize) * mesh.waterMapSize * 2);
    for (int z = 0; z < mesh.waterMapSize; ++z) {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        for (int x = 0; x < mesh.waterMapSize; ++x) {
            const int level = std::max(
                0, sampler
                       .waterAt(p.x + (x + .5) * p.size() / mesh.waterMapSize,
                                p.z + (z + .5) * p.size() / mesh.waterMapSize)
                       .surfaceY);
            const size_t i = (size_t(z) * mesh.waterMapSize + x) * 2;
            mesh.waterLevels[i] = level & 255;
            mesh.waterLevels[i + 1] = level >> 8;
        }
    }
    // Share smooth normals and indexed vertices. Water and coplanar voxel
    // faces in particular should not consume six vertices per quad.
    for (auto& v : mesh.vertices)
        if (v.layer == -1 && v.ny > 0.01f && !riverPatch) {
            // Grid-aligned differences reuse the heights sampled for the
            // surface instead of four fresh off-grid samples per vertex.
            const int x = int(v.x), z = int(v.z), step = p.spacing;
            const float dx =
                (sampler.height(x + step, z) - sampler.height(x - step, z)) /
                (2.f * step);
            const float dz =
                (sampler.height(x, z + step) - sampler.height(x, z - step)) /
                (2.f * step);
            const float n = std::sqrt(dx * dx + dz * dz + 1);
            v.nx = -dx / n;
            v.ny = 1 / n;
            v.nz = -dz / n;
        }
    using Key = std::array<uint32_t, sizeof(BlockVertex) / sizeof(uint32_t)>;
    struct Hash {
        size_t operator()(const Key& k) const {
            size_t h = 0;
            for (auto n : k) h ^= size_t(n) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    const auto indexVertices = [](Geometry& geometry) {
        std::unordered_map<Key, uint32_t, Hash> unique;
        std::vector<BlockVertex> vertices;
        unique.reserve(geometry.vertices.size() / 2);
        vertices.reserve(geometry.vertices.size() / 2);
        for (auto& index : geometry.indices) {
            const auto& v = geometry.vertices[index];
            const auto [it, inserted] = unique.emplace(
                std::bit_cast<Key>(v), uint32_t(vertices.size()));
            if (inserted) vertices.push_back(v);
            index = it->second;
        }
        geometry.vertices = std::move(vertices);
    };
    indexVertices(mesh);
    indexVertices(mesh.water);
    if (!mesh.water.vertices.empty()) {
        auto& bounds = mesh.water.bounds;
        bounds.minX = bounds.minZ = std::numeric_limits<float>::infinity();
        bounds.maxX = bounds.maxZ = -std::numeric_limits<float>::infinity();
        for (const auto& v : mesh.water.vertices) {
            bounds.minX = std::min(bounds.minX, v.x);
            bounds.maxX = std::max(bounds.maxX, v.x);
            bounds.minZ = std::min(bounds.minZ, v.z);
            bounds.maxZ = std::max(bounds.maxZ, v.z);
        }
    }
    return mesh;
}
}  // namespace TerrainLod
