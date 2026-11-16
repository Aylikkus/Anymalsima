#pragma once

#include <cmath>
#include <cstdint>

#include "consts.hpp"

// The world wraps on X: tile x and x + tiles are the same column. Stored
// coordinates (chunks, regions, edits, saves) are canonical, in [0, period).
// Rendering and distances use the copy nearest to a reference point (the
// eye), so the seam at x = 0 / x = tiles is invisible. Z does not wrap.
// tiles <= 0 disables wrapping (every helper returns its input).
struct WorldWrap {
    static constexpr int ChunkSize = 16;
    static constexpr int RegionSize = CHUNKS_PER_REGION * ChunkSize;

    int tiles = 0;

    static WorldWrap fromRegions(int regions) {
        return {regions > 0 ? regions * RegionSize : 0};
    }
    bool enabled() const { return tiles > 0; }
    int chunks() const { return tiles / ChunkSize; }
    int regions() const { return tiles / RegionSize; }

    // Floor modulo into [0, period).
    static int64_t mod(int64_t value, int64_t period) {
        if (period <= 0) return value;
        const int64_t r = value % period;
        return r < 0 ? r + period : r;
    }
    // Signed difference value - ref of the nearest copy, in [-period/2,
    // period/2).
    static int64_t delta(int64_t value, int64_t ref, int64_t period) {
        if (period <= 0) return value - ref;
        return mod(value - ref + period / 2, period) - period / 2;
    }
    static double modf(double value, double period) {
        if (period <= 0) return value;
        const double r = std::fmod(value, period);
        return r < 0 ? r + period : r;
    }
    static double deltaf(double value, double ref, double period) {
        if (period <= 0) return value - ref;
        return modf(value - ref + period / 2, period) - period / 2;
    }

    int64_t wrapTile(int64_t x) const { return mod(x, tiles); }
    int64_t wrapChunk(int64_t x) const { return mod(x, chunks()); }
    int64_t wrapRegion(int64_t x) const { return mod(x, regions()); }
    double wrapX(double x) const { return modf(x, tiles); }

    int64_t deltaTile(int64_t x, int64_t ref) const {
        return delta(x, ref, tiles);
    }
    int64_t deltaChunk(int64_t x, int64_t ref) const {
        return delta(x, ref, chunks());
    }
    int64_t deltaRegion(int64_t x, int64_t ref) const {
        return delta(x, ref, regions());
    }
    double deltaX(double x, double ref) const { return deltaf(x, ref, tiles); }

    // The copy of tile x nearest to ref (may lie outside [0, tiles)).
    double imageX(double x, double ref) const { return ref + deltaX(x, ref); }
};
