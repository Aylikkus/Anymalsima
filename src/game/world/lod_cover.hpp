#pragma once

#include <algorithm>
#include <set>
#include <vector>

#include "terrain_lod.hpp"

namespace TerrainLod {
// Chooses which cached patches to draw so that every in-view part of a root
// is covered exactly once, preferring the desired layout:
// - a desired leaf is drawn when available;
// - a split patch (ancestor of desired leaves) is replaced by its children
//   only once all in-view children can be covered, otherwise it stays;
// - while a coarser desired patch loads, its finer leftovers stay visible.
// `inView(p)` matches TerrainLod::select visibility, `drawable(p)` says a
// patch has a GPU mesh, and `below(p)` says some strict descendant does
// (bounding the search to cached subtrees).
template <typename InView, typename Drawable, typename Below>
class Cover {
   public:
    Cover(const std::set<Patch>& split, InView inView, Drawable drawable,
          Below below)
        : split(split), inView(inView), drawable(drawable), below(below) {}

    bool coverable(const Patch& p) const {
        if (drawable(p)) return true;
        if (p.spacing == 1 || !below(p)) return false;
        for (const auto& child : children(p))
            if (inView(child) && !coverable(child)) return false;
        return true;
    }
    bool coveredByDescendants(const Patch& p) const {
        if (p.spacing == 1) return false;
        const auto parts = children(p);
        return std::all_of(parts.begin(), parts.end(), [&](const auto& child) {
            return !inView(child) || coverable(child);
        });
    }
    void select(const Patch& p, std::vector<Patch>& out) const {
        if (!inView(p)) return;
        const bool splitHere = split.contains(p);
        if (drawable(p) && !splitHere) {
            out.push_back(p);
            return;
        }
        if (p.spacing > 1 && (splitHere || below(p))) {
            if (coveredByDescendants(p)) {
                for (const auto& child : children(p)) select(child, out);
                return;
            }
        }
        if (drawable(p)) out.push_back(p);
    }

   private:
    const std::set<Patch>& split;
    InView inView;
    Drawable drawable;
    Below below;
};
}  // namespace TerrainLod
