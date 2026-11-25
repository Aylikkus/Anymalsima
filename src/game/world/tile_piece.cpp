#include "tile_piece.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace tile_geometry {
namespace {

using Vertices = std::vector<Vec3>;

// The original profile planes use float unit normals. Treat sub-micrometre
// distances as contact when clipping shared faces or testing for interiors.
constexpr float Tolerance = 1e-6f;
constexpr std::array<Plane, 6> cellPlanes{{
    {{1, 0, 0}, 1},
    {{-1, 0, 0}, 0},
    {{0, 1, 0}, 1},
    {{0, -1, 0}, 0},
    {{0, 0, 1}, 1},
    {{0, 0, -1}, 0},
}};

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float length(Vec3 vector) { return std::sqrt(dot(vector, vector)); }

float distance(Plane plane, Vec3 point) {
    return dot(plane.normal, point) - plane.offset;
}

Plane flipped(Plane plane) { return {plane.normal * -1, -plane.offset}; }

bool sameDirection(Vec3 a, Vec3 b) {
    const auto delta = a - b;
    return dot(delta, delta) <= Tolerance * Tolerance;
}

bool coincides(Plane a, Plane b) {
    return sameDirection(a.normal, b.normal) &&
           std::abs(a.offset - b.offset) <= Tolerance;
}

// Parallel planes with the same outward normal only need the tighter bound.
void addPlane(std::vector<Plane>& planes, Plane plane) {
    for (auto& existing : planes) {
        if (sameDirection(existing.normal, plane.normal)) {
            if (plane.offset < existing.offset) existing = plane;
            return;
        }
    }
    planes.push_back(plane);
}

Vertices clip(const Vertices& polygon, Plane plane) {
    Vertices result;
    if (polygon.empty()) return result;
    const auto snappedDistance = [&](Vec3 point) {
        const float d = distance(plane, point);
        return std::abs(d) <= Tolerance ? 0.f : d;
    };
    auto previous = polygon.back();
    float before = snappedDistance(previous);
    for (auto point : polygon) {
        const float after = snappedDistance(point);
        if ((before < 0 && after > 0) || (before > 0 && after < 0))
            result.push_back(previous +
                             (point - previous) * (before / (before - after)));
        if (after <= 0) result.push_back(point);
        previous = point;
        before = after;
    }
    return result;
}

Vec3 areaVector(const Vertices& polygon) {
    Vec3 area{};
    for (size_t i = 2; i < polygon.size(); ++i)
        area =
            area + cross(polygon[i - 1] - polygon[0], polygon[i] - polygon[0]);
    return area;  // Twice the area, along the polygon's outward normal.
}

bool hasArea(const Vertices& polygon) {
    return length(areaVector(polygon)) > 2 * Tolerance * Tolerance;
}

Vertices planeSquare(Plane plane, float halfSide) {
    const Vec3 axis =
        std::abs(plane.normal.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    auto u = cross(axis, plane.normal);
    u = u * (halfSide / length(u));
    const auto v = cross(plane.normal, u);
    const auto centre = plane.normal * plane.offset;
    return {centre - u - v, centre + u - v, centre + u + v, centre - u + v};
}

Vertices facet(Plane plane, std::span<const Plane> bounds) {
    // The projected unit cell fits in this square on every candidate plane.
    auto polygon = planeSquare(plane, 2);
    for (auto bound : bounds) {
        if (coincides(bound, plane)) continue;
        polygon = clip(polygon, bound);
        if (polygon.size() < 3) return {};
    }
    return polygon;
}

bool hasInterior(std::span<const Plane> bounds) {
    // The intersection is bounded by the cell. It has a strict interior iff
    // every plane has a vertex strictly inside it. Averaging those witnesses
    // would give a point strictly inside every plane at once.
    std::vector<bool> inside(bounds.size());
    for (auto plane : bounds) {
        const auto polygon = facet(plane, bounds);
        for (size_t i = 0; i < bounds.size(); ++i) {
            inside[i] =
                inside[i] ||
                std::any_of(polygon.begin(), polygon.end(), [&](Vec3 p) {
                    return distance(bounds[i], p) < -Tolerance;
                });
        }
        if (std::ranges::all_of(inside, [](bool value) { return value; }))
            return true;
    }
    return false;
}

std::vector<Vertices> subtract(Vertices polygon,
                               std::span<const Plane> bounds) {
    auto covered = polygon;
    for (auto bound : bounds) {
        covered = clip(covered, bound);
        if (!hasArea(covered)) return {std::move(polygon)};
    }

    // Each fragment fails the first bound it falls outside. The fragments
    // cover the difference without overlapping interiors.
    std::vector<Vertices> result;
    for (auto bound : bounds) {
        if (std::ranges::none_of(polygon, [&](Vec3 p) {
                return distance(bound, p) > Tolerance;
            }))
            continue;
        auto outside = clip(polygon, flipped(bound));
        if (hasArea(outside)) result.push_back(std::move(outside));
        polygon = clip(polygon, bound);
    }
    return result;
}

void subtractFrom(std::vector<Vertices>& polygons,
                  std::span<const Plane> bounds) {
    std::vector<Vertices> result;
    for (auto& polygon : polygons) {
        auto remainder = subtract(std::move(polygon), bounds);
        for (auto& fragment : remainder) result.push_back(std::move(fragment));
    }
    polygons = std::move(result);
}

// Volume of the convex intersection of outward half-spaces. Each face is
// oriented away from the interior, so the closed-surface integral gives the
// enclosed volume independently of the origin.
double convexVolume(std::span<const Plane> bounds) {
    if (!hasInterior(bounds)) return 0;
    std::vector<Vertices> faces;
    for (auto plane : bounds) {
        auto polygon = facet(plane, bounds);
        if (!hasArea(polygon)) continue;
        if (dot(areaVector(polygon), plane.normal) < 0)
            std::reverse(polygon.begin(), polygon.end());
        faces.push_back(std::move(polygon));
    }
    double sixTimesVolume = 0;
    for (const auto& polygon : faces)
        for (size_t i = 1; i + 1 < polygon.size(); ++i)
            sixTimesVolume +=
                dot(polygon[0], cross(polygon[i], polygon[i + 1]));
    return std::abs(sixTimesVolume) / 6;
}

bool insideCell(Vec2 xz) {
    return xz.x >= 0 && xz.x <= 1 && xz.y >= 0 && xz.y <= 1;
}

bool containsPoint(std::span<const Plane> bounds, Vec3 point) {
    return std::ranges::all_of(bounds, [&](Plane plane) {
        return distance(plane, point) <= Tolerance;
    });
}

bool onFace(const PieceFace& face, Vec3 point) {
    for (size_t i = 0; i < face.vertices.size(); ++i) {
        const auto start = face.vertices[i];
        const auto end = face.vertices[(i + 1) % face.vertices.size()];
        const auto edge = end - start;
        if (dot(cross(edge, point - start), face.plane.normal) <
            -Tolerance * length(edge))
            return false;
    }
    return true;
}

}  // namespace

std::optional<Face> boundaryFace(Plane plane) {
    for (size_t i = 0; i < cellPlanes.size(); ++i)
        if (coincides(cellPlanes[i], plane)) return Face(i);
    return std::nullopt;
}

std::vector<Vec3> clipToFace(std::span<const Vec3> polygon,
                             std::span<const Vec3> boundary, Vec3 normal) {
    Vertices result(polygon.begin(), polygon.end());
    for (size_t i = 0; i < boundary.size() && !result.empty(); ++i) {
        const auto start = boundary[i];
        const auto edge = boundary[(i + 1) % boundary.size()] - start;
        const auto tangent = cross(edge, normal);
        const float size = length(tangent);
        if (size <= Tolerance) continue;
        const auto outward = tangent * (1 / size);
        result = clip(result, {outward, dot(outward, start)});
    }
    return result;
}

TilePiece TilePiece::layer(PieceId id, uint8_t blockId,
                           LayerThickness thickness, Vec3 offset) {
    const bool slab = thickness == LayerThickness::Cm50;
    return {
        id,
        blockId,
        TileShape::make(slab ? TileShapeProfile::Slab : TileShapeProfile::Cube),
        offset,
        PieceFill::BelowProfile,
        slab ? 1.f : float(uint8_t(thickness)) / 100};
}

std::optional<TileShape> TilePiece::legacyShape() const {
    // Drop a redundant cap and use the existing slab for a half-height cube.
    if (offset != Vec3{} || fill != PieceFill::BelowProfile)
        return std::nullopt;
    if (ceiling == 1) return shape;
    if (shape.profile() == TileShapeProfile::Cube && ceiling == .5f)
        return TileShape::make(TileShapeProfile::Slab);
    const auto height = tile_geometry::supportHeight(shape, Vec2{}, Vec2{1, 1});
    return height && ceiling >= *height ? std::optional{shape} : std::nullopt;
}

PiecePlacement CellGeometry::tryAdd(TilePiece piece) {
    if (piece.blockId == 0 || !std::isfinite(piece.offset.x) ||
        !std::isfinite(piece.offset.y) || !std::isfinite(piece.offset.z) ||
        !(piece.ceiling > 0 && piece.ceiling <= 1) ||
        (piece.fill != PieceFill::BelowProfile &&
         piece.fill != PieceFill::AboveProfile))
        return PiecePlacement::Invalid;
    const auto insertion =
        std::ranges::lower_bound(solids_, piece.id, {}, &Solid::id);
    if (insertion != solids_.end() && insertion->id == piece.id)
        return PiecePlacement::DuplicateId;
    if (std::max({std::abs(piece.offset.x), std::abs(piece.offset.y),
                  std::abs(piece.offset.z)}) >= 1)
        return PiecePlacement::Empty;

    Solid solid{piece.id, piece.blockId, {}};
    const auto profile = convexParts(piece.shape);
    for (unsigned i = 0; i < profile.count; ++i) {
        const auto& part = profile.parts[i];
        std::vector<Plane> bounds(cellPlanes.begin(), cellPlanes.end());
        addPlane(bounds, {{0, 1, 0}, piece.ceiling + piece.offset.y});
        for (unsigned j = 0; j < part.count; ++j) {
            auto plane = part.planes[j];
            if (j == 0 && piece.fill == PieceFill::AboveProfile)
                plane = flipped(plane);
            plane.offset += dot(plane.normal, piece.offset);
            addPlane(bounds, plane);
        }
        if (hasInterior(bounds)) solid.parts.push_back(std::move(bounds));
    }
    if (solid.parts.empty()) return PiecePlacement::Empty;

    for (const auto& other : solids_) {
        for (const auto& part : solid.parts) {
            for (const auto& otherPart : other.parts) {
                auto intersection = part;
                for (auto plane : otherPart) addPlane(intersection, plane);
                if (hasInterior(intersection)) return PiecePlacement::Overlap;
            }
        }
    }

    solids_.insert(insertion, std::move(solid));
    return PiecePlacement::Added;
}

bool CellGeometry::contains(Vec3 point) const {
    if (!insideCell({point.x, point.z}) || !(point.y >= 0 && point.y <= 1))
        return false;
    for (const auto& solid : solids_)
        for (const auto& part : solid.parts)
            if (containsPoint(part, point)) return true;
    return false;
}

bool CellGeometry::intersectsBox(Vec3 low, Vec3 high) const {
    const std::array<Plane, 6> bounds{{
        {{1, 0, 0}, high.x},
        {{-1, 0, 0}, -low.x},
        {{0, 1, 0}, high.y},
        {{0, -1, 0}, -low.y},
        {{0, 0, 1}, high.z},
        {{0, 0, -1}, -low.z},
    }};
    for (const auto& solid : solids_)
        for (const auto& part : solid.parts) {
            auto intersection = part;
            for (auto plane : bounds) addPlane(intersection, plane);
            if (hasInterior(intersection)) return true;
        }
    return false;
}

std::vector<std::vector<Vec3>> CellGeometry::overheadOpenings() const {
    std::vector<Vertices> openings{
        {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}};
    for (const auto& solid : solids_)
        for (const auto& part : solid.parts)
            for (auto plane : part) {
                if (plane.normal.y <= 0) continue;
                const auto polygon = facet(plane, part);
                if (areaVector(polygon).y <= 2 * Tolerance * Tolerance)
                    continue;
                std::vector<Plane> bounds;
                for (size_t i = 0; i < polygon.size(); ++i) {
                    const auto point = polygon[i];
                    auto edge = polygon[(i + 1) % polygon.size()] - point;
                    edge.y = 0;
                    const float size = length(edge);
                    if (size <= Tolerance) continue;
                    const auto normal = cross(edge, {0, 1, 0}) * (1 / size);
                    bounds.push_back({normal, dot(normal, point)});
                }
                subtractFrom(openings, bounds);
            }
    return openings;
}

std::vector<PieceFace> CellGeometry::exteriorFaces() const {
    std::vector<PieceFace> result;
    for (const auto& solid : solids_) {
        for (const auto& part : solid.parts) {
            for (auto plane : part) {
                auto polygon = facet(plane, part);
                if (!hasArea(polygon)) continue;
                std::vector<Vertices> fragments;
                fragments.push_back(std::move(polygon));
                // Valid interiors are disjoint. Only a part with a face
                // opposing this one can hide an area of it. This also removes
                // the diagonal split inside a legacy corner profile.
                for (const auto& other : solids_) {
                    for (const auto& otherPart : other.parts) {
                        if (&part == &otherPart) continue;
                        if (std::ranges::any_of(otherPart, [&](Plane bound) {
                                return coincides(bound, flipped(plane));
                            }))
                            subtractFrom(fragments, otherPart);
                    }
                }
                for (auto& fragment : fragments)
                    result.push_back(
                        {solid.id, solid.blockId, plane, std::move(fragment)});
            }
        }
    }
    return result;
}

std::vector<std::vector<Vec3>> CellGeometry::boundaryOpenings(Face face) const {
    const auto plane = cellPlanes[uint8_t(face)];
    // Translate the square tangentially from the plane origin to face centre.
    auto square = planeSquare(plane, .5f);
    const Vec3 centre{.5f, .5f, .5f};
    const auto tangent = centre - plane.normal * dot(plane.normal, centre);
    for (auto& point : square) point = point + tangent;
    std::vector<Vertices> openings;
    openings.push_back(std::move(square));
    for (const auto& solid : solids_)
        for (const auto& part : solid.parts) subtractFrom(openings, part);
    return openings;
}

std::vector<VerticalInterval> CellGeometry::freeIntervals(Vec2 xz) const {
    if (!insideCell(xz)) return {};
    std::vector<VerticalInterval> occupied;
    for (const auto& solid : solids_) {
        for (const auto& part : solid.parts) {
            VerticalInterval interval{0, 1};
            for (auto plane : part) {
                const float d = distance(plane, {xz.x, 0, xz.y});
                if (plane.normal.y > 0)
                    interval.high =
                        std::min(interval.high, -d / plane.normal.y);
                else if (plane.normal.y < 0)
                    interval.low = std::max(interval.low, -d / plane.normal.y);
                else if (d > Tolerance)
                    interval.high = 0;
            }
            if (interval.high - interval.low > Tolerance)
                occupied.push_back(interval);
        }
    }
    std::ranges::sort(occupied, {}, &VerticalInterval::low);
    std::vector<VerticalInterval> gaps;
    float cursor = 0;
    for (auto interval : occupied) {
        if (interval.low - cursor > Tolerance)
            gaps.push_back({cursor, interval.low});
        cursor = std::max(cursor, interval.high);
    }
    if (1 - cursor > Tolerance) gaps.push_back({cursor, 1});
    return gaps;
}

std::optional<float> CellGeometry::supportHeight(Vec2 low, Vec2 high,
                                                 float maxHeight) const {
    if (!(low.x < high.x && low.y < high.y && maxHeight >= 0))
        return std::nullopt;
    const std::array<Plane, 5> bounds{{
        {{-1, 0, 0}, -low.x},
        {{1, 0, 0}, high.x},
        {{0, 0, -1}, -low.y},
        {{0, 0, 1}, high.y},
        {{0, 1, 0}, maxHeight},
    }};
    std::optional<float> highest;
    for (auto& face : exteriorFaces()) {
        if (face.plane.normal.y <= 0) continue;
        auto polygon = std::move(face.vertices);
        for (auto plane : bounds) polygon = clip(polygon, plane);
        if (areaVector(polygon).y <= 2 * Tolerance * Tolerance) continue;
        for (auto point : polygon) {
            const float height = std::min(point.y, maxHeight);
            if (!highest || height > *highest) highest = height;
        }
    }
    return highest;
}

double CellGeometry::freeVolumeBelow(float level) const {
    if (!(level > 0)) return 0;
    level = std::min(level, 1.f);
    double solid = 0;
    for (const auto& solidEntry : solids_) {
        for (const auto& part : solidEntry.parts) {
            auto clipped = part;
            addPlane(clipped, {{0, 1, 0}, level});
            solid += convexVolume(clipped);
        }
    }
    return std::max(0.0, double(level) - solid);
}

bool CellGeometry::isFull() const { return freeVolumeBelow(1) <= 1e-7; }

std::optional<float> CellGeometry::lowestUpwardSupport() const {
    std::optional<float> lowest;
    for (const auto& face : exteriorFaces()) {
        if (face.plane.normal.y <= Tolerance) continue;
        for (auto point : face.vertices)
            if (!lowest || point.y < *lowest) lowest = point.y;
    }
    return lowest;
}

std::optional<float> CellGeometry::lowestOpening(Face face) const {
    std::optional<float> lowest;
    for (const auto& polygon : boundaryOpenings(face))
        for (auto point : polygon)
            if (!lowest || point.y < *lowest) lowest = point.y;
    return lowest;
}

std::optional<PieceRayHit> CellGeometry::intersectRay(Vec3 origin,
                                                      Vec3 direction,
                                                      float tMin,
                                                      float tMax) const {
    if (!(tMin <= tMax) || !std::isfinite(tMin)) return std::nullopt;
    const auto start = origin + direction * tMin;
    const Solid* containing = nullptr;
    if (insideCell({start.x, start.z}) && start.y >= 0 && start.y <= 1) {
        for (const auto& solid : solids_) {
            if (std::ranges::any_of(solid.parts, [&](const auto& part) {
                    return containsPoint(part, start);
                })) {
                containing = &solid;
                break;
            }
        }
    }
    std::optional<PieceRayHit> hit;
    const float raySpeed = length(direction);
    for (const auto& face : exteriorFaces()) {
        const float speed = dot(face.plane.normal, direction);
        if (speed >= 0) continue;
        const float d = distance(face.plane, start);
        const float t = std::abs(d) <= Tolerance ? tMin : tMin - d / speed;
        if (!std::isfinite(t) || !(t >= tMin && t <= tMax) ||
            (containing && t > tMin))
            continue;
        if (hit &&
            (t >= hit->hit.t ||
             (face.id != hit->id && (hit->hit.t - t) * raySpeed <= Tolerance)))
            continue;
        if (!onFace(face, origin + direction * t)) continue;
        hit = PieceRayHit{face.id, face.blockId, {t, face.plane.normal}};
    }
    if (!hit && containing)
        return PieceRayHit{containing->id, containing->blockId, {tMin, {}}};
    return hit;
}

}  // namespace tile_geometry
