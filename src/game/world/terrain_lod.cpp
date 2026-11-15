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
    const int half = p.size() / 2;
    for (int dz : {0, half})
        for (int dx : {0, half}) fn(Patch{p.x + dx, p.z + dz, p.spacing / 2});
}
struct Point {
    float x, y, z;
};
Point terrainPoint(const Patch& p, SurfaceSampler& sampler, float x, float z) {
    float h = sampler.height(int(x), int(z)) + 1;
    if (p.spacing > 1) {
        if (x == p.x) h = edgeHeight(p, sampler, 0, z - p.z);
        if (x == p.x + p.size()) h = edgeHeight(p, sampler, 1, z - p.z);
        if (z == p.z) h = edgeHeight(p, sampler, 2, x - p.x);
        if (z == p.z + p.size()) h = edgeHeight(p, sampler, 3, x - p.x);
    }
    return {x, h, z};
}
std::vector<Point> terrainRing(const Patch& p, SurfaceSampler& sampler, float x,
                               float z, float x1, float z1) {
    const int endX = std::min(p.x + p.size(), sampler.width());
    const int endZ = std::min(p.z + p.size(), sampler.depth());
    const auto point = [&](float px, float pz) {
        return terrainPoint(p, sampler, px, pz);
    };
    std::vector<Point> ring;
    ring.push_back(point(x, z));
    if (x == p.x) ring.push_back(point(x, (z + z1) * .5f));
    ring.push_back(point(x, z1));
    if (z1 == endZ) ring.push_back(point((x + x1) * .5f, z1));
    ring.push_back(point(x1, z1));
    if (x1 == endX) ring.push_back(point(x1, (z + z1) * .5f));
    ring.push_back(point(x1, z));
    if (z == p.z) ring.push_back(point((x + x1) * .5f, z));
    return ring;
}
void triangle(Mesh& mesh, Point a, Point b, Point c, float layer) {
    const Point u{b.x - a.x, b.y - a.y, b.z - a.z};
    const Point v{c.x - a.x, c.y - a.y, c.z - a.z};
    Point n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
            u.x * v.y - u.y * v.x};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < .00001f) return;
    n = {n.x / length, n.y / length, n.z / length};
    const auto base = uint32_t(mesh.vertices.size());
    for (const auto& p : {a, b, c}) {
        mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, 1, 0, 0, p.x,
                                 p.z, layer, 0xffffffff});
        mesh.bounds.minY = std::min(mesh.bounds.minY, p.y);
        mesh.bounds.maxY = std::max(mesh.bounds.maxY, p.y);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}
void quad(Mesh& mesh, Point a, Point b, Point c, Point d, float layer) {
    triangle(mesh, a, b, c, layer);
    triangle(mesh, a, c, d, layer);
}
}  // namespace

std::vector<Patch> select(int width, int depth, Vec3 eye, float viewportHeight,
                          const ErrorFn& error, bool coarse, float pixelError,
                          float exactRadius, std::optional<Vec3> detailFocus) {
    std::vector<Patch> result;
    const auto visible = [&](const Patch& p) {
        return p.x < width && p.z < depth && p.x + p.size() > 0 &&
               p.z + p.size() > 0 && distance(p, eye) < ViewDistance;
    };
    std::function<void(Patch)> visit = [&](Patch p) {
        if (!visible(p)) return;
        const float range = std::max(32.f, distance(p, eye));
        const bool exact = distance(p, detailFocus.value_or(eye)) < exactRadius;
        const float projected = (error ? error(p) : 0.f) * viewportHeight *
                                .8660254f /
                                std::hypot(range, std::max(0.f, eye.y - HeightProfile::PlainsTopY));
        if (!coarse && p.spacing > 1 && (exact || projected > pixelError))
            children(p, visit);
        else
            result.push_back(p);
    };
    constexpr int root = Cells * RootSpacing;
    const int x0 =
        std::max(0, int(std::floor((eye.x - ViewDistance) / root)) * root);
    const int z0 =
        std::max(0, int(std::floor((eye.z - ViewDistance) / root)) * root);
    for (int z = z0; z < std::min(float(depth), eye.z + ViewDistance);
         z += root)
        for (int x = x0; x < std::min(float(width), eye.x + ViewDistance);
             x += root)
            visit(Patch{x, z, RootSpacing});
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
    const auto grid = gridFor();
    for (auto& p : result) {
        p.neighbors.fill(0);
        eachNeighbor(grid, p, [&](int edge, int neighbor) {
            if (neighbor >= 0) {
                const int spacing = result[neighbor].spacing;
                p.neighbors[edge] = p.neighbors[edge]
                                        ? std::min(p.neighbors[edge], spacing)
                                        : spacing;
            }
        });
        for (auto& n : p.neighbors)
            if (!n) n = p.spacing;
    }
    std::sort(result.begin(), result.end());
    return result;
}

