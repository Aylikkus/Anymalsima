#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "serialization/serializable_api.hpp"

// Heat or cold emitted by a definition: blocks today, items and entities
// later. The definition is data only; a placed or carried emitter becomes a
// ThermalSource (world/thermal_sources.hpp) with a world position.
//
// JSON (every field optional; a missing "thermal" object means no source):
//   "thermal": {
//     "enabled": true,          // false: not a thermal source
//     "delta_c": 20,            // signed °C added at the source centre
//     "radius": 6,              // effective range in metres
//     "falloff": 1.5            // exponent of (1 - d / radius)
//   }
//
// `delta_c` is a temperature offset, not an absolute temperature: the local
// query adds it to the ambient weather sample. Positive values warm, negative
// values cool. Sources add linearly where their ranges overlap. A solid in
// the path attenuates the contribution (see local_temperature.hpp); an open
// doorway does not.
struct ThermalEmission {
    // Longest supported range in metres. Local voxel sampling is intended for
    // campfires, heaters and small coolers; large fires use the dated regional
    // handover interface.
    static constexpr float MaxRadius = 512.f;
    static constexpr float MaxDeltaC = 500.f;

    bool enabled = true;
    float deltaC = 10.f;
    float radius = 6.f;
    float falloff = 1.5f;

    bool emits() const {
        return enabled && radius > 0 && std::isfinite(deltaC) && deltaC != 0;
    }

    // Signed °C offset at distance d (metres) before barriers.
    float offsetAt(float d) const {
        if (!emits() || d >= radius) return 0;
        const float t = 1.f - std::max(d, 0.f) / radius;
        return deltaC * std::pow(t, falloff);
    }

    // Throws std::invalid_argument naming the offending field.
    void validate() const {
        auto fail = [](const std::string& what) {
            throw std::invalid_argument("thermal." + what);
        };
        if (!std::isfinite(deltaC) || std::abs(deltaC) > MaxDeltaC)
            fail("delta_c must be finite and in [-500, 500]");
        if (!(radius > 0 && radius <= MaxRadius))
            fail("radius must be in (0, 512]");
        if (!(falloff > 0 && falloff <= 16)) fail("falloff must be in (0, 16]");
    }
};

inline void from_json(const Json& j, ThermalEmission& e) {
    if (!j.is_object())
        throw std::invalid_argument("thermal must be an object");
    if (j.contains("enabled")) j.at("enabled").get_to(e.enabled);
    if (j.contains("delta_c")) j.at("delta_c").get_to(e.deltaC);
    if (j.contains("radius")) j.at("radius").get_to(e.radius);
    if (j.contains("falloff")) j.at("falloff").get_to(e.falloff);
    e.validate();
}
