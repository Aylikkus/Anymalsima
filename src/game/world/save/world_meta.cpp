#include "world_meta.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace {
const char* const KnownKeys[] = {
    "formatVersion", "generatorVersion", "seed",        "width",  "height",
    "createdAt",     "lastPlayed",       "startRegion", "camera", "worldDays"};
}

Json WorldMeta::serialize() const {
    Json j = extra.is_object() ? extra : Json::object();
    j["formatVersion"] = formatVersion;
    j["generatorVersion"] = generatorVersion;
    // Stored as a string: JSON tools commonly lose 64-bit integer precision.
    j["seed"] = std::to_string(seed);
    j["width"] = width;
    j["height"] = height;
    j["createdAt"] = createdAt;
    j["lastPlayed"] = lastPlayed;
    j["startRegion"] = {startRegion.x, startRegion.y};
    j["worldDays"] = worldDays;
    if (camera)
        j["camera"] = {
            {"position",
             {camera->position.x, camera->position.y, camera->position.z}},
            {"yaw", camera->yaw},
            {"pitch", camera->pitch},
            {"distance", camera->distance},
        };
    return j;
}

void WorldMeta::deserialize(const Json& j) {
    if (!j.is_object()) throw std::runtime_error("world.json is not an object");
    formatVersion = j.value("formatVersion", 0);
    if (formatVersion < MinFormatVersion || formatVersion > FormatVersion)
        throw std::runtime_error("unsupported world format version " +
                                 std::to_string(formatVersion));
    generatorVersion = j.value("generatorVersion", 1);
    seed = std::stoull(j.at("seed").get<std::string>());
    width = j.at("width").get<int>();
    height = j.at("height").get<int>();
    createdAt = j.value("createdAt", std::string{});
    lastPlayed = j.value("lastPlayed", createdAt);
    if (auto it = j.find("startRegion"); it != j.end() && it->size() == 2)
        startRegion = {(*it)[0].get<uint32_t>(), (*it)[1].get<uint32_t>()};
    worldDays = 0;
    if (auto it = j.find("worldDays"); it != j.end() && it->is_number()) {
        worldDays = it->get<double>();
        if (!std::isfinite(worldDays) || worldDays < 0) worldDays = 0;
    }
    camera.reset();
    if (auto it = j.find("camera"); it != j.end() && it->is_object()) {
        CameraState c;
        const auto& p = it->at("position");
        c.position = {p.at(0).get<float>(), p.at(1).get<float>(),
                      p.at(2).get<float>()};
        c.yaw = it->value("yaw", c.yaw);
        c.pitch = it->value("pitch", c.pitch);
        c.distance = it->value("distance", c.distance);
        camera = c;
    }
    extra = j;
    for (const char* key : KnownKeys) extra.erase(key);
}

std::string WorldMeta::now() {
    const std::time_t t =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}
