#include "natural_shape.hpp"

#include <array>

namespace natural_shape {
namespace {
constexpr std::array<int, 4> dx{0, 1, 0, -1};
constexpr std::array<int, 4> dz{1, 0, -1, 0};
constexpr unsigned opposite(unsigned dir) { return (dir + 2) & 3; }
uint64_t key(int64_t x, int64_t z) { return (uint64_t(x) << 32) ^ uint32_t(z); }
}  // namespace

bool Field::canCut(int64_t x, int64_t z) {
    const auto found = eligibleCache.find(key(x, z));
    if (found != eligibleCache.end()) return found->second;
    const bool value = eligible(x, z);
    eligibleCache.emplace(key(x, z), value);
    return value;
}

bool Field::rawStraight(int64_t x, int64_t z, unsigned low, unsigned facing) {
    if (opposite(facing) != low || !canCut(x, z)) return false;
    const int heightHere = top(x, z);
    if (top(x + dx[low], z + dz[low]) != heightHere - 1) return false;
    if (top(x + dx[facing], z + dz[facing]) != heightHere) return false;
    for (unsigned dir = 0; dir < 4; ++dir) {
        if (dir == low || dir == facing) continue;
        // A second low side would make this a junction; a lower side would
        // leave the ramp floating over open ground.
        if (top(x + dx[dir], z + dz[dir]) < heightHere) return false;
    }
    return true;
}

Field::Proposal Field::raw(int64_t x, int64_t z) {
    const auto cached = rawCache.find(key(x, z));
    if (cached != rawCache.end()) return cached->second;

    Proposal result;
    if (canCut(x, z)) {
        const int heightHere = top(x, z);
        std::array<bool, 4> lows{};
        int lowCount = 0;
        unsigned first = 0, second = 0;
        for (unsigned dir = 0; dir < 4; ++dir) {
            lows[dir] = top(x + dx[dir], z + dz[dir]) == heightHere - 1;
            if (!lows[dir]) continue;
            if (lowCount == 0)
                first = dir;
            else if (lowCount == 1)
                second = dir;
            ++lowCount;
        }
        if (lowCount == 1) {
            const unsigned low = first;
            const unsigned facing = opposite(low);
            if (top(x + dx[facing], z + dz[facing]) == heightHere &&
                rawStraight(x, z, low, facing))
                result = {Kind::Straight,
                          TileShape::make(TileShapeProfile::Ramp,
                                          TileFacing(facing))};
        } else if (lowCount == 2 && opposite(first) != second) {
            // Outer corner: the two perpendicular approaches rise across the
            // remaining high edges.
            for (unsigned turn = 0; turn < 4 && result.kind == Kind::Cube;
                 ++turn) {
                const unsigned lowX = (3 + turn) & 3;
                const unsigned lowZ = (2 + turn) & 3;
                if (!((first == lowX && second == lowZ) ||
                      (first == lowZ && second == lowX)))
                    continue;
                const unsigned east = (1 + turn) & 3;
                const unsigned north = (0 + turn) & 3;
                if (top(x + dx[east], z + dz[east]) != heightHere ||
                    top(x + dx[north], z + dz[north]) != heightHere)
                    continue;
                if (!rawStraight(x + dx[east], z + dz[east], lowZ,
                                 (0 + turn) & 3) ||
                    !rawStraight(x + dx[north], z + dz[north], lowX,
                                 (1 + turn) & 3))
                    continue;
                result = {Kind::Outer,
                          TileShape::make(TileShapeProfile::OuterCorner,
                                          TileFacing(turn))};
            }
        } else if (lowCount == 0) {
            // Inner corner: a single low diagonal between two perpendicular
            // straight approaches; the opposite edges stay level.
            for (unsigned turn = 0; turn < 4 && result.kind == Kind::Cube;
                 ++turn) {
                const unsigned west = (3 + turn) & 3;
                const unsigned south = (2 + turn) & 3;
                const unsigned east = (1 + turn) & 3;
                const unsigned north = (0 + turn) & 3;
                bool level = true;
                for (unsigned dir : {west, south, east, north})
                    level &= top(x + dx[dir], z + dz[dir]) == heightHere;
                if (!level) continue;
                if (top(x + dx[west] + dx[south], z + dz[west] + dz[south]) !=
                    heightHere - 1)
                    continue;
                if (!rawStraight(x + dx[west], z + dz[west], south,
                                 (0 + turn) & 3) ||
                    !rawStraight(x + dx[south], z + dz[south], west,
                                 (1 + turn) & 3))
                    continue;
                result = {Kind::Inner,
                          TileShape::make(TileShapeProfile::InnerCorner,
                                          TileFacing(turn))};
            }
        }
    }
    rawCache.emplace(key(x, z), result);
    return result;
}

bool Field::acceptedStraight(int64_t x, int64_t z, unsigned low,
                             unsigned facing) {
    const auto proposal = raw(x, z);
    if (proposal.kind != Kind::Straight ||
        unsigned(proposal.shape.facing()) != facing || opposite(facing) != low)
        return false;
    // A landing is only usable while it remains an uncut cube. This is the
    // candidate/landing check that keeps a second cut from removing the
    // first ramp's shared top edge.
    if (raw(x + dx[low], z + dz[low]).kind != Kind::Cube) return false;
    if (raw(x + dx[facing], z + dz[facing]).kind != Kind::Cube) return false;
    return true;
}

Field::Proposal Field::accepted(int64_t x, int64_t z) {
    const auto cached = acceptedCache.find(key(x, z));
    if (cached != acceptedCache.end()) return cached->second;
    auto result = raw(x, z);
    if (result.kind == Kind::Straight) {
        const unsigned facing = unsigned(result.shape.facing());
        const unsigned low = opposite(facing);
        if (raw(x + dx[low], z + dz[low]).kind != Kind::Cube ||
            raw(x + dx[facing], z + dz[facing]).kind != Kind::Cube)
            result = {};
    } else if (result.kind == Kind::Outer) {
        const unsigned turn = unsigned(result.shape.facing());
        const unsigned lowX = (3 + turn) & 3;
        const unsigned lowZ = (2 + turn) & 3;
        const unsigned east = (1 + turn) & 3;
        const unsigned north = (0 + turn) & 3;
        if (raw(x + dx[lowX], z + dz[lowX]).kind != Kind::Cube ||
            raw(x + dx[lowZ], z + dz[lowZ]).kind != Kind::Cube ||
            !acceptedStraight(x + dx[east], z + dz[east], lowZ, turn) ||
            !acceptedStraight(x + dx[north], z + dz[north], lowX,
                              (1 + turn) & 3))
            result = {};
    } else if (result.kind == Kind::Inner) {
        const unsigned turn = unsigned(result.shape.facing());
        const unsigned west = (3 + turn) & 3;
        const unsigned south = (2 + turn) & 3;
        const unsigned east = (1 + turn) & 3;
        const unsigned north = (0 + turn) & 3;
        if (raw(x + dx[east], z + dz[east]).kind != Kind::Cube ||
            raw(x + dx[north], z + dz[north]).kind != Kind::Cube ||
            raw(x + dx[west] + dx[south], z + dz[west] + dz[south]).kind !=
                Kind::Cube ||
            !acceptedStraight(x + dx[west], z + dz[west], south, turn) ||
            !acceptedStraight(x + dx[south], z + dz[south], west,
                              (1 + turn) & 3))
            result = {};
    }
    acceptedCache.emplace(key(x, z), result);
    return result;
}

TileShape Field::at(int64_t x, int64_t z) { return accepted(x, z).shape; }

}  // namespace natural_shape
