#include "tree_rasterizer.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace {
// Squared slack added to a radius: a part of radius 0.5 then reaches 0.81
// from its axis, so a centred one-block trunk stays one block wide (its
// neighbours are 1 away) while a leaning one keeps face-connected tiles row
// to row instead of a diagonal stair; for thick parts it is negligible.
constexpr float RadiusSlack2 = 0.4f;
constexpr float MaxReachPad = 0.71f;
// Extra reach of a cut's removed wood (see markTreeWood).
constexpr float CutSampleSlack(float spacing) { return spacing * 0.5f + 0.05f; }

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

// A flat cut: no tile whose centre lies past `point` along `normal` (cuts
// sit on a tile face, so the row above the face goes with the cut).
struct CutPlane {
    TreeVec point, normal;
};

// Cut planes by segment. A cut end clips its own segment and the segments
// of the same axis below it whose rounded ends reach past the cut (on a
// thick trunk the sample sphere at a joint bulges up to its radius beyond
// it, which would leave a floating dome above a horizontal cut).
std::vector<std::vector<CutPlane>> cutPlanes(const TreeSkeleton& k) {
    std::vector<std::vector<CutPlane>> planes;
    for (size_t i = 0; i < k.segments.size(); ++i) {
        const auto& c = k.segments[i];
        if (!c.live() || !(c.flags & TreeSegment::CutEnd)) continue;
        // The axis of the cut: its own, or (cut right at its start) the
        // axis of the segment it continues.
        TreeVec axis = c.b - c.a;
        if (axis.dot(axis) < 1e-6f && c.parent >= 0) {
            const auto& p = k.segments[size_t(c.parent)];
            axis = p.b - p.a;
        }
        if (axis.dot(axis) < 1e-6f) continue;
        // A grid cut keeps the face the player sawed.
        const bool grid = c.cutNormal.dot(c.cutNormal) > 0.5f;
        const CutPlane plane{c.b, grid ? c.cutNormal : axis.normalized()};
        if (planes.empty()) planes.resize(k.segments.size());
        planes[i].push_back(plane);
        for (size_t s = i; k.segments[s].attachT >= 1;) {
            const int32_t parent = k.segments[s].parent;
            if (parent < 0) break;
            const auto& p = k.segments[size_t(parent)];
            if (!p.live() || p.order != c.order) break;
            if ((c.b - p.b).length() > maxRadius(k, p) + 1) break;
            planes[size_t(parent)].push_back(plane);
            s = size_t(parent);
        }
    }
    return planes;
}
TreeTileBox woodBounds(const TreeSkeleton& skeleton, TreeAnchor anchor,
                       bool includePruned, uint16_t cut) {
    if (!skeleton.formsWood()) return {};
    TreeVec lo{INFINITY, INFINITY, INFINITY},
        hi{-INFINITY, -INFINITY, -INFINITY};
    for (const auto& s : skeleton.segments) {
        if (cut && (s.live() || s.cut != cut)) continue;
        if ((!s.live() && !includePruned) ||
            maxRadius(skeleton, s) < TreeVoxelRadius)
            continue;
        float pad = maxRadius(skeleton, s) + MaxReachPad;
        if (cut)
            pad +=
                CutSampleSlack(std::max(0.5f, maxRadius(skeleton, s) * 0.25f));
        // A cut's wood includes the stump's beyond its (possibly oblique)
        // cut face, up to a stump radius back from the cut point.
        if (cut && s.parent >= 0 && s.attachT >= 1) {
            const auto& stump = skeleton.segments[size_t(s.parent)];
            if (stump.live() && (stump.flags & TreeSegment::CutEnd))
                pad += maxRadius(skeleton, stump) * 1.5f;
        }
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
}  // namespace

TreeTileBox treeWoodBounds(const TreeSkeleton& skeleton, TreeAnchor anchor,
                           bool includePruned) {
    return woodBounds(skeleton, anchor, includePruned, 0);
}

TreeTileBox treeCutWoodBounds(const TreeSkeleton& skeleton, TreeAnchor anchor,
                              uint16_t cut) {
    return cut ? woodBounds(skeleton, anchor, true, cut) : TreeTileBox{};
}

void markTreeWood(const TreeSkeleton& skeleton, TreeAnchor anchor,
                  const TreeTileBox& box, float age, std::vector<uint8_t>& mask,
                  TreeWoodSelection selection) {
    mask.assign(box.volume(), 0);
    if (box.empty() || age <= 0 || !skeleton.formsWood()) return;
    const auto planes = selection.cut == 0
                            ? cutPlanes(skeleton)
                            : std::vector<std::vector<CutPlane>>{};
    static const std::vector<CutPlane> noPlanes;
    // beyond: only tiles past one of `clip` (the stump's wood a cut took),
    // else tiles before all of them.
    const auto raster = [&](const TreeSegment& s,
                            const std::vector<CutPlane>& clip, bool beyond) {
        // Pruned pieces are rasterized whole (as they were when cut).
        const float visible = selection.cut ? 1 : skeleton.visible(s, age);
        if (visible < 0) return;
        const float rMax = maxRadius(skeleton, s);
        if (rMax < TreeVoxelRadius) return;
        const TreeVec axis = s.b - s.a;
        const float length2 = axis.dot(axis);
        const float length = std::sqrt(length2);
        // Fixed sample points along the segment; a quarter of the radius
        // apart, so the union of their spheres is smooth.
        const float spacing = std::max(0.5f, rMax * 0.25f);
        const int samples = std::max(1, int(std::ceil(length / spacing)));
        const float dt = 1.f / float(samples);
        // A cut's wood was rasterized on the uncut segment's samples; the
        // pieces sample anew from the cut point, up to half a spacing away
        // from the old ones. Reach that far, or rim tiles stay behind as
        // stray blocks (live wood is kept by the caller's `now` test).
        const float extra = selection.cut ? CutSampleSlack(spacing) : 0.f;
        const float reach = rMax + MaxReachPad + extra;
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
        if (local.empty()) return;
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
                    // A cut is flat: nothing past its plane.
                    if (std::any_of(
                            clip.begin(), clip.end(), [&](const CutPlane& p) {
                                return (q - p.point).dot(p.normal) > 0.f;
                            }) != beyond)
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
                        const float limit =
                            extra > 0 ? std::sqrt(r * r + RadiusSlack2) + extra
                                      : 0.f;
                        if (extra > 0 ? d.dot(d) <= limit * limit
                                      : d.dot(d) <= r * r + RadiusSlack2) {
                            mask[index] = 1;
                            break;
                        }
                    }
                }
    };
    for (size_t si = 0; si < skeleton.segments.size(); ++si) {
        const auto& s = skeleton.segments[si];
        if (!selected(s, selection)) continue;
        raster(s, planes.empty() ? noPlanes : planes[si], false);
        // The piece a cut took starts on its stump: the stump's wood past
        // the cut face went with it (on a sloping limb sawn along a grid
        // row, a wedge the piece's own capsule does not reach).
        if (selection.cut && s.parent >= 0 && s.attachT >= 1) {
            const auto& stump = skeleton.segments[size_t(s.parent)];
            if (!stump.live() || !(stump.flags & TreeSegment::CutEnd) ||
                !(stump.b == s.a))
                continue;
            const TreeVec axis = stump.b - stump.a;
            const bool grid = stump.cutNormal.dot(stump.cutNormal) > 0.5f;
            if (!grid && axis.dot(axis) < 1e-6f) continue;
            const std::vector<CutPlane> face{
                {stump.b, grid ? stump.cutNormal : axis.normalized()}};
            raster(stump, face, true);
            // And the rounded ends of the same axis below that the face
            // clips (as cutPlanes does for the live wood).
            for (const TreeSegment* c = &stump; c->attachT >= 1;) {
                if (c->parent < 0) break;
                const auto& p = skeleton.segments[size_t(c->parent)];
                if (!p.live() || p.order != stump.order) break;
                if ((stump.b - p.b).length() > maxRadius(skeleton, p) + 1)
                    break;
                raster(p, face, true);
                c = &p;
            }
        }
    }
}

