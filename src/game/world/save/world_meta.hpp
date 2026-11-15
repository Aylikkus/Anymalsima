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
    static constexpr int FormatVersion = 2;
    // Format 2 changed the vertical layout (1024-tall world); older saves
    // are not migrated.
    static constexpr int MinFormatVersion = 2;
    // Bump when chunk generation output changes.
    static constexpr int GeneratorVersion = 2;

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
    Json extra = Json::object();

    Json serialize() const override;
    void deserialize(const Json& j) override;

    static std::string now();
};
