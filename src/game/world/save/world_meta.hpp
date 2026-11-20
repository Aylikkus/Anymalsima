#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/geometry.hpp"
#include "serialization/serializable_api.hpp"

// Contents of a world's world.json: identity, timestamps, and the player's
// view. Unknown keys are preserved so newer saves round-trip through older
// builds.
struct WorldMeta : ISerializable {
    static constexpr int FormatVersion = 3;
    // Format 2 changed the vertical layout (1024-tall world); format 3 wraps
    // the world on X (periodic terrain noise, 512-region default width).
    // Older saves are not migrated.
    static constexpr int MinFormatVersion = 3;
    // Bump when chunk generation output changes. 6: ground cover on the
    // surface topsoil tile. 7: plants (FLOR). 8: trees (living wood).
    // 9: surface ceiling blended across region borders (no beach steps).
    // 10: prototype roots. 11: branching roots. 12: bark and inner wood.
    // 13: uphill root contact. 14: generated standing basins.
    static constexpr int HydrologyVersion = 14;
    static constexpr int RiverVersion = 15;
    static constexpr int NaturalRiverVersion = 16;
    static constexpr int BoundedRiverVersion = 17;
    static constexpr int RiverSedimentVersion = 18;
    static constexpr int LakeOutletVersion = 19;
    static constexpr int CoastalBasinVersion = 20;
    static constexpr int OceanShoreVersion = 21;
    static constexpr int GeneratorVersion = OceanShoreVersion;
    // Oldest generator version supported by terrain edit comparisons.
    // Later surface changes are selected by the saved generator version.
    static constexpr int SurfaceVersion = 9;

    struct CameraState {
        Vec3 position{0, 0, 0};
        float yaw = 0;
        float pitch = -1.5f;
        float distance = 10;
    };

    int formatVersion = FormatVersion;
    int generatorVersion = GeneratorVersion;
    uint64_t seed = 0;
    int width = 0;
    int height = 0;
    std::string createdAt;   // local time, ISO 8601 "YYYY-MM-DDTHH:MM:SS"
    std::string lastPlayed;  // same format
    Vec2u startRegion{0, 0};
    std::optional<CameraState> camera;  // absent until first save in play
    // Game time (WorldClock): days since the world began. Absent in older
    // saves (day 0).
    double worldDays = 0;
    Json extra = Json::object();

    Json serialize() const override;
    void deserialize(const Json& j) override;

    static std::string now();
};
