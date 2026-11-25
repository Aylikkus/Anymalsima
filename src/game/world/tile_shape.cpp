#include "tile_shape.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace tile_geometry {
namespace {

enum class Top { Flat, Straight, Outer, Inner, Diagonal };

struct Profile {
    int low;
    int run;
    Top top = Top::Flat;
    float zMin = 0;

    bool corner() const { return top == Top::Outer || top == Top::Inner; }

    float height(float x, float z) const {
        float rise = 0;
        switch (top) {
            case Top::Flat:
                break;
            case Top::Straight:
                rise = z;
                break;
            case Top::Outer:
                rise = std::min(x, z);
                break;
            case Top::Inner:
                rise = std::max(x, z);
                break;
            case Top::Diagonal:
                rise = (x + z) * .5f;
                break;
        }
        return (float(low) + rise) / float(run);
    }
};

// Indices are persisted profile IDs. Integer intervals give adjacent pieces
// identical endpoint calculations, including when they belong to a junction.
constexpr std::array<Profile, 25> profiles{{
    {1, 1},
    {1, 2},
    {0, 1, Top::Straight},
    {0, 3, Top::Straight},
    {1, 3, Top::Straight},
    {2, 3, Top::Straight},
    {1, 1, Top::Flat, .90f},
    {1, 1, Top::Flat, .75f},
    {1, 1, Top::Flat, .50f},
    {1, 1, Top::Flat, .25f},
    {0, 2, Top::Straight},
    {1, 2, Top::Straight},
    {0, 1, Top::Outer},
    {0, 2, Top::Outer},
    {1, 2, Top::Outer},
    {0, 3, Top::Outer},
    {1, 3, Top::Outer},
    {2, 3, Top::Outer},
    {0, 1, Top::Inner},
    {0, 2, Top::Inner},
    {1, 2, Top::Inner},
    {0, 3, Top::Inner},
    {1, 3, Top::Inner},
    {2, 3, Top::Inner},
    {0, 1, Top::Diagonal},
}};

constexpr std::array<Vec3, 6> faceNormals{{
    {1, 0, 0},
    {-1, 0, 0},
    {0, 1, 0},
    {0, -1, 0},
    {0, 0, 1},
    {0, 0, -1},
}};

float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

Vec3 rotateVector(Vec3 v, TileFacing facing) {
    switch (facing) {
        case TileFacing::PosZ:
            return v;
        case TileFacing::PosX:
            return {v.z, v.y, -v.x};
        case TileFacing::NegZ:
            return {-v.x, v.y, -v.z};
        case TileFacing::NegX:
            return {-v.z, v.y, v.x};
    }
    return v;
}

Vec3 toCanonical(TileShape shape, Vec3 point) {
    return rotatePoint(point, TileFacing((-uint8_t(shape.facing())) & 3));
}

void addFace(Surface& out, TileFacing facing,
             std::initializer_list<Vec3> points) {
    Polygon polygon;
    for (auto point : points) {
        point = rotatePoint(point, facing);
        if (polygon.count == 0 || polygon.vertices[polygon.count - 1] != point)
            polygon.vertices[polygon.count++] = point;
    }
    if (polygon.vertices[polygon.count - 1] == polygon.vertices[0])
        --polygon.count;
    if (polygon.count < 3) return;
    const auto a = polygon.vertices[0];
    const auto normal = cross(polygon.vertices[1] - a, polygon.vertices[2] - a);
    polygon.plane.normal = normal * (1 / std::sqrt(dot(normal, normal)));
    polygon.plane.offset = dot(polygon.plane.normal, a);
    out.faces[out.count++] = polygon;
}

// The convex parts extend the perimeter planes through the full footprint.
// For a corner, each also uses one top plane and one diagonal plane.
void addPlane(ConvexPart& part, TileFacing facing, Vec3 normal, Vec3 point) {
    normal = rotateVector(normal, facing);
    normal = normal * (1 / std::sqrt(dot(normal, normal)));
    part.planes[part.count++] = {normal,
                                 dot(normal, rotatePoint(point, facing))};
}

}  // namespace

Vec3 rotatePoint(Vec3 p, TileFacing facing) {
    switch (facing) {
        case TileFacing::PosZ:
            return p;
        case TileFacing::PosX:
            return {p.z, p.y, 1 - p.x};
        case TileFacing::NegZ:
            return {1 - p.x, p.y, 1 - p.z};
        case TileFacing::NegX:
            return {1 - p.z, p.y, p.x};
    }
    return p;
}

std::optional<float> supportHeight(TileShape shape, float x, float z) {
    if (!(x >= 0 && x <= 1 && z >= 0 && z <= 1)) return std::nullopt;
    const auto local = toCanonical(shape, {x, 0, z});
    const auto& profile = profiles[uint8_t(shape.profile())];
    if (!(local.x >= 0 && local.x <= 1 && local.z >= profile.zMin &&
          local.z <= 1))
        return std::nullopt;
    return profile.height(local.x, local.z);
}

std::optional<float> supportHeight(TileShape shape, Vec2 low, Vec2 high) {
    const auto a = toCanonical(shape, {low.x, 0, low.y});
    const auto b = toCanonical(shape, {high.x, 0, high.y});
    const auto& profile = profiles[uint8_t(shape.profile())];
    const float x = std::min(1.f, std::max(a.x, b.x));
    const float z = std::min(1.f, std::max(a.z, b.z));
    if (x <= std::max(0.f, std::min(a.x, b.x)) ||
        z <= std::max(profile.zMin, std::min(a.z, b.z)))
        return std::nullopt;
    // Every top increases (or stays level) in both canonical axes. This
    // evaluates the maximum over a box, wedge, or the two junction wedges.
    return profile.height(x, z);
}