float edgeHeight(const Patch& p, SurfaceSampler& sampler, int edge,
                 float along) {
    // Every smooth cell has an edge midpoint. A fine patch on a coarse edge
    // interpolates those coarse segments rather than sampling a new height.
    const int stride = std::max(1, std::max(p.spacing, p.neighbors[edge]) / 2);
    const int origin = edge < 2 ? p.z : p.x;
    const float t = origin + along;
    const float first = std::floor(t / stride) * stride;
    const auto sample = [&](float at) {
        return float(
            sampler.height(
                edge < 2 ? p.x + (edge == 1 ? p.size() : 0) : int(at),
                edge < 2 ? int(at) : p.z + (edge == 3 ? p.size() : 0)) +
            1);
    };
    return std::lerp(sample(first), sample(first + stride),
                     (t - first) / stride);
}

float meshHeight(const Patch& p, SurfaceSampler& sampler, float x, float z) {
    if (p.spacing == 1) return sampler.height(int(x), int(z)) + 1;
    const int endX = std::min(p.x + p.size(), sampler.width());
    const int endZ = std::min(p.z + p.size(), sampler.depth());
    const int ix =
        std::clamp(int((x - p.x) / p.spacing), 0, (endX - p.x - 1) / p.spacing);
    const int iz =
        std::clamp(int((z - p.z) / p.spacing), 0, (endZ - p.z - 1) / p.spacing);
    const float x0 = p.x + ix * p.spacing, z0 = p.z + iz * p.spacing;
    const float x1 = std::min(int(x0) + p.spacing, endX),
                z1 = std::min(int(z0) + p.spacing, endZ);
    const auto center =
        terrainPoint(p, sampler, (x0 + x1) * .5f, (z0 + z1) * .5f);
    const auto ring = terrainRing(p, sampler, x0, z0, x1, z1);
    const float error =
        std::abs(center.y - (terrainPoint(p, sampler, x0, z0).y +
                             terrainPoint(p, sampler, x1, z1).y) *
                                .5f);
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
                 const DetailedFn& detailed) {
    Mesh result;
    const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
    for (const auto& band : columns)
        for (int edge = 0; edge < 4; ++edge) {
            const int outsideX = band.x + dx[edge],
                      outsideZ = band.z + dz[edge];
            if (detailed(outsideX, outsideZ) || outsideX < 0 || outsideZ < 0 ||
                outsideX * 16 >= sampler.width() ||
                outsideZ * 16 >= sampler.depth())
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
                const float ox = (x + x1) * .5f + dx[edge] * .5f;
                const float oz = (z + z1) * .5f + dz[edge] * .5f;
                const auto patch = std::find_if(
                    layout.begin(), layout.end(), [&](const Patch& p) {
                        return ox >= p.x && ox < p.x + p.size() && oz >= p.z &&
                               oz < p.z + p.size();
                    });
                if (patch == layout.end() || patch->spacing == 1) continue;
                const auto inside = sampler.column(ix, iz);
                const float top =
                    std::max(inside.surfaceY + 1, SEA_LEVEL_Y + 1);
                const float y0 = std::max(meshHeight(*patch, sampler, x, z),
                                          float(SEA_LEVEL_Y + 1));
                const float y1 = std::max(meshHeight(*patch, sampler, x1, z1),
                                          float(SEA_LEVEL_Y + 1));
                if (std::abs(top - y0) < .0001f && std::abs(top - y1) < .0001f)
                    continue;
                const float sign = top >= (y0 + y1) * .5f ? 1 : -1;
                const float layer =
                    inside.waterY < 0
                        ? inside.material
                        : sampler.column(ix + dx[edge], iz + dz[edge]).material;
                const uint32_t base = uint32_t(result.vertices.size());
                for (const auto& point : std::array<Point, 4>{{{x, top, z},
                                                               {x1, top, z1},
                                                               {x1, y1, z1},
                                                               {x, y0, z}}})
                    result.vertices.push_back(
                        {point.x, point.y, point.z, dx[edge] * sign, 0,
                         dz[edge] * sign, 1, 0, 0, point.x, point.z, layer,
                         0xffffffff});
                result.indices.insert(
                    result.indices.end(),
                    {base, base + 1, base + 2, base, base + 2, base + 3});
            }
        }
    return result;
}

