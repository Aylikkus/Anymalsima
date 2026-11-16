#include <cmath>
#include <random>

#include "test.hpp"
#include "world/light_shadow_math.hpp"
#include "world/lod_light_map.hpp"

TEST(shadow_face_mapping) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1, 1);
    for (int i = 0; i < 2000; ++i) {
        const double x = u(rng), y = u(rng), z = u(rng);
        if (std::abs(x) + std::abs(y) + std::abs(z) < 1e-3) continue;
        double nx, ny;
        const int f = LightShadowMath::face(x, y, z, nx, ny);
        CHECK(f >= 0 && f < 6);
        CHECK(std::abs(nx) <= 1 + 1e-9 && std::abs(ny) <= 1 + 1e-9);
        // The face view matrix maps v to view space whose projection is the
        // same ndc (90 degree frustum: ndc = view.xy / view.z).
        const auto m = LightShadowMath::viewMatrix(f);
        const double vx = x * m[0] + y * m[4] + z * m[8];
        const double vy = x * m[1] + y * m[5] + z * m[9];
        const double vz = x * m[2] + y * m[6] + z * m[10];
        CHECK(vz > 0);
        CHECK(std::abs(vx / vz - nx) < 1e-6);
        CHECK(std::abs(vy / vz - ny) < 1e-6);
    }
    double nx, ny;
    EQUAL(LightShadowMath::face(0, -5, 0.1, nx, ny), 3, "straight down");
    EQUAL(LightShadowMath::face(2, 1, 1, nx, ny), 0, "+x major");
    // Bases are right-handed in bx's left-handed look-at sense:
    // right = cross(up, forward).
    for (const auto& b : LightShadowMath::Faces) {
        const auto& u = b.up;
        const auto& n = b.forward;
        const float r[3] = {u[1] * n[2] - u[2] * n[1],
                            u[2] * n[0] - u[0] * n[2],
                            u[0] * n[1] - u[1] * n[0]};
        for (int a = 0; a < 3; ++a) EQUAL(b.right[a], r[a], "right axis");
    }
}

TEST(shadow_tile_layout) {
    int x, y;
    LightShadowMath::tileOrigin(0, 0, 512, x, y);
    EQUAL(x + y, 0, "first tile");
    LightShadowMath::tileOrigin(1, 3, 512, x, y);  // tile 9: column 1, row 1
    EQUAL(x, 512, "column");
    EQUAL(y, 512, "row");
}

TEST(shadow_reachable_faces) {
    LightEmission omni;
    EQUAL(int(LightShadowMath::reachableFaces(omni, {0, -1, 0})), 0x3f, "omni");
    LightEmission spot;
    spot.cone = LightCone{{0, -1, 0}, 10, 30};
    EQUAL(int(LightShadowMath::reachableFaces(spot, {0, -1, 0})), 1 << 3,
          "narrow cone down: -y face only");
    spot.cone->outerAngle = 60;
    EQUAL(int(LightShadowMath::reachableFaces(spot, {0, -1, 0})), 0x3b,
          "wide cone: every face but +y");
    spot.cone->outerAngle = 150;
    EQUAL(int(LightShadowMath::reachableFaces(spot, {0, -1, 0})), 0x3f,
          "very wide cone");
    CHECK(LightShadowMath::reaches(-1, -1, -1, 1, 1, 1, 0.5));
    CHECK(LightShadowMath::reaches(3, 4, -1, 10, 10, 1, 5));
    CHECK(!LightShadowMath::reaches(3, 4, -1, 10, 10, 1, 4.9));
}

TEST(lod_light_map_splat) {
    LightSourceRegistry registry;
    LightEmission torch;
    torch.radius = 12;
    torch.color = {1, 0.5f, 0};
    registry.setBlockLight({1000, 70, 1000}, 1, torch);
    LightEmission far;
    far.radius = 64;
    registry.setBlockLight({1200, 70, 1000}, 1, far);       // per pixel
    registry.setBlockLight({1100, 70, 1000}, 1, torch, 0);  // walled in
    LodLightMap map;
    CHECK(map.stale(1000, 1000, registry.version()));
    map.build(registry, 1000, 1000, 0);
    CHECK(map.valid());
    EQUAL(map.lightCount(), size_t(1), "only the open small light");
    CHECK(!map.stale(1010, 990, registry.version()));
    CHECK(map.stale(1600, 1000, registry.version()));  // recentred
    // Texel centres sit at origin + (i + 0.5) * 4; the torch is 2.12 tiles
    // from the centre of its texel.
    const double cx =
        map.originX() + (std::floor((1000.5 - map.originX()) / 4) + 0.5) * 4;
    const double cz =
        map.originZ() + (std::floor((1000.5 - map.originZ()) / 4) + 0.5) * 4;
    const float expected =
        torch.attenuation(float(std::hypot(cx - 1000.5, cz - 1000.5)));
    const auto texel = map.at(1000.5, 1000.5);
    CHECK(std::abs(texel[0] - expected) < 1e-4f);
    CHECK(std::abs(texel[1] - expected * 0.5f) < 1e-4f);
    EQUAL(texel[2], 0.f, "no blue");
    EQUAL(texel[3], 70.5f, "height of the light");
    EQUAL(map.at(1200.5, 1000.5)[0], 0.f, "far lights are not splatted");
    EQUAL(map.at(1100.5, 1000.5)[0], 0.f, "sealed lights are not splatted");
    EQUAL(map.at(1030.5, 1000.5)[0], 0.f, "beyond the radius");
    registry.removeBlockLight({1000, 70, 1000});
    CHECK(map.stale(1000, 1000, registry.version()));
}

TEST(lod_light_map_wraps) {
    // A light just across the X seam lands next to the camera.
    LightSourceRegistry registry;
    LightEmission torch;
    registry.setBlockLight({9998, 70, 500}, 1, torch);
    LodLightMap map;
    map.build(registry, 3, 500, 10000);
    CHECK(map.at(-1.5, 500.5)[0] > 0);
}

TEST(shadow_guard_band) {
    // The guarded projection maps ndc = 1 (the face border) about
    // GuardTexels inside the tile edge.
    const int size = 512;
    const double scale = LightShadowMath::guardScale(size);
    const double borderTexel = (0.5 + 0.5 / scale) * size;
    CHECK(std::abs(borderTexel - (size - LightShadowMath::GuardTexels)) < 0.05);
    const double fov = LightShadowMath::guardFov(size);
    CHECK(fov > 90 && fov < 91);
    CHECK(std::abs(std::tan(fov / 2 * 3.14159265358979 / 180) - scale) < 1e-9);
}
