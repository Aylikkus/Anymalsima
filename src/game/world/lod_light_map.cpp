#include "lod_light_map.hpp"

#include <algorithm>
#include <cmath>

#include "world_wrap.hpp"

bool LodLightMap::stale(double eyeX, double eyeZ,
                        uint64_t registryVersion) const {
    return !built || snapped(eyeX) != centreX || snapped(eyeZ) != centreZ ||
           registryVersion != version;
}

void LodLightMap::build(const LightSourceRegistry& registry, double eyeX,
                        double eyeZ, int periodX) {
    centreX = snapped(eyeX);
    centreZ = snapped(eyeZ);
    minX = centreX - Extent / 2.0;
    minZ = centreZ - Extent / 2.0;
    version = registry.version();
    built = true;
    included = 0;
    data.assign(size_t(Size) * Size * 4, 0.f);
    std::vector<float> strongest(size_t(Size) * Size, 0.f);
    registry.forEachNear(
        centreX, centreZ, Extent / 2.0 + LightEmission::NearRange, periodX,
        [&](LightSourceRegistry::Id, const LightSource& s) {
            const auto& e = s.emission;
            if (!e.emits() || e.isFar() || s.sealed()) return;
            const double lx =
                centreX + WorldWrap::deltaf(s.position[0], centreX, periodX);
            const double lz = s.position[2];
            const double tx = (lx - minX) / TilesPerTexel;
            const double tz = (lz - minZ) / TilesPerTexel;
            const int reach = int(std::ceil(e.radius / TilesPerTexel)) + 1;
            const int x0 = std::max(0, int(tx) - reach),
                      x1 = std::min(Size - 1, int(tx) + reach);
            const int z0 = std::max(0, int(tz) - reach),
                      z1 = std::min(Size - 1, int(tz) + reach);
            if (x0 > x1 || z0 > z1) return;
            ++included;
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const double cx = minX + (x + 0.5) * TilesPerTexel;
                    const double cz = minZ + (z + 0.5) * TilesPerTexel;
                    const float v =
                        e.attenuation(float(std::hypot(cx - lx, cz - lz)));
                    if (v <= 0) continue;
                    const size_t i = size_t(z) * Size + x;
                    for (int c = 0; c < 3; ++c)
                        data[i * 4 + c] += e.color[c] * v;
                    if (v > strongest[i]) {
                        strongest[i] = v;
                        data[i * 4 + 3] = float(s.position[1]);
                    }
                }
        });
}

std::array<float, 4> LodLightMap::at(double x, double z) const {
    if (!built) return {0, 0, 0, 0};
    const int tx = int(std::floor((x - minX) / TilesPerTexel));
    const int tz = int(std::floor((z - minZ) / TilesPerTexel));
    if (tx < 0 || tz < 0 || tx >= Size || tz >= Size) return {0, 0, 0, 0};
    const size_t i = (size_t(tz) * Size + tx) * 4;
    return {data[i], data[i + 1], data[i + 2], data[i + 3]};
}
