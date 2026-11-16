#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

#include "serialization/serializable_api.hpp"

// Light emitted by a definition: blocks today, items and entities later.
// The definition is data only; a placed or carried emitter becomes a
// LightSource (world/lighting/light_source.hpp) that adds a position and a
// world direction.
//
// JSON (every field optional; a missing "light" object means no emission):
//   "light": {
//     "enabled": true,               // false: not a light source
//     "radius": 15,                  // blocks, (0, MaxRadius]
//     "falloff": 1,                  // exponent of (1 - d / radius)
//     "intensity": 1,                // (0, 1], brightness at the source
//     "color": [1, 1, 1],            // linear RGB, each in [0, 1]
//     "cone": {                      // spotlight; omitted = omnidirectional
//       "direction": [0, -1, 0],     // local forward, normalized on load
//       "inner_angle": 20,           // degrees off axis at full intensity
//       "outer_angle": 35            // degrees off axis where it reaches 0
//     }
//   }
struct LightCone {
    std::array<float, 3> direction{0, -1, 0};
    float innerAngle = 20;  // degrees
    float outerAngle = 35;  // degrees
};

struct LightEmission {
    // Longest supported radius, sized for lighthouses seen across the sea.
    static constexpr float MaxRadius = 8192;
    // Light within this path distance (blocks) is flood-filled through the
    // voxels and respects walls; farther light is evaluated per pixel.
    static constexpr int NearRange = 15;

    bool enabled = true;
    float radius = NearRange;
    float falloff = 1;
    float intensity = 1;
    std::array<float, 3> color{1, 1, 1};
    std::optional<LightCone> cone;

    bool emits() const { return enabled && radius > 0 && intensity > 0; }
    // Reaches beyond the voxel near field, so it needs the per-pixel path,
    // the glare sprite and the world-wide far-light index.
    bool isFar() const { return emits() && radius > NearRange; }
    bool isWhite() const {
        return color[0] == color[1] && color[1] == color[2];
    }

    // Brightness at distance d (blocks) from the source, before the cone.
    float attenuation(float d) const {
        if (!emits() || d >= radius) return 0;
        const float t = 1.f - std::max(d, 0.f) / radius;
        return intensity * std::pow(t, falloff);
    }
    // Cone factor for a unit direction from the source toward the point,
    // given the source's world forward (unit). 1 without a cone.
    float coneFactor(const std::array<float, 3>& toPoint,
                     const std::array<float, 3>& forward) const {
        if (!cone) return 1;
        const float c = toPoint[0] * forward[0] + toPoint[1] * forward[1] +
                        toPoint[2] * forward[2];
        const float inner = std::cos(cone->innerAngle * kDegToRad);
        const float outer = std::cos(cone->outerAngle * kDegToRad);
        if (inner - outer <= 1e-6f) return c >= outer ? 1.f : 0.f;
        const float t = std::clamp((c - outer) / (inner - outer), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // Throws std::invalid_argument naming the offending field.
    void validate() const {
        auto fail = [](const std::string& what) {
            throw std::invalid_argument("light." + what);
        };
        if (!(radius > 0 && radius <= MaxRadius))
            fail("radius must be in (0, 8192]");
        if (!(falloff > 0 && falloff <= 16)) fail("falloff must be in (0, 16]");
        if (!(intensity > 0 && intensity <= 1))
            fail("intensity must be in (0, 1]");
        for (float c : color)
            if (!(c >= 0 && c <= 1)) fail("color components must be in [0, 1]");
        if (std::max({color[0], color[1], color[2]}) <= 0)
            fail("color must not be black");
        if (cone) {
            const auto& d = cone->direction;
            if (!(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-12f))
                fail("cone.direction must not be zero");
            if (!(cone->innerAngle >= 0 && cone->outerAngle > 0 &&
                  cone->innerAngle <= cone->outerAngle &&
                  cone->outerAngle <= 180))
                fail("cone angles need 0 <= inner_angle <= outer_angle <= 180");
        }
    }

   private:
    static constexpr float kDegToRad = 3.14159265358979f / 180.f;
};

inline void from_json(const Json& j, LightCone& c) {
    if (j.contains("direction")) j.at("direction").get_to(c.direction);
    if (j.contains("inner_angle")) j.at("inner_angle").get_to(c.innerAngle);
    if (j.contains("outer_angle")) j.at("outer_angle").get_to(c.outerAngle);
    const auto& d = c.direction;
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length > 1e-6f)
        for (auto& v : c.direction) v /= length;
}

inline void from_json(const Json& j, LightEmission& e) {
    if (!j.is_object()) throw std::invalid_argument("light must be an object");
    if (j.contains("enabled")) j.at("enabled").get_to(e.enabled);
    if (j.contains("radius")) j.at("radius").get_to(e.radius);
    if (j.contains("falloff")) j.at("falloff").get_to(e.falloff);
    if (j.contains("intensity")) j.at("intensity").get_to(e.intensity);
    if (j.contains("color")) j.at("color").get_to(e.color);
    if (j.contains("cone")) e.cone = j.at("cone").get<LightCone>();
    e.validate();
}
