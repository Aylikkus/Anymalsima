#pragma once

#include <array>
#include <map>
#include <string>

#include "structure_voxels.hpp"

namespace structure_voxels {

enum class OverrideMode : uint8_t { Removed, Patch, Replace };

// Coordinates are in the source's unwrapped tile frame. Patch uses zero to
// erase a generated tile. Replace describes the entire rebuilt or planted
// structure and can introduce a key absent from generation.
struct Override {
    OverrideMode mode = OverrideMode::Removed;
    uint64_t revision = 0;
    int anchorX = 0, anchorZ = 0;
    bool landmark = false;
    std::map<std::array<int, 3>, uint8_t> voxels;
    bool operator==(const Override&) const = default;
};

// Stable kind ID -> stable source key -> authoritative override.
using Overrides = std::map<std::string, std::map<uint64_t, Override>>;

void validateOverride(std::string_view kind, const Override& record);
std::vector<Kind> applyOverrides(const std::vector<Kind>& generated,
                                 const Overrides& overrides);

}  // namespace structure_voxels