bool contains(TileShape shape, Vec3 point) {
    const auto height = supportHeight(shape, point.x, point.z);
    return height && point.y >= 0 && point.y <= *height;
}

Surface surface(TileShape shape) {
    const auto& profile = profiles[uint8_t(shape.profile())];
    const auto facing = shape.facing();
    const std::array<Vec3, 4> bottom{{
        {0, 0, profile.zMin},
        {1, 0, profile.zMin},
        {1, 0, 1},
        {0, 0, 1},
    }};
    auto top = bottom;
    for (auto& p : top) p.y = profile.height(p.x, p.z);

    Surface result;
    addFace(result, facing, {bottom[0], bottom[1], bottom[2], bottom[3]});
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned j = (i + 1) % 4;
        addFace(result, facing, {bottom[i], top[i], top[j], bottom[j]});
    }
    if (profile.corner()) {
        addFace(result, facing, {top[0], top[2], top[1]});
        addFace(result, facing, {top[0], top[3], top[2]});
    } else {
        addFace(result, facing, {top[0], top[3], top[2], top[1]});
    }
    return result;
}

ConvexParts convexParts(TileShape shape) {
    const auto& profile = profiles[uint8_t(shape.profile())];
    const auto facing = shape.facing();
    ConvexParts result;
    result.count = profile.corner() ? 2 : 1;
    for (unsigned i = 0; i < result.count; ++i) {
        auto& part = result.parts[i];
        const float slope = profile.top == Top::Flat ? 0 : 1.f / profile.run;
        const bool risesX =
            profile.corner() && ((i == 0) == (profile.top == Top::Inner));
        // Keep the top first for a stable normal at ties with a perimeter face.
        if (profile.top == Top::Diagonal)
            addPlane(part, facing, {-.5f, 1, -.5f},
                     {0, profile.height(0, 0), 0});
        else
            addPlane(part, facing,
                     {risesX ? -slope : 0, 1, risesX ? 0 : -slope},
                     {0, profile.height(0, 0), 0});
        addPlane(part, facing, {0, -1, 0}, {0, 0, 0});
        addPlane(part, facing, {-1, 0, 0}, {0, 0, 0});
        addPlane(part, facing, {1, 0, 0}, {1, 0, 0});
        addPlane(part, facing, {0, 0, -1}, {0, 0, profile.zMin});
        addPlane(part, facing, {0, 0, 1}, {0, 0, 1});
        part.exteriorCount = part.count;
        if (profile.corner())
            addPlane(part, facing, i == 0 ? Vec3{-1, 0, 1} : Vec3{1, 0, -1},
                     {0, 0, 0});
    }
    return result;
}

namespace {

bool onBoundary(const Polygon& polygon, Face face) {
    const auto normal = faceNormals[uint8_t(face)];
    const float offset = uint8_t(face) % 2 == 0 ? 1 : 0;
    return dot(polygon.plane.normal, normal) > 0 &&
           std::all_of(polygon.vertices.begin(),
                       polygon.vertices.begin() + polygon.count,
                       [&](Vec3 p) { return dot(normal, p) == offset; });
}

}  // namespace

std::optional<Face> boundaryFace(const Polygon& polygon) {
    for (uint8_t i = 0; i < faceNormals.size(); ++i)
        if (onBoundary(polygon, Face(i))) return Face(i);
    return std::nullopt;
}

std::optional<Polygon> boundaryFace(TileShape shape, Face face) {
    const auto geometry = surface(shape);
    for (unsigned i = 0; i < geometry.count; ++i) {
        const auto& polygon = geometry.faces[i];
        if (onBoundary(polygon, face)) return polygon;
    }
    return std::nullopt;
}

std::optional<RayHit> intersectRay(TileShape shape, Vec3 origin, Vec3 direction,
                                   float tMin, float tMax) {
    if (tMin > tMax) return std::nullopt;
    const bool inside = contains(shape, origin + direction * tMin);
    origin = toCanonical(shape, origin);
    direction =
        rotateVector(direction, TileFacing((-uint8_t(shape.facing())) & 3));
    std::optional<RayHit> hit;
    // Clip in canonical coordinates so the junction's facet tie order stays
    // the same after a quarter turn.
    const auto geometry = convexParts(TileShape::make(shape.profile()));
    for (unsigned i = 0; i < geometry.count; ++i) {
        const auto& part = geometry.parts[i];
        float entry = tMin, exit = hit ? hit->t : tMax;
        const Plane* entering = nullptr;
        for (unsigned j = 0; j < part.count && entry <= exit; ++j) {
            const auto& plane = part.planes[j];
            const float speed = dot(plane.normal, direction);
            const float distance = plane.offset - dot(plane.normal, origin);
            if (speed < 0) {
                const float t = distance / speed;
                if (t > entry) {
                    entry = t;
                    entering = nullptr;
                }
                if (t == entry && !entering && j < part.exteriorCount)
                    entering = &plane;
            } else if (speed > 0) {
                exit = std::min(exit, distance / speed);
            } else if (distance < 0) {
                exit = -std::numeric_limits<float>::infinity();
            }
        }
        if (entry <= exit && entering && (!hit || entry < hit->t))
            hit = RayHit{entry, rotateVector(entering->normal, shape.facing())};
    }
    if (inside && (!hit || hit->t > tMin)) return RayHit{tMin, {0, 0, 0}};
    return hit;
}

}  // namespace tile_geometry
