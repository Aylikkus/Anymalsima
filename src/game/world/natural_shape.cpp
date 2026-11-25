#include "natural_shape.hpp"

#include <array>
#include <cmath>

namespace natural_shape {
namespace {
constexpr std::array<int, 4> dx{0, 1, 0, -1};
constexpr std::array<int, 4> dz{1, 0, -1, 0};
constexpr unsigned opposite(unsigned dir) { return (dir + 2) & 3; }
uint64_t key(int64_t x, int64_t z) { return (uint64_t(x) << 32) ^ uint32_t(z); }
constexpr size_t MaxCachedInputs = 1 << 16;

// Top height at the two endpoints of one cell boundary, in local units and
// in ascending world X (for +Z/-Z) or Z (for +X/-X) order. A cube reports a
// level top; a ramp or junction reports the actual linear edge.
struct EdgeTops {
    bool valid = false;
    float first = 0, second = 0;
};

EdgeTops edgeTops(TileShape shape, unsigned dir) {
    float x0 = 0, z0 = 0, x1 = 1, z1 = 1;
    switch (dir) {
        case 0:  // +Z
            z0 = z1 = 1;
            break;
        case 1:  // +X
            x0 = x1 = 1;
            z0 = 0, z1 = 1;
            break;
        case 2:  // -Z
            z0 = z1 = 0;
            break;
        default:  // -X
            x0 = x1 = 0;
            z0 = 0, z1 = 1;
            break;
    }
    const auto a = tile_geometry::supportHeight(shape, x0, z0);
    const auto b = tile_geometry::supportHeight(shape, x1, z1);
    if (!a || !b) return {};
    return {true, *a, *b};
}

bool sameWorldEdge(const EdgeTops& a, float abase, const EdgeTops& b,
                   float bbase) {
    if (!a.valid || !b.valid) return false;
    constexpr float epsilon = 1e-4f;
    return std::abs((abase + a.first) - (bbase + b.first)) <= epsilon &&
           std::abs((abase + a.second) - (bbase + b.second)) <= epsilon;
}
}  // namespace

int Field::heightAt(int64_t x, int64_t z) {
    const uint64_t k = key(x, z);
    if (const auto found = heightCache.find(k); found != heightCache.end())
        return found->second;
    if (heightCache.size() >= MaxCachedInputs) heightCache.clear();
    const int value = height(x, z);
    heightCache.emplace(k, value);
    return value;
}

int Field::top(int64_t x, int64_t z) { return heightAt(x, z) + 1; }

bool Field::canCut(int64_t x, int64_t z) {
    const auto found = eligibleCache.find(key(x, z));
    if (found != eligibleCache.end()) return found->second;
    const bool value = eligible(x, z);
    eligibleCache.emplace(key(x, z), value);
    return value;
}

bool Field::rawStraight(int64_t x, int64_t z, unsigned low, unsigned facing,
                        bool exactLanding) {
    if (opposite(facing) != low || !canCut(x, z)) return false;
    const int heightHere = top(x, z);
    if (top(x + dx[low], z + dz[low]) != heightHere - 1) return false;
    // A landing at the same top keeps walking level; a higher cell is a
    // wall that starts exactly at the ramp's high edge.
    if (exactLanding ? top(x + dx[facing], z + dz[facing]) != heightHere
                     : top(x + dx[facing], z + dz[facing]) < heightHere)
        return false;
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
            if (rawStraight(x, z, low, facing, !edgeCompatibility))
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
                const int highEast = top(x + dx[east], z + dz[east]);
                const int highNorth = top(x + dx[north], z + dz[north]);
                if (highEast < heightHere || highNorth < heightHere) continue;
                if (highEast == heightHere && highNorth == heightHere &&
                    rawStraight(x + dx[east], z + dz[east], lowZ,
                                (0 + turn) & 3) &&
                    rawStraight(x + dx[north], z + dz[north], lowX,
                                (1 + turn) & 3)) {
                    result = {Kind::Outer,
                              TileShape::make(TileShapeProfile::OuterCorner,
                                              TileFacing(turn))};
                } else if (diagonalCorners) {
                    // The surface steps down in both horizontal axes, so no
                    // level ramp or junction can meet the taller hillside.
                    // A single-plane diagonal ramp is the closest profile.
                    result = {Kind::Diagonal,
                              TileShape::make(TileShapeProfile::DiagonalRamp,
                                              TileFacing(turn))};
                }
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

bool Field::edgeMatches(const Proposal& proposal, int64_t x, int64_t z,
                        unsigned dir) {
    const auto mine = edgeTops(proposal.shape, dir);
    if (!mine.valid) return false;
    const int64_t nx = x + dx[dir], nz = z + dz[dir];
    const auto other = raw(nx, nz);
    const auto theirs = edgeTops(other.shape, opposite(dir));
    return sameWorldEdge(mine, float(heightAt(x, z)), theirs,
                         float(heightAt(nx, nz)));
}

bool Field::acceptedStraight(int64_t x, int64_t z, unsigned low,
                             unsigned facing) {
    const auto proposal = raw(x, z);
    if (proposal.kind != Kind::Straight ||
        unsigned(proposal.shape.facing()) != facing || opposite(facing) != low)
        return false;
    if (!edgeCompatibility) {
        // Generator 25: both neighbours of a ramp had to stay whole cubes,
        // which kept a second cut from removing the shared top edge but left
        // many terrace corners and short ridges as full blocks.
        return raw(x + dx[low], z + dz[low]).kind == Kind::Cube &&
               raw(x + dx[facing], z + dz[facing]).kind == Kind::Cube;
    }
    // The ramp must meet the terrain on both sloping ends in world space.
    // A higher neighbour is a wall that starts at the ramp's high edge; a
    // level neighbour must present the same edge so a ridge or landing is
    // continuous.
    if (!edgeMatches(proposal, x, z, low)) return false;
    const int high = top(x + dx[facing], z + dz[facing]);
    if (high == top(x, z)) return edgeMatches(proposal, x, z, facing);
    return high > top(x, z);
}

Field::Proposal Field::accepted(int64_t x, int64_t z) {
    const auto cached = acceptedCache.find(key(x, z));
    if (cached != acceptedCache.end()) return cached->second;
    auto result = raw(x, z);
    const unsigned turn = unsigned(result.shape.facing());
    const unsigned facing = turn;
    const unsigned low = opposite(facing);
    const unsigned lowX = (3 + turn) & 3;
    const unsigned lowZ = (2 + turn) & 3;
    const unsigned west = (3 + turn) & 3;
    const unsigned south = (2 + turn) & 3;
    const unsigned east = (1 + turn) & 3;
    const unsigned north = (0 + turn) & 3;
    if (result.kind == Kind::Straight) {
        if (!acceptedStraight(x, z, low, facing)) result = {};
    } else if (!edgeCompatibility) {
        if (result.kind == Kind::Outer) {
            if (raw(x + dx[lowX], z + dz[lowX]).kind != Kind::Cube ||
                raw(x + dx[lowZ], z + dz[lowZ]).kind != Kind::Cube ||
                !acceptedStraight(x + dx[east], z + dz[east], lowZ, turn) ||
                !acceptedStraight(x + dx[north], z + dz[north], lowX,
                                  (1 + turn) & 3))
                result = {};
        } else if (result.kind == Kind::Inner) {
            if (raw(x + dx[east], z + dz[east]).kind != Kind::Cube ||
                raw(x + dx[north], z + dz[north]).kind != Kind::Cube ||
                raw(x + dx[west] + dx[south], z + dz[west] + dz[south]).kind !=
                    Kind::Cube ||
                !acceptedStraight(x + dx[west], z + dz[west], south, turn) ||
                !acceptedStraight(x + dx[south], z + dz[south], west,
                                  (1 + turn) & 3))
                result = {};
        }
    } else if (result.kind == Kind::Outer) {
        bool ok = acceptedStraight(x + dx[east], z + dz[east], lowZ, turn) &&
                  acceptedStraight(x + dx[north], z + dz[north], lowX,
                                   (1 + turn) & 3);
        for (unsigned dir : {lowX, lowZ, east, north})
            ok = ok && edgeMatches(result, x, z, dir);
        // A junction that cannot meet its approaches falls back to the
        // single-plane corner ramp instead of a whole block.
        if (!ok)
            result =
                diagonalCorners
                    ? Proposal{Kind::Diagonal,
                               TileShape::make(TileShapeProfile::DiagonalRamp,
                                               TileFacing(turn))}
                    : Proposal{};
    } else if (result.kind == Kind::Inner) {
        bool ok =
            acceptedStraight(x + dx[west], z + dz[west], south, turn) &&
            acceptedStraight(x + dx[south], z + dz[south], west,
                             (1 + turn) & 3) &&
            raw(x + dx[west] + dx[south], z + dz[west] + dz[south]).kind ==
                Kind::Cube;
        for (unsigned dir : {west, south, east, north})
            ok = ok && edgeMatches(result, x, z, dir);
        if (!ok) result = {};
    }
    acceptedCache.emplace(key(x, z), result);
    return result;
}

TileShape Field::at(int64_t x, int64_t z) { return accepted(x, z).shape; }

}  // namespace natural_shape
