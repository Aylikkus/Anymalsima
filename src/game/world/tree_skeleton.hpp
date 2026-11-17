#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "items/tree.hpp"

// Procedural tree skeletons (docs/handoff/trees.md, "Skeleton"). A skeleton
// is a pure function of the species, a seed, the headroom, the ground and
// the cuts made to the tree. Positions are fixed offsets from the anchor
// point and never move: age only decides which parts exist and how thick
// they are, so a growing tree only ever gains wood.

// Version of the tree generator (skeleton forms, rasterizer, placement).
// Saved per chunk with its wood (TreeStamp): growth is only reconciled into
// wood made by the same version, so a changed algorithm never grows
// new-shape wood next to old wood. Bump with any change to their output.
inline constexpr uint16_t TreeVersion = 1;

struct TreeVec {
    float x = 0, y = 0, z = 0;
    TreeVec operator+(TreeVec o) const { return {x + o.x, y + o.y, z + o.z}; }
    TreeVec operator-(TreeVec o) const { return {x - o.x, y - o.y, z - o.z}; }
    TreeVec operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(TreeVec o) const { return x * o.x + y * o.y + z * o.z; }
    TreeVec cross(TreeVec o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(dot(*this)); }
    TreeVec normalized() const {
        const float l = length();
        return l > 1e-6f ? *this * (1 / l) : TreeVec{0, 1, 0};
    }
    bool operator==(const TreeVec&) const = default;
};

inline TreeVec lerp(TreeVec a, TreeVec b, float t) { return a + (b - a) * t; }

// One capsule of wood. Ages are tree ages in game days.
struct TreeSegment {
    enum Flags : uint8_t {
        Pruned = 1,  // removed by a cut (kept so the cut wood can be found)
        Sprout = 2,  // regrowth from a cut
        CutEnd = 4,  // its end is a cut: the wood stops flat at it
    };
    TreeVec a, b;          // metres from the anchor point
    float r0 = 0, r1 = 0;  // full-grown radius at a and b
    // The growing tip passes a at birthA and b at birthB.
    float birthA = 0, birthB = 0;
    float matureAge = 0;  // full radius from this age on
    int32_t parent = -1;
    float attachT = 0;  // where on the parent it starts (1: continues it)
    uint8_t order = 0;  // 0: trunk (and giant limbs), 1+: branches
    uint8_t flags = 0;
    uint16_t cut = 0;  // 1-based index of the cut that pruned or grew it

    bool live() const { return !(flags & Pruned); }
};

// Where a clump of needles or leaves, or a palm frond, grows.
struct TreeFoliageSite {
    TreeVec position, direction;
    float size = 1;  // clump diameter, or frond length
    float birth = 0;
    float death = INFINITY;  // juvenile foliage is shed as the crown forms
    int32_t segment = -1;
    bool pruned = false;
};

// A cut through segment `segment` at fraction t, made when the tree was
// `age` days old. Indices refer to the skeleton with the earlier cuts
// applied (cuts never renumber segments; sprouts are appended).
struct TreeCut {
    int32_t segment = 0;
    float t = 0;
    float age = 0;
    bool operator==(const TreeCut&) const = default;
};

// Extra radius of the trunk near the ground (root flare and buttresses).
struct TreeFlare {
    float amount = 0;  // times the trunk radius at the ground
    float height = 1;  // metres over which it fades
    int lobes = 0;     // buttresses; 0: round
    float phase = 0;
};

struct TreeSkeletonInput {
    uint32_t seed = 0;
    // Free height above the anchor point (metres): ceiling or build limit.
    float headroom = TreeMaxHeight;
    // Ground height relative to the anchor at a horizontal offset (metres);
    // branches ending below it are dropped. Empty: no terrain pruning.
    std::function<float(float dx, float dz)> ground;
    std::vector<TreeCut> cuts;
    // Quarter turns around +Y applied to the finished tree, as the foliage
    // shader's yaw: x' = c x + s z, z' = -s x + c z. A quarter turn maps the
    // tile grid onto itself, so shared crown meshes drawn with that yaw
    // match the rotated wood exactly.
    int rotation = 0;
};

