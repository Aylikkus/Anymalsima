#include "tree_foliage_builder.hpp"

#include <algorithm>
#include <cmath>

namespace tree_foliage {
namespace {

constexpr float Pi = 3.14159265358979f;
constexpr size_t MaxVertices = 65535;

struct Rng {
    uint64_t state;
    float next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return float((z ^ (z >> 31)) >> 40) / float(1 << 24);
    }
    float signedUnit() { return next() * 2 - 1; }
};

TreeVec perpendicular(TreeVec d) {
    const TreeVec ref =
        std::abs(d.y) < 0.9f ? TreeVec{0, 1, 0} : TreeVec{1, 0, 0};
    return d.cross(ref).normalized();
}

Color3 mix(const Color3& a, const Color3& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t};
}
Color3 scaled(const Color3& c, float k) { return {c.r * k, c.g * k, c.b * k}; }

// Palette colour at gradient position t (0 shade, 0.5 base, 1 tip); the
// tip stays subtle so clumps read mostly as base.
Color3 gradientColor(const FoliagePalette& p, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t < 0.5f ? mix(p.shade, p.base, t * 2)
                    : mix(p.base, p.tip, (t - 0.5f) * 2 * 0.6f);
}

class Builder {
   public:
    Builder(const TreeSkeleton& k, const Tree& species,
            const DataStorage<FoliageMaterial>& materials, float age,
            Detail detail, bool bare)
        : k(k),
          species(species),
          age(age),
          detail(detail),
          bare(bare),
          leaf(find(materials, species.foliage.material)),
          bark(find(materials, species.wood.bark)) {
        rng.state = uint64_t(k.segments.size()) * 0x632be59bd9b4e019ULL ^
                    uint64_t(k.foliage.size()) * 0x9e3779b97f4a7c15ULL ^
                    uint64_t(std::lround(k.height * 1000));
        reach = std::max(1.f, k.crownRadius);
    }

    FloraMesh run() {
        markCarriers();
        if (detail == Detail::Distant)
            prisms();
        else
            tubes();
        if (!bare) {
            if (species.foliage.type == TreeFoliageType::Frond)
                fronds();
            else
                clumps();
        }
        mesh.height = 0;
        for (const auto& v : mesh.vertices) {
            mesh.height = std::max(mesh.height, v.y);
            mesh.radius = std::max(mesh.radius, std::hypot(v.x, v.z));
        }
        return std::move(mesh);
    }

   private:
    static const FoliageMaterial* find(
        const DataStorage<FoliageMaterial>& materials,
        const std::string& name) {
        const auto it = materials.indexByName.find(name);
        return it == materials.indexByName.end() ? nullptr
                                                 : &materials.items[it->second];
    }
    float layer(const FoliageMaterial* m) const {
        return m ? float(m->id - 1) : -1.f;
    }
    // Wind weight: grows outward from the trunk and upward.
    float sway(TreeVec p) const {
        const float out = std::hypot(p.x, p.z) / reach;
        const float up = std::clamp(p.y / std::max(1.f, k.height), 0.f, 1.f);
        return std::clamp(0.75f * out + 0.25f * up * up, 0.f, 1.f);
    }
    bool room(size_t vertices) const {
        return mesh.vertices.size() + vertices <= MaxVertices;
    }
    void vertex(TreeVec p, TreeVec n, float u, float v, float layerIndex,
                float weight, const FoliageMaterial* m, Color3 color,
                Color3 dry, bool shaped) {
        FoliageVertex out{};
        out.x = p.x;
        out.y = p.y;
        out.z = p.z;
        out.nx = n.x;
        out.ny = n.y;
        out.nz = n.z;
        out.u = u;
        out.v = v;
        out.layer = layerIndex;
        out.height = weight;
        out.translucency = m ? m->translucency : 0;
        out.hueJitter = m ? m->hue_jitter : 0;
        out.valueJitter = m ? m->value_jitter : 0;
        out.normalMode = shaped ? 1 : 0;
        out.color = color.packed();
        out.dry = dry.packed();
        mesh.vertices.push_back(out);
    }
    void quad(uint16_t base) {
        mesh.indices.insert(mesh.indices.end(),
                            {base, uint16_t(base + 1), uint16_t(base + 3), base,
                             uint16_t(base + 3), uint16_t(base + 2)});
    }

