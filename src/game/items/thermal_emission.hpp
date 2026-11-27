#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
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
//     "falloff": 1.5,           // exponent of (1 - d / radius)
//     "mode": "fixed",          // "fixed" (default) or "target"
//     "target_c": 21,           // target mode: requested room air °C
//     "deadband_c": .5,         // target mode: hysteresis half-width
//     "gain": 3,                // target mode: command °C per °C error
//     "min_delta_c": -20,       // target mode: command clamp
//     "max_delta_c": 40
//   }
//
// `delta_c` is a temperature offset, not an absolute temperature: the local
// query adds it to the ambient weather sample. Positive values warm, negative
// values cool. Sources add linearly where their ranges overlap. A solid in
// the path attenuates the contribution (see local_temperature.hpp); an open
// doorway does not.
//
// Fixed mode keeps the step 15 open-loop behaviour. Target mode is a
// thermostat: the room simulation measures the connected room air temperature
// and replaces `delta_c` with a command clamped to [min_delta_c, max_delta_c].
// The command is held while the error stays inside the deadband, giving
// hysteresis, and the clamp is the anti-windup limit. An open door simply
// raises the leakage, so the controller saturates without accumulating an
// unbounded command.
enum class ThermalMode : uint8_t { Fixed = 0, Target = 1 };

// Per-source controller state. It is runtime-only; the configured mode and
// target come from the definition, so a reload cannot silently drop them.
struct ThermalControllerState {
    bool initialized = false;
    bool saturated = false;
    float commandC = 0;
};

struct ThermalEmission {
    // Longest supported range in metres. Local voxel sampling is intended for
    // campfires, heaters and small coolers; large fires use the dated regional
    // handover interface.
    static constexpr float MaxRadius = 512.f;
    static constexpr float MaxDeltaC = 500.f;
    static constexpr float MaxTargetC = 200.f;
    static constexpr float MaxGain = 100.f;
    static constexpr float MaxDeadbandC = 20.f;

    bool enabled = true;
    float deltaC = 10.f;
    float radius = 6.f;
    float falloff = 1.5f;

    ThermalMode mode = ThermalMode::Fixed;
    float targetC = 21.f;
    float deadbandC = .5f;
    float gain = 3.f;
    float minDeltaC = -20.f;
    float maxDeltaC = 40.f;

    bool thermostat() const { return mode == ThermalMode::Target; }

    bool emits() const {
        return enabled && radius > 0 &&
               (thermostat() || (std::isfinite(deltaC) && deltaC != 0));
    }

    // Signed °C offset at distance d (metres) before barriers.
    float offsetAt(float d) const { return offsetAt(d, deltaC); }

    // Signed °C offset for an explicit command (a fixed offset or the
    // thermostat's current output).
    float offsetAt(float d, float command) const {
        if (!enabled || radius <= 0 || !std::isfinite(d) ||
            !std::isfinite(command) || command == 0 || d >= radius)
            return 0;
        const float t = 1.f - std::max(d, 0.f) / radius;
        return command * std::pow(t, falloff);
    }

    // Open-loop command for the given measured room temperature. Fixed mode
    // returns its configured offset unchanged.
    float commandedDelta(float roomC) const {
        if (!thermostat()) return deltaC;
        return std::clamp(gain * (targetC - roomC), minDeltaC, maxDeltaC);
    }

    // Advances a controller one simulation step. A room inside the deadband
    // receives no command, giving on/off hysteresis around the setpoint;
    // outside it the proportional command is clamped (anti-windup). The state
    // records whether the command sits at either clamp for the panel.
    float updateCommand(float roomC, ThermalControllerState& state) const {
        if (!thermostat()) {
            state.commandC = deltaC;
            state.saturated = false;
            state.initialized = true;
            return state.commandC;
        }
        if (std::abs(roomC - targetC) <= deadbandC) {
            state.commandC = 0;
        } else {
            state.commandC = commandedDelta(roomC);
        }
        const float epsilon = std::max(1e-4f, 1e-3f * std::abs(maxDeltaC));
        state.saturated = state.commandC >= maxDeltaC - epsilon ||
                          state.commandC <= minDeltaC + epsilon;
        state.initialized = true;
        return state.commandC;
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
        if (thermostat()) {
            if (!std::isfinite(targetC) || std::abs(targetC) > MaxTargetC)
                fail("target_c must be finite and in [-200, 200]");
            if (!std::isfinite(deadbandC) || deadbandC < 0 ||
                deadbandC > MaxDeadbandC)
                fail("deadband_c must be in [0, 20]");
            if (!std::isfinite(gain) || !(gain > 0) || gain > MaxGain)
                fail("gain must be in (0, 100]");
            if (!std::isfinite(minDeltaC) || !std::isfinite(maxDeltaC) ||
                std::abs(minDeltaC) > MaxDeltaC ||
                std::abs(maxDeltaC) > MaxDeltaC || minDeltaC > maxDeltaC)
                fail(
                    "min_delta_c and max_delta_c must be finite, ordered and "
                    "in [-500, 500]");
        }
    }
};

inline void from_json(const Json& j, ThermalEmission& e) {
    if (!j.is_object())
        throw std::invalid_argument("thermal must be an object");
    if (j.contains("enabled")) j.at("enabled").get_to(e.enabled);
    if (j.contains("delta_c")) j.at("delta_c").get_to(e.deltaC);
    if (j.contains("radius")) j.at("radius").get_to(e.radius);
    if (j.contains("falloff")) j.at("falloff").get_to(e.falloff);
    if (j.contains("mode")) {
        const auto mode = j.at("mode").get<std::string>();
        if (mode == "fixed")
            e.mode = ThermalMode::Fixed;
        else if (mode == "target")
            e.mode = ThermalMode::Target;
        else
            throw std::invalid_argument(
                "thermal.mode must be \"fixed\" or \"target\"");
    }
    if (j.contains("target_c")) j.at("target_c").get_to(e.targetC);
    if (j.contains("deadband_c")) j.at("deadband_c").get_to(e.deadbandC);
    if (j.contains("gain")) j.at("gain").get_to(e.gain);
    if (j.contains("min_delta_c")) j.at("min_delta_c").get_to(e.minDeltaC);
    if (j.contains("max_delta_c")) j.at("max_delta_c").get_to(e.maxDeltaC);
    e.validate();
}
