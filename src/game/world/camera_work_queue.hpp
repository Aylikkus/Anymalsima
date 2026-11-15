#pragma once

#include <algorithm>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "chunk.hpp"
#include "chunk_coord.hpp"
#include "core/geometry.hpp"

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

    static double distance(ChunkCoord coord, Vec3 focus) {
        auto axis = [](unsigned c, int size, float p) {
            const double lo = double(c) * size;
            const double d = std::max({lo - p, double(p) - (lo + size), 0.0});
            return d * d;
        };
        return axis(coord.x, Chunk::SIZE_X, focus.x) +
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
        struct Entry {
            ChunkCoord coord;
            uint64_t age;
            double distance;
        };
        std::vector<Entry> entries;
        entries.reserve(ages.size());
        for (const auto& [coord, age] : ages)
            entries.push_back(
                {coord, age, oldest ? 0 : distance(coord, focus)});
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

   private:
    std::unordered_map<ChunkCoord, uint64_t> ages;
    uint64_t sequence = 0;
};
