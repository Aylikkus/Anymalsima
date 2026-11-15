#pragma once
#include <array>
#include <cmath>

struct AABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct Frustum {
    // Each plane as (nx, ny, nz, d) where nx*x + ny*y + nz*z + d >= 0 is inside.
    std::array<std::array<float, 4>, 6> planes;

    // Build from a column-major combined view-projection matrix (bgfx convention).
    static Frustum fromViewProj(const float* vp) {
        Frustum f;
        // Left, Right, Bottom, Top, Near, Far
        // Gribb/Hartmann method — works directly on the VP matrix.
        auto& p = f.planes;
        p[0] = { vp[3]+vp[0], vp[7]+vp[4], vp[11]+vp[8],  vp[15]+vp[12] }; // left
        p[1] = { vp[3]-vp[0], vp[7]-vp[4], vp[11]-vp[8],  vp[15]-vp[12] }; // right
        p[2] = { vp[3]+vp[1], vp[7]+vp[5], vp[11]+vp[9],  vp[15]+vp[13] }; // bottom
        p[3] = { vp[3]-vp[1], vp[7]-vp[5], vp[11]-vp[9],  vp[15]-vp[13] }; // top
        p[4] = { vp[3]+vp[2], vp[7]+vp[6], vp[11]+vp[10], vp[15]+vp[14] }; // near
        p[5] = { vp[3]-vp[2], vp[7]-vp[6], vp[11]-vp[10], vp[15]-vp[14] }; // far

        // Normalize so the distance term is meaningful (optional but clean).
        for (auto& pl : p) {
            float len = std::sqrt(pl[0]*pl[0] + pl[1]*pl[1] + pl[2]*pl[2]);
            if (len > 0.f) { pl[0]/=len; pl[1]/=len; pl[2]/=len; pl[3]/=len; }
        }
        return f;
    }

    // Returns false if the AABB is fully outside any plane (cull it).
    bool intersects(const AABB& b) const {
        for (auto& pl : planes) {
            // Pick the positive vertex (furthest in the plane normal direction).
            float px = (pl[0] >= 0.f) ? b.maxX : b.minX;
            float py = (pl[1] >= 0.f) ? b.maxY : b.minY;
            float pz = (pl[2] >= 0.f) ? b.maxZ : b.minZ;
            if (pl[0]*px + pl[1]*py + pl[2]*pz + pl[3] < 0.f)
                return false; // fully outside this plane
        }
        return true;
    }
};