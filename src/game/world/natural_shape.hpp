#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "tile_shape.hpp"

// Deterministic surface profiles over a cube-only height field. A column's
// top is surfaceY + 1; a candidate high cell is cut into a one-metre ramp
// when exactly one cardinal neighbour has its top one lower and the opposite
// neighbour keeps the old top as a landing. Two adjacent low directions form
// an outer corner when both perpendicular approaches are straight ramps; a
// single low diagonal between two straight approaches forms an inner corner.
//
// The rule only reads heights and eligibility, never chunk load order. All
// coordinates are world tiles; the height callback owns X wrapping and Z
// clamping. Results are cached per queried cell.
namespace natural_shape {

class Field {
   public:
    using Height = std::function<int(int64_t, int64_t)>;     // surface_y
    using Eligible = std::function<bool(int64_t, int64_t)>;  // cuttable

    Field(Height height, Eligible eligible, bool edgeCompatibility = false,
          bool diagonalCorners = false)
        : height(std::move(height)),
          eligible(std::move(eligible)),
          edgeCompatibility(edgeCompatibility),
          diagonalCorners(diagonalCorners) {}

    TileShape at(int64_t x, int64_t z);

   private:
    enum class Kind : uint8_t { Cube, Straight, Outer, Inner, Diagonal };
    struct Proposal {
        Kind kind = Kind::Cube;
        TileShape shape{};
    };

    int top(int64_t x, int64_t z);
    int heightAt(int64_t x, int64_t z);
    bool canCut(int64_t x, int64_t z);
    Proposal raw(int64_t x, int64_t z);
    bool rawStraight(int64_t x, int64_t z, unsigned low, unsigned facing,
                     bool exactLanding = true);
    // Both proposals' tops match along the shared world-space edge.
    bool edgeMatches(const Proposal& proposal, int64_t x, int64_t z,
                     unsigned dir);
    bool acceptedStraight(int64_t x, int64_t z, unsigned low, unsigned facing);
    Proposal accepted(int64_t x, int64_t z);

    Height height;
    Eligible eligible;
    bool edgeCompatibility = false;
    bool diagonalCorners = false;
    std::unordered_map<uint64_t, int> heightCache;
    std::unordered_map<uint64_t, Proposal> rawCache;
    std::unordered_map<uint64_t, Proposal> acceptedCache;
    std::unordered_map<uint64_t, bool> eligibleCache;
};

}  // namespace natural_shape
