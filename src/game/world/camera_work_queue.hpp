#pragma once

#include <algorithm>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "core/geometry.hpp"
#include "world_wrap.hpp"

// Pending work stays outside the worker pool so a new focus changes its order
// immediately. Sequence numbers survive duplicate requests and
// reprioritization.
class CameraWorkQueue {
   public:
    void insert(ChunkCoord coord) {
        if (!ages.contains(coord)) ages.emplace(coord, ++sequence);
    }
    void erase(ChunkCoord coord) { ages.erase(coord); }
    bool contains(ChunkCoord coord) const { return ages.contains(coord); }
    bool empty() const { return ages.empty(); }
    size_t size() const { return ages.size(); }
    template <typename F>
    void forEach(F&& fn) const {
        for (const auto& [coord, age] : ages) fn(coord);
    }

    // periodX: world width in tiles when X wraps (0: no wrapping). The X
    // distance is measured to the chunk copy nearest to the focus.
    void setPeriodX(int value) { periodX = value; }
    double distance(ChunkCoord coord, Vec3 focus) const {
        return distance(coord, focus, periodX);
    }
    static double distance(ChunkCoord coord, Vec3 focus, int periodX) {
        auto axis = [](unsigned c, int size, double p) {
            const double lo = double(c) * size;
            const double d = std::max({lo - p, p - (lo + size), 0.0});
            return d * d;
        };
        const double lo = double(coord.x) * Chunk::SIZE_X;
        const double fx =
            periodX > 0 ? lo + Chunk::SIZE_X / 2.0 +
                              WorldWrap::deltaf(
                                  focus.x, lo + Chunk::SIZE_X / 2.0, periodX)
                        : focus.x;
        return axis(coord.x, Chunk::SIZE_X, fx) +
               axis(coord.y, Chunk::SIZE_Y, focus.y) +
               axis(coord.z, Chunk::SIZE_Z, focus.z);
    }

    template <typename Eligible, typename Anchor>
    std::optional<ChunkCoord> best(Vec3 focus, bool oldest, Eligible eligible,
                                   Anchor anchor) const {
        using Rank = std::tuple<double, uint64_t, unsigned, unsigned, unsigned>;
        std::optional<ChunkCoord> result;
        Rank bestRank{};
        for (const auto& [coord, age] : ages) {
            if (!eligible(coord)) continue;
            const Rank rank{oldest ? 0.0 : distance(anchor(coord), focus), age,
                            coord.x, coord.y, coord.z};
            if (!result || rank < bestRank) {
                result = coord;
                bestRank = rank;
            }
        }
        return result;
    }

    std::vector<ChunkCoord> ordered(Vec3 focus, bool oldest) const {
        return ordered(focus, oldest, [](ChunkCoord coord) { return coord; });
    }
    // Same order as repeated best() calls with the same anchor, computed once.
    template <typename Anchor>
    std::vector<ChunkCoord> ordered(Vec3 focus, bool oldest,
                                    Anchor anchor) const {
        struct Entry {
            ChunkCoord coord;
            uint64_t age;
            double distance;
        };
        std::vector<Entry> entries;
        entries.reserve(ages.size());
        for (const auto& [coord, age] : ages)
            entries.push_back(
                {coord, age, oldest ? 0 : distance(anchor(coord), focus)});
        std::sort(
            entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return std::tie(a.distance, a.age, a.coord.x, a.coord.y,
                                a.coord.z) < std::tie(b.distance, b.age,
                                                      b.coord.x, b.coord.y,
                                                      b.coord.z);
            });
        std::vector<ChunkCoord> result;
        result.reserve(entries.size());
        for (const auto& entry : entries) result.push_back(entry.coord);
        return result;
    }

    // The `limit` first entries of ordered(focus, false, anchor), without
    // sorting the rest: a queue of tens of thousands stays cheap.
    template <typename Anchor>
    std::vector<ChunkCoord> nearest(Vec3 focus, Anchor anchor,
                                    size_t limit) const {
        struct Entry {
            double distance;
            uint64_t age;
            ChunkCoord coord;
        };
        std::vector<Entry> entries;
        entries.reserve(ages.size());
        for (const auto& [coord, age] : ages)
            entries.push_back({distance(anchor(coord), focus), age, coord});
        const auto less = [](const Entry& a, const Entry& b) {
            return std::tie(a.distance, a.age, a.coord.x, a.coord.y,
                            a.coord.z) <
                   std::tie(b.distance, b.age, b.coord.x, b.coord.y, b.coord.z);
        };
        if (entries.size() > limit) {
            std::nth_element(entries.begin(), entries.begin() + limit,
                             entries.end(), less);
            entries.resize(limit);
        }
        std::sort(entries.begin(), entries.end(), less);
        std::vector<ChunkCoord> result;
        result.reserve(entries.size());
        for (const auto& e : entries) result.push_back(e.coord);
        return result;
    }

   private:
    std::unordered_map<ChunkCoord, uint64_t> ages;
    uint64_t sequence = 0;
    int periodX = 0;
};