    // Coarser details keep fewer clumps and thin branches; a branch still
    // carrying a kept clump (or one beyond it) is always drawn, so no clump
    // floats away from the tree.
    void markCarriers() {
        carries.assign(k.segments.size(), 0);
        if (bare || !leaf || species.foliage.type == TreeFoliageType::Frond)
            return;
        size_t index = 0;
        for (const auto& f : k.foliage) {
            if (!alive(f) || woodAt(f) > 0) continue;
            if (index++ % keepEvery()) continue;
            for (int32_t i = f.segment;
                 i >= 0 && size_t(i) < k.segments.size() && !carries[size_t(i)];
                 i = k.segments[size_t(i)].parent)
                carries[size_t(i)] = 1;
        }
    }
    size_t keepEvery() const {
        return detail == Detail::Near ? 1 : detail == Detail::Far ? 2 : 6;
    }
    bool carried(size_t segment) const {
        return segment < carries.size() && carries[segment];
    }

    // Branches too thin for blocks, as two crossed strips each.
    void tubes() {
        const FoliagePalette palette = bark ? bark->palette : FoliagePalette{};
        for (size_t i = 0; i < k.segments.size(); ++i) {
            const auto& s = k.segments[i];
            if (!s.live()) continue;
            const auto span = tubeSpan(k, s, age);
            if (!span) continue;
            const auto [start, visible] = *span;
            const float thin = k.formsWood() ? TreeVoxelRadius : INFINITY;
            const float r0 = std::min(k.radius(s, start, age), thin);
            if (detail != Detail::Near && (s.order > 1 || r0 < 0.06f) &&
                !carried(i))
                continue;
            if (r0 < 0.004f) continue;
            const TreeVec a = lerp(s.a, s.b, start),
                          b = lerp(s.a, s.b, visible);
            const TreeVec axis = (b - a).normalized();
            const float r1 = std::max(0.01f, k.radius(s, visible * 0.98f, age));
            const float w0 = std::max(0.02f, r0), w1 = std::max(0.012f, r1);
            const TreeVec u1 = perpendicular(axis);
            const TreeVec u2 = axis.cross(u1).normalized();
            const Color3 low = mix(palette.shade, palette.base, 0.6f);
            const Color3 high = palette.base;
            for (const TreeVec side : {u1, u2}) {
                if (!room(4)) return;
                const TreeVec facing = axis.cross(side).normalized();
                const auto base = uint16_t(mesh.vertices.size());
                for (int row = 0; row < 2; ++row) {
                    const TreeVec centre = row ? b : a;
                    const float half = row ? w1 : w0;
                    for (int edge = 0; edge < 2; ++edge) {
                        const float e = edge ? 1.f : -1.f;
                        const TreeVec n =
                            (side * e + facing * 0.35f).normalized();
                        vertex(centre + side * (e * half), n, float(edge),
                               1.f - float(row), -1, sway(centre), bark,
                               row ? high : low, palette.dry, true);
                    }
                }
                quad(base);
            }
        }
    }

    // Distant trees: every visible branch part as a six-sided prism (block
    // wood and thinner main branches alike), bark coloured.
    void prisms() {
        const FoliagePalette palette = bark ? bark->palette : FoliagePalette{};
        constexpr int Sides = 6;
        for (size_t i = 0; i < k.segments.size(); ++i) {
            const auto& s = k.segments[i];
            if (!s.live()) continue;
            const float visible = k.visible(s, age);
            if (visible <= 0) continue;
            float r0 = k.radius(s, 0, age);
            float r1 = k.radius(s, visible * 0.98f, age);
            const bool block =
                k.formsWood() && std::max(r0, r1) >= TreeVoxelRadius;
            // Thin parts only for main axes; blocks always (they carry the
            // silhouette of large trees).
            if (!block && (s.order > 1 || r0 < 0.15f) && !carried(i)) continue;
            if (block) {
                r0 = std::max(0.3f, r0 - 0.6f);
                r1 = std::max(0.2f, r1 - 0.6f);
            }
            if (!room(size_t(Sides) * 4)) return;
            const TreeVec a = s.a, b = lerp(s.a, s.b, visible);
            const TreeVec axis = (b - a).normalized();
            const TreeVec u = perpendicular(axis);
            const TreeVec v = axis.cross(u).normalized();
            for (int i = 0; i < Sides; ++i) {
                const float a0 = 2 * Pi * float(i) / Sides,
                            a1 = 2 * Pi * float(i + 1) / Sides;
                const TreeVec d0 = u * std::cos(a0) + v * std::sin(a0);
                const TreeVec d1 = u * std::cos(a1) + v * std::sin(a1);
                const TreeVec n = (d0 + d1).normalized();
                const auto base = uint16_t(mesh.vertices.size());
                const TreeVec corners[4] = {a + d0 * r0, a + d1 * r0,
                                            b + d0 * r1, b + d1 * r1};
                for (int c = 0; c < 4; ++c)
                    vertex(corners[c], n, float(c & 1), c < 2 ? 1.f : 0.f, -1,
                           0, bark,
                           c < 2 ? mix(palette.shade, palette.base, 0.6f)
                                 : palette.base,
                           palette.dry, true);
                quad(base);
            }
        }
    }

