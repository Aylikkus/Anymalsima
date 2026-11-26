#pragma once

#include <string>

#include "serialization/serializable_api.hpp"
#include "thermal_emission.hpp"

// Definition of a movable thermal item. There is no inventory container yet,
// so the debug fixture carries one item at its current world position. The
// schema is deliberately the same as a block's thermal object, keeping units,
// ranges and the local-query handover one definition.
//
// JSON: { "name": "items.heater", "thermal": { "delta_c": 20, ... } }
struct ThermalItem {
    static constexpr const std::string TypeName = "items";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    ThermalEmission thermal;
};

inline void from_json(const Json& j, ThermalItem& item) {
    j.at("name").get_to(item.name);
    if (!j.contains("thermal"))
        throw std::runtime_error(item.name + ": missing thermal object");
    try {
        item.thermal = j.at("thermal").get<ThermalEmission>();
    } catch (const std::exception& e) {
        throw std::runtime_error(item.name + ": " + e.what());
    }
    if (!item.thermal.emits())
        throw std::runtime_error(item.name + ": thermal source emits nothing");
}
