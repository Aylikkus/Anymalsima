#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "flora_json.hpp"
#include "soil.hpp"
#include "utils/region_enum_maps.hpp"

// A ground cover (grass, moss, lichen, ...) is not a block: it is an overlay
// painted on the faces of the solid soil tile it grows on (Tile::cover).
// See docs/handoff/flora.md.
struct GroundCoverFaces {
    bool top = true;
    bool sides = true;
    // Length of the fringe hanging down an exposed side face below the
    // tile's top edge; each tile picks a length in this range.
    ValueRange side_fringe_m{0.25f, 0.45f};
};

// Which soils the cover grows on. Empty names accept any soil; property
// ranges are tested against the topsoil properties.
struct GroundCoverSoils {
    std::vector<std::string> names;
    ValueRange fertility{0, 1};
    ValueRange water_retention{0, 1};
};

struct GroundCoverDensityNoise {
    int scale_m = 24;
    // Noise values at or below the threshold leave bare soil.
    float threshold = 0.35f;
    // Ramp steepness above the threshold: density reaches 1 at
    // threshold + (1 - threshold) / contrast.
    float contrast = 1.5f;
};

// Tint by local climate: humidity 0..1 and temperature across
// temperature_c (cold..warm), bilinear between the four corners.
struct GroundCoverTint {
    Color3 humid_warm{0.31f, 0.54f, 0.23f};
    Color3 humid_cold{0.24f, 0.42f, 0.27f};
    Color3 dry_warm{0.60f, 0.60f, 0.27f};
    Color3 dry_cold{0.48f, 0.50f, 0.32f};
    ValueRange temperature_c{0, 30};

    Color3 at(float humidity, float temperatureC) const {
        const float h = std::clamp(humidity, 0.f, 1.f);
        const float span = temperature_c.max - temperature_c.min;
        const float t =
            span > 0 ? std::clamp((temperatureC - temperature_c.min) / span,
                                  0.f, 1.f)
                     : 0.5f;
        const auto mix = [](const Color3& a, const Color3& b, float f) {
            return Color3{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f,
                          a.b + (b.b - a.b) * f};
        };
        return mix(mix(dry_cold, dry_warm, t), mix(humid_cold, humid_warm, t),
                   h);
    }
};

struct GroundCover {
    static constexpr const std::string TypeName = "ground_covers";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    // Material whose albedo alpha is the coverage mask and whose colour is
    // light detail multiplied by the tint.
    std::string overlay_material;
    GroundCoverFaces faces;
    GroundCoverSoils soils;
    std::vector<SoilPlacement> placement;
    // Empty: any vegetation except NONE (bare regions have no topsoil).
    std::vector<VegetationType> vegetation;
    GroundCoverDensityNoise density_noise;
    // Surface slope (metres per tile) at which density reaches zero.
    float max_slope = 0.8f;
    bool requires_sky = true;
    GroundCoverTint tint_gradient;

    bool acceptsSoil(const std::string& soilName,
                     const SoilProperties& properties) const {
        if (!soils.names.empty() &&
            std::find(soils.names.begin(), soils.names.end(), soilName) ==
                soils.names.end())
            return false;
        return soils.fertility.contains(properties.fertility) &&
               soils.water_retention.contains(properties.water_retention);
    }
    bool acceptsVegetation(VegetationType value) const {
        if (value == VegetationType::NONE) return false;
        return vegetation.empty() ||
               std::find(vegetation.begin(), vegetation.end(), value) !=
                   vegetation.end();
    }
};

inline void from_json(const Json& j, GroundCover& c) {
    using namespace flora_json;
    known(j, "ground cover",
          {"name", "overlay_material", "faces", "soils", "placement",
           "vegetation", "density_noise", "max_slope", "requires_sky",
           "tint_gradient"});
    c.name = name(j, "name");
    try {
        c.overlay_material = name(j, "overlay_material");
        if (j.contains("faces")) {
            const auto& f = j.at("faces");
            known(f, "faces", {"top", "sides", "side_fringe_m"});
            c.faces.top = boolean(f, "top", true);
            c.faces.sides = boolean(f, "sides", true);
            range(f, "side_fringe_m", c.faces.side_fringe_m, 0, 1);
        }
        if (!c.faces.top && !c.faces.sides)
            fail("faces", "a cover must show on the top or the sides");
        if (j.contains("soils")) {
            const auto& s = j.at("soils");
            known(s, "soils", {"names", "fertility", "water_retention"});
            c.soils.names = names(s, "names");
            range(s, "fertility", c.soils.fertility, 0, 1);
            range(s, "water_retention", c.soils.water_retention, 0, 1);
        }
        if (!j.contains("placement") || !j.at("placement").is_array() ||
            j.at("placement").empty())
            fail("placement", "expected at least one rule");
        j.at("placement").get_to(c.placement);
        soil_json::enums(j, "vegetation", RegionEnumMaps::vegetation(),
                         c.vegetation);
        if (j.contains("density_noise")) {
            const auto& d = j.at("density_noise");
            known(d, "density_noise", {"scale_m", "threshold", "contrast"});
            if (d.contains("scale_m")) {
                const auto& scale = d.at("scale_m");
                if (!scale.is_number_integer() || scale.get<int64_t>() < 1 ||
                    scale.get<int64_t>() > 4096)
                    fail("scale_m", "expected an integer in [1, 4096]");
                c.density_noise.scale_m = scale.get<int>();
            }
            if (d.contains("threshold")) {
                c.density_noise.threshold =
                    finite(d.at("threshold"), "threshold");
                if (c.density_noise.threshold < 0 ||
                    c.density_noise.threshold >= 1)
                    fail("threshold", "expected [0, 1)");
            }
            if (d.contains("contrast")) {
                c.density_noise.contrast = finite(d.at("contrast"), "contrast");
                if (c.density_noise.contrast <= 0)
                    fail("contrast", "expected a positive number");
            }
        }
        if (j.contains("max_slope")) {
            c.max_slope = finite(j.at("max_slope"), "max_slope");
            if (c.max_slope <= 0)
                fail("max_slope", "expected a positive number");
        }
        c.requires_sky = boolean(j, "requires_sky", true);
        if (j.contains("tint_gradient")) {
            const auto& t = j.at("tint_gradient");
            known(t, "tint_gradient",
                  {"humid_warm", "humid_cold", "dry_warm", "dry_cold",
                   "temperature_c"});
            auto& g = c.tint_gradient;
            for (const auto& [key, target] :
                 std::initializer_list<std::pair<const char*, Color3*>>{
                     {"humid_warm", &g.humid_warm},
                     {"humid_cold", &g.humid_cold},
                     {"dry_warm", &g.dry_warm},
                     {"dry_cold", &g.dry_cold}})
                if (t.contains(key)) *target = color(t.at(key), key);
            range(t, "temperature_c", g.temperature_c, -273.15f, 1000);
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(c.name + ": " + error.what());
    }
}