    bool alive(const TreeFoliageSite& f) const {
        return !f.pruned && age >= f.birth && age < f.death;
    }
    // Radius of the block wood at a foliage site (0 when its branch is
    // thinner than a block there), including the rasterizer's slack.
    float woodAt(const TreeFoliageSite& f) const {
        if (f.segment < 0 || size_t(f.segment) >= k.segments.size()) return 0;
        const auto& s = k.segments[size_t(f.segment)];
        const TreeVec axis = s.b - s.a;
        const float length2 = axis.dot(axis);
        const float t =
            length2 > 1e-6f
                ? std::clamp((f.position - s.a).dot(axis) / length2, 0.f, 1.f)
                : 0.f;
        const float r = k.radius(s, t, age);
        return k.formsWood() && r >= TreeVoxelRadius ? std::sqrt(r * r + 0.4f)
                                                     : 0.f;
    }

    // Needle and leaf clumps: cards around the site, spherical normals.
    void clumps() {
        if (!leaf) return;
        const bool needles =
            species.foliage.type == TreeFoliageType::NeedleClump;
        const int cards = detail == Detail::Near  ? (needles ? 4 : 5)
                          : detail == Detail::Far ? 3
                                                  : 2;
        const float grow = detail == Detail::Near  ? 1.f
                           : detail == Detail::Far ? 1.35f
                                                   : 2.2f;
        const size_t keep = keepEvery();
        const Color3 dry =
            leaf->withered.empty() ? leaf->palette.dry : leaf->withered[0];
        size_t index = 0;
        for (const auto& f : k.foliage) {
            if (!alive(f)) continue;
            // Leaves on wood that has grown into blocks would wrap the
            // blocks like ivy (juvenile leaves on a thickened stem).
            if (woodAt(f) > 0) continue;
            if (index++ % keep) continue;
            // Clumps fill out over their first part of the maturity.
            const float since = (age - f.birth) / std::max(1.f, k.maturity);
            const float size =
                f.size * grow * std::clamp(0.4f + since * 4, 0.4f, 1.f);
            const float radius = size * 0.5f;
            const float shade = 1 + 0.1f * rng.signedUnit();
            for (int c = 0; c < cards; ++c) {
                if (!room(4)) return;
                // Random card orientation, centre jittered inside the clump.
                const float z = rng.signedUnit();
                const float a = rng.next() * 2 * Pi;
                const float rxy = std::sqrt(std::max(0.f, 1 - z * z));
                const TreeVec normal{rxy * std::cos(a), z, rxy * std::sin(a)};
                const TreeVec right = perpendicular(normal);
                const TreeVec up = normal.cross(right).normalized();
                const TreeVec centre =
                    f.position + TreeVec{rng.signedUnit(), rng.signedUnit(),
                                         rng.signedUnit()} *
                                     (radius * 0.3f);
                const float half = size * 0.55f;
                const auto base = uint16_t(mesh.vertices.size());
                for (int row = 0; row < 2; ++row)
                    for (int col = 0; col < 2; ++col) {
                        const TreeVec p = centre +
                                          right * (half * (col ? 1.f : -1.f)) +
                                          up * (half * (row ? 1.f : -1.f));
                        const TreeVec d = (p - f.position) * (1 / radius);
                        const TreeVec n = d.normalized();
                        const float t = std::clamp(
                            0.45f * d.length() + 0.55f * (0.5f + 0.5f * d.y),
                            0.f, 1.f);
                        vertex(p, n, float(col), 1.f - float(row), layer(leaf),
                               sway(p), leaf,
                               scaled(gradientColor(leaf->palette, t), shade),
                               dry, true);
                    }
                quad(base);
            }
        }
    }