void treeWoodTops(const TreeSkeleton& skeleton, TreeAnchor anchor,
                  const TreeTileBox& box, float age, std::vector<int>& tops) {
    const int width = box.x1 - box.x0, depth = box.z1 - box.z0;
    tops.assign(box.empty() ? 0 : size_t(width) * size_t(depth), INT_MIN);
    if (box.empty() || age <= 0 || !skeleton.formsWood()) return;
    const auto planes = cutPlanes(skeleton);
    static const std::vector<CutPlane> noPlanes;
    for (size_t si = 0; si < skeleton.segments.size(); ++si) {
        const auto& s = skeleton.segments[si];
        if (!s.live()) continue;
        const auto& clip = planes.empty() ? noPlanes : planes[si];
        const float visible = skeleton.visible(s, age);
        if (visible < 0) continue;
        const float rMax = maxRadius(skeleton, s);
        if (rMax < TreeVoxelRadius) continue;
        // The sampling of markTreeWood. Its window around a tile's
        // projection (rMax + 1 along the axis) and its reach test never
        // exclude a sample whose sphere holds the tile centre (a sphere
        // reaches at most sqrt(r^2 + RadiusSlack2) < rMax + 0.71), so every
        // sample up to `visible` can be tested alone.
        const TreeVec axis = s.b - s.a;
        const float length = std::sqrt(axis.dot(axis));
        const float spacing = std::max(0.5f, rMax * 0.25f);
        const int samples = std::max(1, int(std::ceil(length / spacing)));
        const float dt = 1.f / float(samples);
        const bool lobed = s.order == 0 && skeleton.flare.lobes > 0 &&
                           !(s.flags & TreeSegment::Sprout);
        // markTreeWood's per-tile test against one sample.
        const auto inside = [&](const TreeVec& p, float r, int x, int y,
                                int z) {
            const TreeVec q = centre(anchor, x, y, z);
            const TreeVec d = q - p;
            if (d.dot(d) > r * r + RadiusSlack2) return false;
            return std::none_of(clip.begin(), clip.end(),
                                [&](const CutPlane& c) {
                                    return (q - c.point).dot(c.normal) > 0.f;
                                });
        };
        for (int k = 0; float(k) * dt <= visible; ++k) {
            const float t = float(k) * dt;
            const TreeVec p = lerp(s.a, s.b, t);
            // Columns the sphere may reach.
            const float reach = rMax + MaxReachPad;
            const int x0 =
                std::max(box.x0, anchor.x + int(std::floor(p.x - reach)));
            const int x1 =
                std::min(box.x1 - 1, anchor.x + int(std::ceil(p.x + reach)));
            const int z0 =
                std::max(box.z0, anchor.z + int(std::floor(p.z - reach)));
            const int z1 =
                std::min(box.z1 - 1, anchor.z + int(std::ceil(p.z + reach)));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const TreeVec q0 = centre(anchor, x, box.y0, z);
                    const float dx = q0.x - p.x, dz = q0.z - p.z;
                    const float angle = lobed ? std::atan2(dz, dx) : 0;
                    const float r = skeleton.radius(s, t, age, angle);
                    if (r < TreeVoxelRadius) continue;
                    const float h2 = r * r + RadiusSlack2 - dx * dx - dz * dz;
                    // A little negative slack for float rounding: the exact
                    // test below decides.
                    if (h2 < -0.01f) continue;
                    const float h = std::sqrt(std::max(0.f, h2));
                    // Tile y has its centre at y - anchor.y - 0.5.
                    int hi = anchor.y + int(std::floor(p.y + h + 0.5f)) + 1;
                    int lo = anchor.y + int(std::ceil(p.y - h + 0.5f)) - 1;
                    hi = std::min(hi, box.y1 - 1);
                    lo = std::max(lo, box.y0);
                    int& top = tops[size_t(z - box.z0) * size_t(width) +
                                    size_t(x - box.x0)];
                    for (int y = hi; y >= lo && y > top; --y)
                        if (inside(p, r, x, y, z)) {
                            top = y;
                            break;
                        }
                }
        }
    }
}
