#include "fluid_cell.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace fluid {
namespace {

constexpr float Tolerance = 1e-5f;
constexpr float Nan = std::numeric_limits<float>::quiet_NaN();

constexpr Face opposite(Face face) { return Face(uint8_t(face) ^ 1); }

Vec3 faceNormal(Face face) {
    switch (face) {
        case Face::PosX:
            return {1, 0, 0};
        case Face::NegX:
            return {-1, 0, 0};
        case Face::PosY:
            return {0, 1, 0};
        case Face::NegY:
            return {0, -1, 0};
        case Face::PosZ:
            return {0, 0, 1};
        case Face::NegZ:
            return {0, 0, -1};
    }
    return {};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float polygonArea(std::span<const Vec3> polygon) {
    Vec3 area{};
    for (size_t i = 2; i < polygon.size(); ++i)
        area =
            area + cross(polygon[i - 1] - polygon[0], polygon[i] - polygon[0]);
    const float twice =
        std::sqrt(area.x * area.x + area.y * area.y + area.z * area.z);
    return twice * .5f;
}

}  // namespace

Profile Profile::open() {
    Profile profile;
    profile.uniform = true;
    for (int i = 0; i <= Buckets; ++i) profile.volume[i] = float(i) / Buckets;
    for (int i = 0; i < 6; ++i) {
        profile.openingLowest[i] = 0.f;
        profile.openingArea[i] = 1.f;
    }
    return profile;
}

Profile Profile::closed() {
    Profile profile;
    profile.uniform = false;
    profile.ceiling = 0.f;
    profile.capacity = 0.f;
    profile.volume.fill(0.f);
    for (int i = 0; i < 6; ++i) {
        profile.openingLowest[i] = Nan;
        profile.openingArea[i] = 0.f;
    }
    return profile;
}

Profile Profile::make(const tile_geometry::CellGeometry& geometry) {
    Profile profile = open();
    profile.uniform = false;
    for (int i = 0; i <= Buckets; ++i)
        profile.volume[i] = float(geometry.freeVolumeBelow(float(i) / Buckets));

    // A full-width horizontal solid makes freeVolumeBelow constant. The
    // lowest such plate above the bottom run seals everything above it from
    // water resting on the cell floor. A solid at the floor (a bottom slab)
    // is not a ceiling: its free run simply starts higher.
    bool sealed = false;
    for (int i = 1; i <= Buckets; ++i) {
        if (profile.volume[i] <= 1e-6f) continue;
        int end = i;
        while (end < Buckets &&
               profile.volume[end + 1] - profile.volume[end] <= 1e-6f)
            ++end;
        // A plateau needs at least one following sample with the same
        // volume. A single nonzero bucket followed by growth is just the
        // first slice of an open or sloping run, not a ceiling.
        if (end > i) {
            profile.ceiling = float(i) / Buckets;
            profile.capacity = profile.volume[i];
            sealed = true;
            break;
        }
    }
    if (!sealed) {
        profile.ceiling = 1.f;
        profile.capacity = profile.volume[Buckets];
    }
    if (profile.capacity <= 1e-6f) {
        profile.capacity = 0.f;
        profile.ceiling = 0.f;
    }
    for (int i = 0; i <= Buckets; ++i)
        if (float(i) / Buckets >= profile.ceiling)
            profile.volume[i] = profile.capacity;

    for (int f = 0; f < 6; ++f) {
        const auto face = Face(f);
        float lowest = std::numeric_limits<float>::infinity();
        float area = 0;
        for (const auto& polygon : geometry.boundaryOpenings(face)) {
            const float piece = polygonArea(polygon);
            if (piece <= Tolerance) continue;
            area += piece;
            for (const auto point : polygon) lowest = std::min(lowest, point.y);
        }
        profile.openingArea[f] = std::clamp(area, 0.f, 1.f);
        profile.openingLowest[f] =
            profile.openingArea[f] > Tolerance && std::isfinite(lowest) ? lowest
                                                                        : Nan;
    }
    return profile;
}

bool Profile::closed(Face face) const { return !hasOpening(face); }

bool Profile::hasOpening(Face face) const {
    const auto i = size_t(uint8_t(face));
    return openingArea[i] > Tolerance && std::isfinite(openingLowest[i]);
}

float Profile::volumeAt(float level) const {
    if (!(level > 0)) return 0;
    if (level >= ceiling) return capacity;
    if (uniform) return level;
    const float scaled = level * Buckets;
    const int low = std::clamp(int(std::floor(scaled)), 0, Buckets);
    if (low >= Buckets) return capacity;
    const int high = low + 1;
    const float t = scaled - low;
    return volume[low] + (volume[high] - volume[low]) * t;
}

float Profile::levelFor(float requested) const {
    if (requested <= 0) return 0;
    if (uniform) return std::clamp(requested, 0.f, capacity);
    if (requested >= capacity) return ceiling;
    float low = 0, high = ceiling;
    for (int i = 0; i < 24; ++i) {
        const float mid = (low + high) * .5f;
        if (volumeAt(mid) < requested)
            low = mid;
        else
            high = mid;
    }
    return (low + high) * .5f;
}

int Profile::unitsAt(float level) const {
    // Units are absolute volume: 255 units is one cubic metre. A cell with a
    // reduced free volume therefore tops out below 255.
    return std::clamp(int(std::lround(255 * volumeAt(level))), 0, 255);
}

float Profile::levelForUnits(int units) const {
    if (capacity <= 1e-6f) return 0;
    return levelFor(std::clamp(units, 0, 255) / 255.f);
}

std::optional<float> sharedOpening(const Profile& a, const Profile& b,
                                   Face face) {
    if (!a.hasOpening(face) || !b.hasOpening(opposite(face)))
        return std::nullopt;
    // Two open faces may still miss each other when a partial piece covers
    // complementary parts of the shared square. The opening profiles retain
    // area and the lowest point, not horizontal extent. Require the smaller
    // open area to be non-zero; a full-face opener passes the other side's
    // own opening. This is deliberately conservative for V1.
    const auto i = size_t(uint8_t(face));
    const auto j = size_t(uint8_t(opposite(face)));
    const float lowest = std::max(a.openingLowest[i], b.openingLowest[j]);
    const float area =
        std::min(a.openingArea[i], b.openingArea[j]);  // conservative
    // A full-face opener on either side leaves the other side's own area as
    // the true opening; otherwise use the smaller area.
    const float overlap = a.openingArea[i] >= 1.f - Tolerance ||
                                  b.openingArea[j] >= 1.f - Tolerance
                              ? std::max(a.openingArea[i], b.openingArea[j])
                              : area;
    if (overlap <= Tolerance) return std::nullopt;
    return lowest;
}

}  // namespace fluid