    // Palm fronds: strips arching out along the site's direction and
    // drooping toward the tip.
    void fronds() {
        if (!leaf) return;
        const Color3 dry =
            leaf->withered.empty() ? leaf->palette.dry : leaf->withered[0];
        const int segments = detail == Detail::Near ? 4 : 2;
        for (const auto& f : k.foliage) {
            if (!alive(f)) continue;
            if (!room(size_t(segments + 1) * 2)) return;
            const float since = (age - f.birth) / std::max(1.f, k.maturity);
            const float length =
                f.size * std::clamp(0.3f + since * 6, 0.3f, 1.f);
            const TreeVec dir = f.direction.normalized();
            // Fronds start at the surface of a block stem, not inside it.
            TreeVec origin = f.position;
            if (const float wood = woodAt(f); wood > 0) {
                const TreeVec out =
                    TreeVec{dir.x, 0, dir.z}.normalized() * wood;
                origin = origin + out + TreeVec{0, 0.3f, 0};
            }
            const TreeVec right =
                std::abs(dir.y) > 0.99f
                    ? TreeVec{1, 0, 0}
                    : dir.cross(TreeVec{0, 1, 0}).normalized();
            const float droop = 0.35f + 0.15f * rng.next();
            const float width = length * 0.3f;
            const float shade = 1 + 0.1f * rng.signedUnit();
            const auto base = uint16_t(mesh.vertices.size());
            for (int row = 0; row <= segments; ++row) {
                const float t = float(row) / float(segments);
                const TreeVec centre =
                    origin + dir * (length * t) +
                    TreeVec{0, -1, 0} * (droop * length * t * t);
                const TreeVec tangent =
                    (dir * length +
                     TreeVec{0, -1, 0} * (2 * droop * length * t))
                        .normalized();
                const TreeVec normal = right.cross(tangent).normalized();
                for (int side = 0; side < 2; ++side) {
                    const float e = side ? 1.f : -1.f;
                    const TreeVec p = centre + right * (e * width * 0.5f);
                    vertex(
                        p, normal, float(side), 1.f - t, layer(leaf),
                        std::max(sway(p), t), leaf,
                        scaled(gradientColor(leaf->palette, 0.25f + 0.75f * t),
                               shade),
                        dry, false);
                }
            }
            for (int row = 0; row < segments; ++row)
                quad(uint16_t(base + row * 2));
        }
    }

    const TreeSkeleton& k;
    const Tree& species;
    float age;
    Detail detail;
    bool bare;
    const FoliageMaterial* leaf;
    const FoliageMaterial* bark;
    Rng rng{0};
    float reach = 1;
    std::vector<uint8_t> carries;  // segments holding up a kept clump
    FloraMesh mesh;
};

}  // namespace

std::optional<std::pair<float, float>> tubeSpan(const TreeSkeleton& k,
                                                const TreeSegment& s,
                                                float age) {
    const float visible = k.visible(s, age);
    if (visible <= 0) return std::nullopt;
    // Block wood shows the part at least a block thick: the strips start
    // where the segment thins below it (tapering limbs cross it midway).
    const float thin = k.formsWood() ? TreeVoxelRadius : INFINITY;
    if (k.radius(s, 0, age) < thin) return std::pair{0.f, visible};
    if (k.radius(s, visible, age) >= thin) return std::nullopt;
    float lo = 0, hi = visible;
    for (int step = 0; step < 12; ++step) {
        const float mid = 0.5f * (lo + hi);
        (k.radius(s, mid, age) >= thin ? lo : hi) = mid;
    }
    return std::pair{lo, visible};
}

FloraMesh build(const TreeSkeleton& skeleton, const Tree& species,
                const DataStorage<FoliageMaterial>& materials, float age,
                Detail detail, bool bare) {
    if (age <= 0) return {};
    return Builder(skeleton, species, materials, age, detail, bare).run();
}

