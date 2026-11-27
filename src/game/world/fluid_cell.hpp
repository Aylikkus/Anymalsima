#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "tile_piece.hpp"
#include "tile_shape.hpp"

// Water occupying a cell that also contains a partial solid. A cell profile
// describes the free volume below a horizontal head, the sealed ceiling of the
// lowest free run, and the open area on each cell face. It is derived from the
// same CellGeometry used by meshing, picking and collision, so liquid,
// rendering and barriers share one occupancy rule.
namespace fluid {

// A 1 m cell is sampled at this resolution to find a full-width horizontal
// solid (a sealed ceiling). Free volume between samples is interpolated.
inline constexpr int Buckets = 64;

struct Profile {
    bool uniform = true;   // empty cell or full cube: volume == height
    float ceiling = 1.f;   // lowest full-width solid above the bottom run
    float capacity = 1.f;  // free volume below the ceiling, m3 per 1 m2
    std::array<float, Buckets + 1> volume{};  // cumulative free volume
    // Lowest point of any opening on each Face (PosX..NegZ), NaN if closed.
    std::array<float, 6> openingLowest{};
    std::array<float, 6> openingArea{};  // fraction of the face (0..1)

    static Profile open();
    static Profile closed();
    static Profile make(const tile_geometry::CellGeometry& geometry);

    bool closed(Face face) const;
    bool hasOpening(Face face) const;
    // Free volume (m3) below a height in [0, 1] measured from the cell floor.
    float volumeAt(float level) const;
    // Height whose cumulative free volume is the requested amount.
    float levelFor(float volume) const;
    // Water units (0..255) for a local water level; 255 means the bottom run
    // is full to its ceiling.
    int unitsAt(float level) const;
    float levelForUnits(int units) const;
};

// The lowest height at which the two cells share open area. Both profiles
// describe their own cell; `face` is the side of A that touches B. Returns
// nullopt when the shared face is fully blocked.
std::optional<float> sharedOpening(const Profile& a, const Profile& b,
                                   Face face);

}  // namespace fluid
