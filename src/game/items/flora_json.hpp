#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "serialization/serializable_api.hpp"

// JSON helpers shared by ground covers, foliage materials and flora
// (docs/handoff/flora.md). Every numeric flora field accepts either a number
// or a [min, max] range; each plant instance samples the range from its seed.

// A closed range of floats. A plain number is stored as {n, n}.
struct ValueRange {
    float min = 0;
    float max = 0;

    constexpr ValueRange() = default;
    constexpr ValueRange(float value) : min(value), max(value) {}
    constexpr ValueRange(float lo, float hi) : min(lo), max(hi) {}

    // t in [0, 1] (for example SoilGenerator::unit of an instance seed).
    float sample(double t) const {
        return float(min + (double(max) - min) * std::clamp(t, 0.0, 1.0));
    }
    // Integer counts: every value in [min, max] is equally likely.
    int sampleInt(double t) const {
        const double lo = std::ceil(min), hi = std::floor(max);
        if (hi <= lo) return int(lo);
        return int(std::min(
            hi, lo + std::floor(std::clamp(t, 0.0, 1.0) * (hi - lo + 1))));
    }
    bool contains(float value) const { return value >= min && value <= max; }
    bool operator==(const ValueRange&) const = default;
};

// An sRGB colour as authored ("#rrggbb"), channels in [0, 1].
struct Color3 {
    float r = 1, g = 1, b = 1;
    bool operator==(const Color3&) const = default;
    // RGBA8 packed as bgfx expects for a normalized Uint8 x4 attribute
    // (little endian: r in the low byte).
    uint32_t packed(uint8_t alpha = 255) const {
        const auto channel = [](float v) {
            return uint32_t(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
        };
        return channel(r) | channel(g) << 8 | channel(b) << 16 |
               uint32_t(alpha) << 24;
    }
};

namespace flora_json {

[[noreturn]] inline void fail(std::string_view key, std::string_view what) {
    throw std::runtime_error(std::string(key) + ": " + std::string(what));
}

inline float finite(const Json& value, std::string_view key) {
    if (!value.is_number()) fail(key, "expected a number");
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > 1e9)
        fail(key, "number out of range");
    return float(number);
}

// Number or [min, max]; bounds [lo, hi] are inclusive.
inline ValueRange range(const Json& value, std::string_view key,
                        float lo = -1e9f, float hi = 1e9f) {
    ValueRange out;
    if (value.is_array()) {
        if (value.size() != 2) fail(key, "expected a number or [min, max]");
        out = {finite(value[0], key), finite(value[1], key)};
        if (out.min > out.max) fail(key, "reversed bounds");
    } else {
        out = ValueRange(finite(value, key));
    }
    if (out.min < lo || out.max > hi)
        fail(key, "expected values in [" + std::to_string(lo) + ", " +
                      std::to_string(hi) + "]");
    return out;
}

// Optional field: leaves out unchanged when key is absent.
inline void range(const Json& j, const char* key, ValueRange& out,
                  float lo = -1e9f, float hi = 1e9f) {
    if (j.contains(key)) out = range(j.at(key), key, lo, hi);
}

inline Color3 color(const Json& value, std::string_view key) {
    if (!value.is_string()) fail(key, "expected \"#rrggbb\"");
    const auto text = value.get<std::string>();
    if (text.size() != 7 || text[0] != '#') fail(key, "expected \"#rrggbb\"");
    uint32_t rgb = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = text[i];
        int digit = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
        if (digit < 0) fail(key, "expected \"#rrggbb\"");
        rgb = rgb << 4 | uint32_t(digit);
    }
    return {float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f,
            float(rgb & 0xFF) / 255.f};
}

inline std::string name(const Json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_string() ||
        j.at(key).get<std::string>().empty())
        fail(key, "expected a non-empty name");
    return j.at(key).get<std::string>();
}

inline bool boolean(const Json& j, const char* key, bool fallback) {
    if (!j.contains(key)) return fallback;
    if (!j.at(key).is_boolean()) fail(key, "expected true or false");
    return j.at(key).get<bool>();
}

inline std::vector<std::string> names(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key)) return out;
    const auto& value = j.at(key);
    if (!value.is_array()) fail(key, "expected an array of names");
    for (const auto& entry : value) {
        if (!entry.is_string() || entry.get<std::string>().empty())
            fail(key, "expected an array of names");
        out.push_back(entry.get<std::string>());
    }
    return out;
}

// Rejects keys outside the schema, so a typo fails loudly instead of being
// ignored.
inline void known(const Json& j, std::string_view what,
                  std::initializer_list<std::string_view> keys) {
    if (!j.is_object()) fail(what, "expected an object");
    for (const auto& [key, _] : j.items()) {
        bool found = false;
        for (const auto k : keys) found = found || k == key;
        if (!found) fail(what, "unknown field \"" + key + "\"");
    }
}

}  // namespace flora_json
