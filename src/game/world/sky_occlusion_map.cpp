#include "sky_occlusion_map.hpp"

#include <algorithm>

#include "world_wrap.hpp"

float SkyOcclusionMap::skyAtDistance(int k) {
    if (k <= 0) return 1.f;
    const float tiles = k * TilesPerTexel - (TilesPerTexel - 1) * 0.5f;
    return std::clamp((MaxLight - tiles) / MaxLight, 0.f, 1.f);
}

void SkyOcclusionMap::build(
    const std::vector<structure_voxels::FootprintPtr>& footprints, double eyeX,
    double eyeZ, int periodX) {
    centreX = snapped(eyeX);
    centreZ = snapped(eyeZ);
    minX = centreX - Extent / 2.0;
    minZ = centreZ - Extent / 2.0;
    built = true;
    covered = 0;
    data.assign(size_t(Size) * Size * Thresholds, Open);
    std::vector<float> top(size_t(Size) * Size, Open);
    int bx0 = Size, bz0 = Size, bx1 = -1, bz1 = -1;
    for (const auto& footprint : footprints) {
        if (!footprint) continue;
        for (const auto& [key, t] : footprint->tops) {
            const auto [cx, cz] =
                structure_voxels::Footprint::unpackColumn(key);
            // The cell's copy nearest the centre (X wraps).
            const double x =
                centreX + WorldWrap::deltaf(
                              double(cx) * TilesPerTexel + TilesPerTexel / 2.0,
                              centreX, periodX);
            const int tx = int(std::floor((x - minX) / TilesPerTexel));
            const int tz = int(std::floor(
                (double(cz) * TilesPerTexel + TilesPerTexel / 2.0 - minZ) /
                TilesPerTexel));
            if (tx < 0 || tz < 0 || tx >= Size || tz >= Size) continue;
            float& v = top[size_t(tz) * Size + tx];
            if (v == Open) ++covered;
            v = std::max(v, float(t));
            bx0 = std::min(bx0, tx);
            bx1 = std::max(bx1, tx);
            bz0 = std::min(bz0, tz);
            bz1 = std::max(bz1, tz);
        }
    }
    if (bx1 < 0) return;
    // Threshold k: the lowest top within k texels (4-neighbour), by k
    // passes of a cross-shaped min filter, around the occupied rectangle.
    constexpr int Reach = Thresholds - 1;
    bx0 = std::max(0, bx0 - Reach);
    bz0 = std::max(0, bz0 - Reach);
    bx1 = std::min(Size - 1, bx1 + Reach);
    bz1 = std::min(Size - 1, bz1 + Reach);
    std::vector<float> next = top;
    for (int k = 0; k < Thresholds; ++k) {
        if (k > 0) {
            for (int z = bz0; z <= bz1; ++z)
                for (int x = bx0; x <= bx1; ++x) {
                    const size_t i = size_t(z) * Size + x;
                    float v = top[i];
                    if (x > 0) v = std::min(v, top[i - 1]);
                    if (x + 1 < Size) v = std::min(v, top[i + 1]);
                    if (z > 0) v = std::min(v, top[i - Size]);
                    if (z + 1 < Size) v = std::min(v, top[i + Size]);
                    next[i] = v;
                }
            std::swap(top, next);
        }
        for (int z = bz0; z <= bz1; ++z)
            for (int x = bx0; x <= bx1; ++x) {
                const size_t i = size_t(z) * Size + x;
                data[i * Thresholds + k] = top[i];
            }
    }
}

std::array<float, SkyOcclusionMap::Thresholds> SkyOcclusionMap::at(
    double x, double z) const {
    std::array<float, Thresholds> out;
    out.fill(Open);
    if (!built) return out;
    const int tx = int(std::floor((x - minX) / TilesPerTexel));
    const int tz = int(std::floor((z - minZ) / TilesPerTexel));
    if (tx < 0 || tz < 0 || tx >= Size || tz >= Size) return out;
    const size_t i = (size_t(tz) * Size + tx) * Thresholds;
    for (int k = 0; k < Thresholds; ++k) out[size_t(k)] = data[i + k];
    return out;
}

float SkyOcclusionMap::sky(const std::array<float, Thresholds>& texel,
                           double y) {
    if (texel[0] <= Open) return 1.f;
    // above[k]: how far the point is above threshold k (smoothstep, as the
    // shaders); non-decreasing in k, since thresholds only fall.
    float out = 0, previous = 0;
    for (int k = 0; k < Thresholds; ++k) {
        const float a = texel[size_t(k)] - Fade[0],
                    b = texel[size_t(k)] - Fade[1];
        const float t = std::clamp(float((y - a) / (b - a)), 0.f, 1.f);
        const float above = std::max(previous, t * t * (3 - 2 * t));
        out += (above - previous) * skyAtDistance(k);
        previous = above;
    }
    return out + (1.f - previous) * skyAtDistance(Thresholds);
}

float SkyOcclusionMap::sky(double x, double y, double z) const {
    return sky(at(x, z), y);
}
