#pragma once

#include <optional>

#include "core/geometry.hpp"

class ChunkManager;

// Upright, kinematic debug walker. Coordinates refer to the center of its feet.
class GroundedProbe {
   public:
    static constexpr float Radius = .25f;
    static constexpr float Height = 1.7f;
    static constexpr float EyeHeight = 1.55f;

    static std::optional<GroundedProbe> spawn(ChunkManager& chunks, Vec3 from);
    void update(ChunkManager& chunks, Vec2 velocity, float dt);
    Vec3 feet() const { return {float(x), y, float(z)}; }
    bool grounded() const { return onGround; }

   private:
    // Keep substeps precise in the largest worlds and on either side of X=0.
    double x = 0, z = 0;
    float y = 0, verticalSpeed = 0;
    bool onGround = true;

    bool move(ChunkManager& chunks, double nextX, double nextZ);
};
