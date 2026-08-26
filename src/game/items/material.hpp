#pragma once


#include <optional>
#include <string>

#include "serialization/serializable_api.hpp"

struct Material {
    static constexpr const std::string TypeName = "materials";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    std::string texture_albedo;
    std::string texture_normal;
};

inline void from_json(const Json &j, Material &m) {
    j.at("name").get_to(m.name);
    j.at("texture_albedo").get_to(m.texture_albedo);
    j.at("texture_normal").get_to(m.texture_normal);
}