FloraMesh buildBillboard(const TreeSkeleton& shape, const Tree& species,
                         const DataStorage<FoliageMaterial>& materials) {
    const auto find = [&](const std::string& name) -> const FoliageMaterial* {
        const auto it = materials.indexByName.find(name);
        return it == materials.indexByName.end() ? nullptr
                                                 : &materials.items[it->second];
    };
    const char* kind = species.form == TreeForm::Excurrent ? "conifer"
                       : species.form == TreeForm::Palm    ? "palm"
                       : species.form == TreeForm::Stunted ? "pine"
                                                           : "broadleaf";
    const auto* card = find(std::string("foliage.billboard_") + kind);
    const auto* leaf = find(species.foliage.material);
    const auto* bark = find(species.wood.bark);
    if (!card || !leaf) return {};
    const float height = std::max(1.f, shape.boundsMax.y);
    const float crown = std::max({1.f, -shape.boundsMin.x, shape.boundsMax.x,
                                  -shape.boundsMin.z, shape.boundsMax.z});
    // Where the crown starts: the lowest leaves of the full-grown tree.
    float low = height;
    for (const auto& f : shape.foliage)
        if (!f.pruned && f.death == INFINITY)
            low = std::min(low, f.position.y - f.size * 0.5f);
    float y0 = std::clamp(low / height, 0.f, 0.85f), y1 = 1;
    if (species.form == TreeForm::Palm) {
        // The frond burst's centre (62% up the card) at the top.
        const float span = 2 * crown / height;
        y1 = 1 + 0.38f * span * 0.6f;
        y0 = y1 - span;
    }
    FloraMesh mesh;
    const auto vertex = [&](TreeVec p, TreeVec n, float u, float v, float layer,
                            const FoliageMaterial* m, Color3 c, Color3 dry) {
        FoliageVertex out{};
        out.x = p.x;
        out.y = p.y;
        out.z = p.z;
        out.nx = n.x;
        out.ny = n.y;
        out.nz = n.z;
        out.u = u;
        out.v = v;
        out.layer = layer;
        out.height = 0;  // no wind at this distance
        out.translucency = m ? m->translucency : 0;
        out.hueJitter = m ? m->hue_jitter : 0;
        out.valueJitter = m ? m->value_jitter : 0;
        out.normalMode = 1;
        out.color = c.packed();
        out.dry = dry.packed();
        mesh.vertices.push_back(out);
    };
    const auto quad = [&](float angle, float halfWidth, float bottom, float top,
                          float layer, const FoliageMaterial* m, Color3 low,
                          Color3 high, Color3 dry) {
        const TreeVec side{std::cos(angle), 0, std::sin(angle)};
        const auto base = uint16_t(mesh.vertices.size());
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col) {
                const float s = col ? 1.f : -1.f;
                // Soft volume normals: outward along the card, mostly up.
                const TreeVec n =
                    (side * (0.6f * s) + TreeVec{0, 0.8f, 0}).normalized();
                vertex(
                    side * (halfWidth * s) + TreeVec{0, row ? top : bottom, 0},
                    n, float(col), row ? 0.f : 1.f, layer, m, row ? high : low,
                    dry);
            }
        mesh.indices.insert(mesh.indices.end(),
                            {base, uint16_t(base + 1), uint16_t(base + 3), base,
                             uint16_t(base + 3), uint16_t(base + 2)});
    };
    const Color3 dry =
        leaf->withered.empty() ? leaf->palette.dry : leaf->withered[0];
    const auto& p = leaf->palette;
    // Trunk: two crossed bark quads up into the crown.
    const auto barkPalette = bark ? bark->palette : FoliagePalette{};
    const float trunk = std::max(0.03f, shape.trunkRadius / crown);
    const float trunkTop = std::min(1.f, std::max(y0 + 0.15f, 0.2f));
    for (const float a : {0.f, 1.5707963f})
        quad(a, trunk, 0, trunkTop, -1, bark, barkPalette.shade,
             barkPalette.base, barkPalette.dry);
    for (int i = 0; i < 3; ++i)
        quad(3.14159265f * float(i) / 3, 1, y0, y1, float(card->id - 1), leaf,
             mix(p.shade, p.base, 0.5f), mix(p.base, p.tip, 0.25f), dry);
    mesh.height = y1;
    mesh.radius = 1;
    return mesh;
}

}  // namespace tree_foliage