struct TreeSkeleton {
    std::vector<TreeSegment> segments;
    std::vector<TreeFoliageSite> foliage;
    TreeFlare flare;
    // Sampled for this tree (height after the headroom cap).
    float height = 0, trunkRadius = 0, crownRadius = 0;
    float maturity = 1, lifespan = 1, curve = 2;
    // Full-grown bounds of live wood and foliage, metres from the anchor.
    TreeVec boundsMin, boundsMax;
    bool dead = false;  // cut at the root of a species that does not resprout
    // What regrowth from a cut needs (applyTreeCuts).
    uint32_t seed = 0;
    float headroom = TreeMaxHeight;
    float clumpSize = 1;
    float windAzimuth = 0;

    // Size fraction of growth (0..1), 1 - (1 - x)^curve.
    float growth(float x) const {
        if (x <= 0) return 0;
        if (x >= 1) return 1;
        return 1 - std::pow(1 - x, curve);
    }
    // Inverse of growth: when a fraction of the size is reached.
    float growthTime(float fraction) const {
        if (fraction <= 0) return 0;
        if (fraction >= 1) return 1;
        return 1 - std::pow(1 - fraction, 1 / curve);
    }
    // How far along the segment the tip has grown (0..1) at `age`.
    float visible(const TreeSegment& s, float age) const {
        if (age < s.birthA) return -1;
        if (s.birthB <= s.birthA) return 1;
        return std::min(1.f, (age - s.birthA) / (s.birthB - s.birthA));
    }
    // Full-grown radius at fraction t of s, with the root flare at azimuth
    // `angle` (radians) for the trunk.
    float fullRadius(const TreeSegment& s, float t, float angle) const;
    // Radius at fraction t of s at `age`; 0 where it has not grown yet.
    float radius(const TreeSegment& s, float t, float age,
                 float angle = 0) const;
    // Age from which nothing grows any more.
    float matureAge() const;
};
using TreeSkeletonPtr = std::shared_ptr<const TreeSkeleton>;

TreeSkeletonPtr buildTreeSkeleton(const Tree& species,
                                  const TreeSkeletonInput& input);

// Applies cuts[first..] to a finished skeleton (pruning and sprouts); cut i
// is tagged i + 1. A tree cut while the game runs and the same tree rebuilt
// later from its saved cuts get identical skeletons, because
// buildTreeSkeleton applies input.cuts through this too (after rotation).
TreeSkeletonPtr applyTreeCuts(const TreeSkeleton& skeleton, const Tree& species,
                              const std::vector<TreeCut>& cuts,
                              size_t first = 0);

// A copy turned by `quarterTurns` (TreeSkeletonInput::rotation).
TreeSkeletonPtr rotateTreeSkeleton(const TreeSkeleton& skeleton,
                                   int quarterTurns);

// Skeletons shared by workers: building a massive tree takes milliseconds
// and every chunk it touches needs it. Keys must cover everything the
// skeleton depends on (tree identity and its cut revision).
class TreeSkeletonCache {
   public:
    explicit TreeSkeletonCache(size_t capacity = 256) : capacity(capacity) {}
    template <typename Build>
    TreeSkeletonPtr get(uint64_t key, Build&& build) {
        {
            std::lock_guard lock(mutex);
            if (const auto it = index.find(key); it != index.end()) {
                order.splice(order.begin(), order, it->second);
                return it->second->second;
            }
        }
        // Built outside the lock: two workers may build the same tree; the
        // result is identical, so the second insert is simply dropped.
        TreeSkeletonPtr built = build();
        std::lock_guard lock(mutex);
        if (const auto it = index.find(key); it != index.end())
            return it->second->second;
        order.emplace_front(key, built);
        index[key] = order.begin();
        while (order.size() > capacity) {
            index.erase(order.back().first);
            order.pop_back();
        }
        return built;
    }
    size_t size() const {
        std::lock_guard lock(mutex);
        return order.size();
    }

   private:
    using Entry = std::pair<uint64_t, TreeSkeletonPtr>;
    size_t capacity;
    mutable std::mutex mutex;
    std::list<Entry> order;
    std::unordered_map<uint64_t, std::list<Entry>::iterator> index;
};
