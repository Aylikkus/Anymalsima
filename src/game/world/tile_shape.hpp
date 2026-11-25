#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#include "core/geometry.hpp"

enum class TileShapeProfile : uint8_t {
    Cube = 0,
    Slab = 1,
    Ramp = 2,
    RampThirdLow = 3,
    RampThirdMiddle = 4,
    RampThirdHigh = 5,
    Wall10 = 6,
    Wall25 = 7,
    Wall50 = 8,
    Wall75 = 9,
    RampHalfLow = 10,
    RampHalfHigh = 11,
    OuterCorner = 12,
    OuterHalfLow = 13,
    OuterHalfHigh = 14,
    OuterThirdLow = 15,
    OuterThirdMiddle = 16,
    OuterThirdHigh = 17,
    InnerCorner = 18,
    InnerHalfLow = 19,
    InnerHalfHigh = 20,
    InnerThirdLow = 21,
    InnerThirdMiddle = 22,
    InnerThirdHigh = 23,
};

enum class TileFacing : uint8_t { PosZ, PosX, NegZ, NegX };

class TileShape {
   public:
    constexpr TileShape() = default;

    static constexpr std::optional<TileShape> fromCode(uint8_t code) {
        const auto profile = code >> 2;
        if (profile > static_cast<uint8_t>(TileShapeProfile::InnerThirdHigh))
            return std::nullopt;
        TileShape shape;
        shape.code_ = profile <= 1 ? uint8_t(profile << 2) : code;
        return shape;
    }

    static constexpr std::optional<TileShape> fromCanonicalCode(uint8_t code) {
        const auto shape = fromCode(code);
        return shape && shape->code() == code ? shape : std::nullopt;
    }

    static constexpr TileShape make(TileShapeProfile profile,
                                    TileFacing facing = TileFacing::PosZ) {
        const auto id = static_cast<uint8_t>(profile);
        if (id > static_cast<uint8_t>(TileShapeProfile::InnerThirdHigh))
            return {};
        return *fromCode(uint8_t((id << 2) | (uint8_t(facing) & 3)));
    }

    constexpr uint8_t code() const { return code_; }
    constexpr TileShapeProfile profile() const {
        return static_cast<TileShapeProfile>(code_ >> 2);
    }
    constexpr TileFacing facing() const {
        return static_cast<TileFacing>(code_ & 3);
    }
    constexpr TileShape rotated(unsigned quarterTurns) const {
        return make(profile(),
                    TileFacing((uint8_t(facing()) + quarterTurns) & 3));
    }
    constexpr bool operator==(const TileShape&) const = default;

   private:
    uint8_t code_ = 0;
};
static_assert(sizeof(TileShape) == 1);

enum class Face : uint8_t { PosX, NegX, PosY, NegY, PosZ, NegZ };

namespace tile_geometry {

struct Plane {
    Vec3 normal;   // outward unit normal
    float offset;  // inside when dot(normal, point) <= offset
};

struct Polygon {
    std::array<Vec3, 4> vertices{};  // CCW from outside
    unsigned count = 0;
    Plane plane{};
};

struct Surface {
    std::array<Polygon, 7> faces{};
    unsigned count = 0;
};

struct ConvexPart {
    // The first exteriorCount planes describe exposed sides. The remaining
    // plane, when present, closes the part at the junction's diagonal.
    std::array<Plane, 7> planes{};
    unsigned exteriorCount = 0;
    unsigned count = 0;
};

struct ConvexParts {
    std::array<ConvexPart, 2> parts{};
    unsigned count = 0;
};

struct RayHit {
    float t;
    Vec3 normal;  // zero if already occupying the ray at tMin
};

// All coordinates are cell-local, with Y up.
Vec3 rotatePoint(Vec3 point, TileFacing facing);
std::optional<float> supportHeight(TileShape shape, float x, float z);
bool contains(TileShape shape, Vec3 point);
Surface surface(TileShape shape);
ConvexParts convexParts(TileShape shape);
// Occupied polygon on a cell boundary, in the same coordinates as surface().
std::optional<Polygon> boundaryFace(TileShape shape, Face face);
// t is in units of the supplied direction vector. Interval endpoints are
// inclusive, including entries exactly at tMin. The diagonal between convex
// parts is never a hit surface.
std::optional<RayHit> intersectRay(
    TileShape shape, Vec3 origin, Vec3 direction, float tMin = 0,
    float tMax = std::numeric_limits<float>::infinity());

}  // namespace tile_geometry