Mesh build(const Patch& p, SurfaceSampler& sampler,
           const std::atomic_bool& cancelled) {
    Mesh mesh;
    const int endX = std::min(p.x + p.size(), sampler.width());
    const int endZ = std::min(p.z + p.size(), sampler.depth());
    mesh.bounds = {float(p.x), float(Y_CHUNKS * Chunk::SIZE_Y),
                   float(p.z), float(endX),
                   0,          float(endZ)};
    const bool stepped = p.spacing == 1;
    const auto point = [&](float x, float z) {
        return terrainPoint(p, sampler, x, z);
    };
    const auto cellRing = [&](float x, float z, float x1, float z1) {
        return terrainRing(p, sampler, x, z, x1, z1);
    };
    for (int z = p.z; z < endZ; z += p.spacing) {
        if (cancelled.load(std::memory_order_relaxed)) return {};
        for (int x = p.x; x < endX; x += p.spacing) {
            const float x1 = std::min(x + p.spacing, endX);
            const float z1 = std::min(z + p.spacing, endZ);
            const auto column = sampler.column(x, z);
            if (stepped) {
                const float y = column.surfaceY + 1;
                quad(mesh, {float(x), y, float(z)}, {float(x), y, z1},
                     {x1, y, z1}, {x1, y, float(z)}, column.material);
                const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
                for (int edge = 0; edge < 4; ++edge) {
                    const bool boundary = edge == 0   ? x == p.x
                                          : edge == 1 ? x1 == endX
                                          : edge == 2 ? z == p.z
                                                      : z1 == endZ;
                    const int nx = x + dx[edge], nz = z + dz[edge];
                    const bool outside = nx < 0 || nz < 0 ||
                                         nx >= sampler.width() ||
                                         nz >= sampler.depth();
                    if (boundary && !outside && p.neighbors[edge] > 1) continue;
                    const int bottom = outside ? 0 : sampler.height(nx, nz) + 1;
                    for (int y0 = bottom; y0 < int(y);) {
                        const float layer = sampler.materialAt(x, y0, z);
                        int nextY = y0 + 1;
                        while (nextY < int(y) &&
                               sampler.materialAt(x, nextY, z) == layer)
                            ++nextY;
                        const float low = y0, high = nextY;
                        y0 = nextY;
                        if (edge == 0)
                            quad(mesh, {float(x), low, float(z)},
                                 {float(x), low, z1}, {float(x), high, z1},
                                 {float(x), high, float(z)}, layer);
                        if (edge == 1)
                            quad(mesh, {x1, low, z1}, {x1, low, float(z)},
                                 {x1, high, float(z)}, {x1, high, z1}, layer);
                        if (edge == 2)
                            quad(mesh, {x1, low, float(z)},
                                 {float(x), low, float(z)},
                                 {float(x), high, float(z)},
                                 {x1, high, float(z)}, layer);
                        if (edge == 3)
                            quad(mesh, {float(x), low, z1}, {x1, low, z1},
                                 {x1, high, z1}, {float(x), high, z1}, layer);
                    }
                }
            } else {
                const float cx = (x + x1) * .5f, cz = (z + z1) * .5f;
                const auto center = point(cx, cz);
                const auto ring = cellRing(x, z, x1, z1);
                const float error = std::abs(
                    center.y - (point(x, z).y + point(x1, z1).y) * .5f);
                mesh.error = std::max(mesh.error, error);
                if (ring.size() == 4 && error <= 1.f)
                    quad(mesh, ring[0], ring[1], ring[2], ring[3], -1);
                else
                    for (size_t i = 0; i < ring.size(); ++i)
                        triangle(mesh, center, ring[i],
                                 ring[(i + 1) % ring.size()], -1);
            }
            // Clip each terrain triangle against the water plane. The opaque
            // cap covers only submerged terrain, including partial coast cells.
            if (stepped) {
                if (column.waterY >= 0) {
                    const float y = column.waterY;
                    quad(mesh, {float(x), y, float(z)}, {float(x), y, z1},
                         {x1, y, z1}, {x1, y, float(z)}, -2);
                }
            } else {
                const auto cap = [&](Point a, Point b, Point c) {
                    constexpr float level = SEA_LEVEL_Y + 1;
                    std::vector<Point> polygon;
                    const std::array<Point, 3> input{a, b, c};
                    for (int i = 0; i < 3; ++i) {
                        const auto u = input[i], v = input[(i + 1) % 3];
                        if (u.y < level) polygon.push_back({u.x, level, u.z});
                        if ((u.y < level) != (v.y < level)) {
                            const float t = (level - u.y) / (v.y - u.y);
                            polygon.push_back({std::lerp(u.x, v.x, t), level,
                                               std::lerp(u.z, v.z, t)});
                        }
                    }
                    for (size_t i = 1; i + 1 < polygon.size(); ++i)
                        triangle(mesh, polygon[0], polygon[i], polygon[i + 1],
                                 -2);
                };
                const auto center = point((x + x1) * .5f, (z + z1) * .5f);
                const auto ring = cellRing(x, z, x1, z1);
                const float error = std::abs(
                    center.y - (point(x, z).y + point(x1, z1).y) * .5f);
                if (ring.size() == 4 && error <= 1.f) {
                    cap(ring[0], ring[1], ring[2]);
                    cap(ring[0], ring[2], ring[3]);
                } else
                    for (size_t i = 0; i < ring.size(); ++i)
                        cap(center, ring[i], ring[(i + 1) % ring.size()]);
            }
        }
    }
    // Smooth/stepped joins and finite-world walls share the mesh's canonical
    // boundary. Each stepped neighbor column gets its own vertical join.
    for (int edge = 0; edge < 4; ++edge) {
        const bool outer = edge == 0   ? p.x == 0
                           : edge == 1 ? endX == sampler.width()
                           : edge == 2 ? p.z == 0
                                       : endZ == sampler.depth();
        if (stepped || (!outer && p.neighbors[edge] != 1)) continue;
        const int length = edge < 2 ? endZ - p.z : endX - p.x;
        const int stride = outer ? std::max(1, p.spacing / 2) : 1;
        for (int at = 0; at < length; at += stride) {
            if (cancelled.load(std::memory_order_relaxed)) return {};
            const int next = std::min(at + stride, length);
            const float x = edge < 2 ? (edge == 0 ? p.x : endX) : p.x + at;
            const float z = edge < 2 ? p.z + at : (edge == 2 ? p.z : endZ);
            const float xx = edge < 2 ? x : p.x + next;
            const float zz = edge < 2 ? p.z + next : z;
            const auto a = point(x, z), b = point(xx, zz);
            const int nx = int(x) - (edge == 0 ? 1 : 0),
                      nz = int(z) - (edge == 2 ? 1 : 0);
            const float bottom = outer ? 0 : sampler.height(nx, nz) + 1;
            const float layer =
                outer ? -1 : sampler.materialAt(nx, int(bottom) - 1, nz);
            quad(mesh, a, b, {xx, bottom, zz}, {x, bottom, z}, layer);
        }
    }
    if (stepped) {
        for (int cz = p.z / 16; cz < (endZ + 15) / 16; ++cz)
            for (int cx = p.x / 16; cx < (endX + 15) / 16; ++cx) {
                int low = Y_CHUNKS * Chunk::SIZE_Y, high = 0;
                for (int z = cz * 16 - 1; z <= cz * 16 + 16; ++z)
                    for (int x = cx * 16 - 1; x <= cx * 16 + 16; ++x) {
                        const int h = sampler.height(x, z);
                        low = std::min(low, h);
                        high = std::max(high, std::max(h, SEA_LEVEL_Y));
                    }
                mesh.bands.push_back({cx, cz, low / 16, high / 16});
            }
    }
    mesh.mapSize = stepped ? 1 : 64;
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
    // Share smooth normals and indexed vertices. Water and coplanar voxel
    // faces in particular should not consume six vertices per quad.
    for (auto& v : mesh.vertices)
        if (v.layer == -1 && v.ny > 0.01f) {
            const int x = int(v.x), z = int(v.z);
            const float dx =
                (sampler.height(x + 1, z) - sampler.height(x - 1, z)) * .5f;
            const float dz =
                (sampler.height(x, z + 1) - sampler.height(x, z - 1)) * .5f;
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
    std::unordered_map<Key, uint32_t, Hash> unique;
    std::vector<BlockVertex> vertices;
    for (auto& index : mesh.indices) {
        const auto& v = mesh.vertices[index];
        const auto [it, inserted] =
            unique.emplace(std::bit_cast<Key>(v), uint32_t(vertices.size()));
        if (inserted) vertices.push_back(v);
        index = it->second;
    }
    mesh.vertices = std::move(vertices);
    return mesh;
}
}  // namespace TerrainLod
