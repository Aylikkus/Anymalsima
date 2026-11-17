#include "tree_rasterizer.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Squared slack added to a radius: a part of radius 0.5 then reaches 0.81
// from its axis, so a centred one-block trunk stays one block wide (its
// neighbours are 1 away) while a leaning one keeps face-connected tiles row
// to row instead of a diagonal stair; for thick parts it is negligible.
constexpr float RadiusSlack2 = 0.4f;
constexpr float MaxReachPad = 0.71f;

// Tile centre relative to the anchor point (top centre of the anchor tile).
TreeVec centre(TreeAnchor anchor, int x, int y, int z) {
    return {float(x - anchor.x), float(y - anchor.y) - 0.5f,
            float(z - anchor.z)};
}

bool selected(const TreeSegment& s, TreeWoodSelection selection) {
    if (selection.cut == 0) return s.live();
    return !s.live() && s.cut == selection.cut;
}

float maxRadius(const TreeSkeleton& k, const TreeSegment& s) {
    const float flare = s.order == 0 ? 1 + k.flare.amount : 1;
    return std::max(s.r0, s.r1) * flare;
}
}  // namespace

TreeTileBox treeWoodBounds(const TreeSkeleton& skeleton, TreeAnchor anchor,
                           bool includePruned) {
    TreeVec lo{INFINITY, INFINITY, INFINITY},
        hi{-INFINITY, -INFINITY, -INFINITY};
    for (const auto& s : skeleton.segments) {
        if ((!s.live() && !includePruned) ||
            maxRadius(skeleton, s) < TreeVoxelRadius)
            continue;
        const float pad = maxRadius(skeleton, s) + MaxReachPad;
        for (const auto& v : {s.a, s.b}) {
            lo = {std::min(lo.x, v.x - pad), std::min(lo.y, v.y - pad),
                  std::min(lo.z, v.z - pad)};
            hi = {std::max(hi.x, v.x + pad), std::max(hi.y, v.y + pad),
                  std::max(hi.z, v.z + pad)};
        }
    }
    if (lo.x > hi.x) return {};
    // Tile x has its centre at x - anchor.x (y: y - anchor.y - 0.5).
    return {anchor.x + int(std::floor(lo.x)),
            anchor.y + int(std::floor(lo.y + 0.5f)),
            anchor.z + int(std::floor(lo.z)),
            anchor.x + int(std::ceil(hi.x)) + 1,
            anchor.y + int(std::ceil(hi.y + 0.5f)) + 1,
            anchor.z + int(std::ceil(hi.z)) + 1};
}

void markTreeWood(const TreeSkeleton& skeleton, TreeAnchor anchor,
                  const TreeTileBox& box, float age, std::vector<uint8_t>& mask,
                  TreeWoodSelection selection) {
    mask.assign(box.volume(), 0);
    if (box.empty() || age <= 0) return;
    for (const auto& s : skeleton.segments) {
        if (!selected(s, selection)) continue;
        // Pruned pieces are rasterized whole (as they were when cut).
        const float visible = selection.cut ? 1 : skeleton.visible(s, age);
        if (visible < 0) continue;
        const float rMax = maxRadius(skeleton, s);
        if (rMax < TreeVoxelRadius) continue;
        const TreeVec axis = s.b - s.a;
        const float length2 = axis.dot(axis);
        const float length = std::sqrt(length2);
        // Fixed sample points along the segment; a quarter of the radius
        // apart, so the union of their spheres is smooth.
        const float spacing = std::max(0.5f, rMax * 0.25f);
        const int samples = std::max(1, int(std::ceil(length / spacing)));
        const float dt = 1.f / float(samples);
        const float reach = rMax + MaxReachPad;
        const TreeVec lo{std::min(s.a.x, s.b.x) - reach,
                         std::min(s.a.y, s.b.y) - reach,
                         std::min(s.a.z, s.b.z) - reach};
        const TreeVec hi{std::max(s.a.x, s.b.x) + reach,
                         std::max(s.a.y, s.b.y) + reach,
                         std::max(s.a.z, s.b.z) + reach};
        const TreeTileBox local =
            TreeTileBox{anchor.x + int(std::floor(lo.x)),
                        anchor.y + int(std::floor(lo.y + 0.5f)),
                        anchor.z + int(std::floor(lo.z)),
                        anchor.x + int(std::ceil(hi.x)) + 1,
                        anchor.y + int(std::ceil(hi.y + 0.5f)) + 1,
                        anchor.z + int(std::ceil(hi.z)) + 1}
                .intersect(box);
        if (local.empty()) continue;
        const bool lobed = s.order == 0 && skeleton.flare.lobes > 0 &&
                           !(s.flags & TreeSegment::Sprout);
        const float window = length > 1e-4f ? (rMax + 1) / length : 1;
        for (int y = local.y0; y < local.y1; ++y)
            for (int z = local.z0; z < local.z1; ++z)
                for (int x = local.x0; x < local.x1; ++x) {
                    const size_t index = box.index(x, y, z);
                    if (mask[index]) continue;
                    const TreeVec q = centre(anchor, x, y, z);
                    // Unclamped projection: independent of age, so the set
                    // of samples tested only grows as the tip extends.
                    const float tu =
                        length2 > 1e-8f ? (q - s.a).dot(axis) / length2 : 0;
                    // A cut end is flat: nothing past the cut plane.
                    if ((s.flags & TreeSegment::CutEnd) &&
                        selection.cut == 0 &&
                        (tu - 1) * length > 0.5f)
                        continue;
                    const float t0 = std::max(0.f, tu - window);
                    const float t1 = std::min(visible, tu + window);
                    if (t1 < t0) continue;
                    // Quick reject by distance to the axis line.
                    const TreeVec nearest =
                        lerp(s.a, s.b, std::clamp(tu, 0.f, 1.f));
                    if ((q - nearest).dot(q - nearest) > reach * reach)
                        continue;
                    for (int k = int(std::ceil(t0 / dt)); k * dt <= t1; ++k) {
                        const float t = float(k) * dt;
                        const TreeVec p = lerp(s.a, s.b, t);
                        const TreeVec d = q - p;
                        const float angle = lobed ? std::atan2(d.z, d.x) : 0;
                        const float r = selection.cut
                                            ? skeleton.fullRadius(s, t, angle)
                                            : skeleton.radius(s, t, age, angle);
                        if (r < TreeVoxelRadius) continue;
                        if (d.dot(d) <= r * r + RadiusSlack2) {
                            mask[index] = 1;
                            break;
                        }
                    }
                }
    }
}
