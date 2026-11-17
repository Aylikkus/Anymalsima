#include "flora_mesh_builder.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

constexpr float Pi = 3.14159265358979f;
// How far a canopy may reach past its tile footprint, in metres; and the
// largest instance scale (FloraInstance::scale).
constexpr float FootprintOverhang = 0.3f;
constexpr float MaxInstanceScale = 1.15f;

struct V3 {
    float x = 0, y = 0, z = 0;
    V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    V3 operator-() const { return {-x, -y, -z}; }
};
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
V3 normalize(V3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 1e-6f ? v * (1 / l) : V3{0, 1, 0};
}
V3 mul(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
V3 horizontal(float angle) { return {std::cos(angle), 0, std::sin(angle)}; }
// Rotates v around unit axis k by angle (Rodrigues).
V3 rotate(V3 v, V3 k, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}

class Rng {
   public:
    explicit Rng(uint64_t seed) : state(seed) {}
    float next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return float((z ^ (z >> 31)) >> 40) / float(1 << 24);
    }
    float range(const ValueRange& r) { return r.sample(next()); }
    float range(const std::optional<ValueRange>& r, float fallback) {
        return r ? r->sample(next()) : fallback;
    }
    int count(const ValueRange& r) { return std::max(0, r.sampleInt(next())); }
    float signedUnit() { return next() * 2 - 1; }

   private:
    uint64_t state;
};

Color3 mix(const Color3& a, const Color3& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t};
}

// Vertex before colours are resolved (they need the whole mesh height).
struct Raw {
    V3 p, n;
    float u, v, layer;
    // Clump gradient position: 0 deep and low in a clump, 1 at its top
    // surface; -1 outside clumps.
    float radial;
    float shade;  // per-card value multiplier
    bool shaped;  // normal mode: spherical/cylindrical instead of card
    const FoliageMaterial* material;
};

struct Stem {
    V3 base, up, bendDir;
    float height, bend;
    V3 at(float t) const {
        return base + up * (height * t) + bendDir * (bend * height * t * t);
    }
    V3 tangent(float t) const {
        return normalize(up * height + bendDir * (2 * bend * height * t));
    }
};

struct Clump {
    V3 center, radii;
};

class Builder {
   public:
    Builder(const Flora& flora, const DataStorage<FoliageMaterial>& materials,
            int variant, FloraStage stage)
        : flora(flora), materials(materials), variant(variant), stage(stage) {}

    FloraMesh run();

   private:
    // A card strip: `segments` rows from base along `up`, curving toward
    // bendDir; u across, v = 1 at the base (cards are authored root down).
    struct Card {
        V3 base, right, up, bendDir;
        float width, height, bend = 0;
        int segments = 1;
        float layer = 0;
        float shade = 1;
        const FoliageMaterial* material = nullptr;
        // Shaped normals: spherical around a clump, or blended upward.
        const Clump* clump = nullptr;
        float upBlend = 0;      // > 0: mix the card normal toward +Y (shaped)
        bool cylinder = false;  // normals point away from the strip's axis
        float gradient = -1;    // >= 0: fixed gradient position (radial)
        float taper = 0;        // narrows the top by this fraction
    };
    void card(const Card& c);
    void crossed(Card c, int count, float yaw);
    const FoliageMaterial* material(const FloraPart& part) const {
        const auto it = materials.indexByName.find(part.material);
        return it == materials.indexByName.end() ? nullptr
                                                 : &materials.items[it->second];
    }
    float layer(const FoliageMaterial* m) const {
        return m ? float(m->id - 1) : -1.f;
    }
    Rng rng(size_t part) const {
        return Rng(uint64_t(flora.id) * 0x9e3779b97f4a7c15ULL ^
                   uint64_t(variant + 1) * 0xbf58476d1ce4e5b9ULL ^
                   uint64_t(part + 1) * 0x94d049bb133111ebULL);
    }
    float shadeFor(const FloraPart& part, Rng& r) {
        return 1 + part.color_variation * r.signedUnit();
    }

    void stems(const FloraPart& part, size_t index, float scale);
    void clumps(const FloraPart& part, size_t index, float scale);
    void blades(const FloraPart& part, size_t index, float scale);
    void leafCards(const FloraPart& part, size_t index, float scale);
    void petals(const FloraPart& part, size_t index, float scale);
    void heads(const FloraPart& part, size_t index, float scale);
    void fruit(const FloraPart& part, size_t index, float scale);
    void fronds(const FloraPart& part, size_t index, float scale);
    // Attachment points of a part (stem tips, along stems, on clumps).
    std::vector<std::pair<V3, V3>> anchors(const FloraPart& part, Rng& r,
                                           int count);

    const Flora& flora;
    const DataStorage<FoliageMaterial>& materials;
    int variant;
    FloraStage stage;
    std::vector<Raw> raw;
    std::vector<uint16_t> indices;
    std::vector<Stem> stemList;
    std::vector<Clump> clumpList;
};

void Builder::card(const Card& c) {
    if (raw.size() + size_t(c.segments + 1) * 2 > 65535) return;
    const auto base = uint16_t(raw.size());
    for (int row = 0; row <= c.segments; ++row) {
        const float t = float(row) / float(c.segments);
        const V3 centre = c.base + c.up * (c.height * t) +
                          c.bendDir * (c.bend * c.height * t * t);
        const V3 tangent = normalize(c.up * c.height +
                                     c.bendDir * (2 * c.bend * c.height * t));
        const V3 cardNormal = normalize(cross(c.right, tangent));
        const float half = 0.5f * c.width * (1 - c.taper * t);
        for (int side = 0; side < 2; ++side) {
            const float s = side ? 1.f : -1.f;
            const V3 p = centre + c.right * (s * half);
            V3 n = cardNormal;
            bool shaped = false;
            float radial = c.gradient;
            if (c.clump) {
                const V3 d{(p.x - c.clump->center.x) / c.clump->radii.x,
                           (p.y - c.clump->center.y) / c.clump->radii.y,
                           (p.z - c.clump->center.z) / c.clump->radii.z};
                n = normalize(d);
                // Inner and lower cards take the shade colour, the upper
                // shell the tip colour, most of the surface the base.
                radial = std::clamp(
                    0.45f * std::sqrt(dot(d, d)) + 0.55f * (0.5f + 0.5f * d.y),
                    0.f, 1.f);
                shaped = true;
            } else if (c.cylinder) {
                n = normalize(c.right * s + cardNormal * 0.35f);
                shaped = true;
            } else if (c.upBlend > 0) {
                V3 up = cardNormal.y < 0 ? -cardNormal : cardNormal;
                n = normalize(up * (1 - c.upBlend) + V3{0, 1, 0} * c.upBlend);
                shaped = true;
            }
            raw.push_back({p, n, float(side), 1 - t, c.layer, radial, c.shade,
                           shaped, c.material});
        }
    }
    for (int row = 0; row < c.segments; ++row) {
        const auto i = uint16_t(base + row * 2);
        indices.insert(indices.end(), {i, uint16_t(i + 1), uint16_t(i + 3), i,
                                       uint16_t(i + 3), uint16_t(i + 2)});
    }
}

void Builder::crossed(Card c, int count, float yaw) {
    for (int i = 0; i < count; ++i) {
        const float a = yaw + Pi * float(i) / float(count);
        c.right = horizontal(a);
        card(c);
    }
}

std::vector<std::pair<V3, V3>> Builder::anchors(const FloraPart& part, Rng& r,
                                                int count) {
    // (point, outward direction) per attachment.
    std::vector<std::pair<V3, V3>> out;
    const float golden =
        part.phyllotaxis ? part.phyllotaxis->sample(r.next()) : 137.5f;
    for (int i = 0; i < count; ++i) {
        switch (part.attach) {
            case FloraAttach::StemTip:
                if (!stemList.empty()) {
                    const auto& s = stemList[size_t(i) % stemList.size()];
                    out.push_back({s.at(1), s.tangent(1)});
                }
                break;
            case FloraAttach::Stem:
                if (!stemList.empty()) {
                    const auto& s = stemList[size_t(i) % stemList.size()];
                    const float t = 0.25f + 0.6f * (float(i) + r.next()) /
                                                float(std::max(1, count));
                    const V3 dir =
                        horizontal(float(i) * golden * Pi / 180 + r.next());
                    out.push_back({s.at(std::min(t, 0.95f)), dir});
                }
                break;
            case FloraAttach::LeafClumpSurface:
                if (!clumpList.empty()) {
                    const auto& c =
                        clumpList[size_t(r.next() * clumpList.size()) %
                                  clumpList.size()];
                    // Mostly the upper, outer shell where fruit shows.
                    const float a = r.next() * 2 * Pi;
                    const float y = 0.1f + 0.85f * r.next();
                    const float h = std::sqrt(std::max(0.f, 1 - y * y));
                    const V3 d{std::cos(a) * h, y, std::sin(a) * h};
                    out.push_back({c.center + mul(d, c.radii) * 0.98f, d});
                }
                break;
            case FloraAttach::Ground: {
                const float a = float(i) * golden * Pi / 180 + r.next() * 0.3f;
                out.push_back(
                    {horizontal(a) * (0.02f * r.next()), horizontal(a)});
                break;
            }
        }
    }
    return out;
}

void Builder::stems(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const int count = r.count(part.count);
    for (int i = 0; i < count; ++i) {
        const float h = r.range(part.height_m, 0.3f) * scale;
        const float w = r.range(part.width_m, 0.015f) * std::sqrt(scale);
        const float bend = r.range(part.bend, 0.1f);
        const V3 dir = horizontal(r.next() * 2 * Pi);
        const float spread = count > 1 ? 0.04f * scale : 0.f;
        Stem s{dir * (spread * r.next()), {0, 1, 0}, dir, h, bend};
        stemList.push_back(s);
        Card c;
        c.base = s.base;
        c.up = s.up;
        c.bendDir = s.bendDir;
        c.width = std::max(w, 0.004f);
        c.height = h;
        c.bend = bend;
        c.segments = 4;
        c.layer = -1;  // untextured
        c.material = material(part);
        c.shade = shadeFor(part, r);
        c.cylinder = true;
        c.taper = 0.5f;
        crossed(c, 2, r.next() * Pi);
    }
}

void Builder::clumps(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    const int count = std::max(1, r.count(part.count));
    const FloraShell shell = part.shell.value_or(FloraShell{});
    for (int i = 0; i < count; ++i) {
        V3 radii = V3{r.range(shell.radii_m[0]), r.range(shell.radii_m[1]),
                      r.range(shell.radii_m[2])} *
                   scale;
        // Clumps overlap around the trunk, the first one central. Their
        // shells reach the ground (lift_m raises them, for shrubs on stems),
        // so a bush sits on the ground instead of floating above it.
        const float a = r.next() * 2 * Pi;
        const float off = i == 0 ? 0.f : 0.45f + 0.25f * r.next();
        // Plants with a tile footprint keep their canopy over it (plus a
        // small overhang, at the largest instance scale), so they never grow
        // into neighbouring blocks.
        if (flora.footprint.tiles) {
            const float card = 0.5f * r.range(part.card_size_m, 0.4f) * scale;
            for (const auto [axis, tiles] :
                 {std::pair{&radii.x, (*flora.footprint.tiles)[0]},
                  std::pair{&radii.z, (*flora.footprint.tiles)[1]}}) {
                const float limit = (0.5f * float(tiles) + FootprintOverhang) /
                                    MaxInstanceScale;
                *axis = std::min(*axis,
                                 std::max(0.05f, (limit - card) / (1 + off)));
            }
        }
        const V3 centre{std::cos(a) * radii.x * off,
                        r.range(shell.lift_m) * scale +
                            radii.y * (i == 0 ? 0.6f : 0.45f + 0.2f * r.next()),
                        std::sin(a) * radii.z * off};
        clumpList.push_back({centre, radii});
        const Clump* clump = &clumpList.back();
        const int cards =
            std::max(1, int(std::lround(r.range(part.cards_per_clump, 20.f))));
        for (int k = 0; k < cards; ++k) {
            // Mostly the upper hemisphere, but down to the ground too.
            const float y = -0.9f + 1.9f * std::sqrt(r.next());
            const float h = std::sqrt(std::max(0.f, 1 - y * y));
            const float az = r.next() * 2 * Pi;
            const V3 d{std::cos(az) * h, y, std::sin(az) * h};
            const V3 at = centre + mul(d, radii) * (0.7f + 0.3f * r.next());
            V3 right = normalize(cross({0, 1, 0}, d));
            if (dot(right, right) < 0.5f) right = {1, 0, 0};
            right = rotate(right, d, r.signedUnit() * 0.6f);
            const V3 up = normalize(cross(d, right));
            const float size = r.range(part.card_size_m, 0.4f) * scale;
            Card c;
            c.base = at - up * (size * 0.5f);
            c.right = right;
            c.up = up;
            c.bendDir = d;
            c.width = size;
            c.height = size;
            c.layer = layer(m);
            c.material = m;
            c.shade = shadeFor(part, r);
            c.clump = clump;
            card(c);
        }
        // A skirt of near-flat leaves on the ground under the clump, in the
        // shade colour: a painted contact shadow tying the bush to the
        // ground.
        const int skirt = std::max(2, cards / 5);
        for (int k = 0; k < skirt; ++k) {
            const float az = r.next() * 2 * Pi;
            const float reach = 0.35f + 0.65f * r.next();
            const V3 out = horizontal(az);
            const float size = r.range(part.card_size_m, 0.4f) * scale;
            Card c;
            c.right = normalize(cross({0, 1, 0}, out));
            // Lying outward, tilted up 10-30 degrees.
            const float tilt = (10 + 20 * r.next()) * Pi / 180;
            c.up =
                normalize(out * std::cos(tilt) + V3{0, 1, 0} * std::sin(tilt));
            c.base = V3{centre.x + out.x * radii.x * reach * 0.5f, 0.02f,
                        centre.z + out.z * radii.z * reach * 0.5f} -
                     c.up * (size * 0.3f);
            c.bendDir = {0, -1, 0};
            c.width = size;
            c.height = size;
            c.layer = layer(m);
            c.material = m;
            c.shade = 0.8f * shadeFor(part, r);
            c.upBlend = 0.6f;
            c.gradient = 0.05f;
            card(c);
        }
    }
}

void Builder::blades(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    const int count = std::max(1, r.count(part.count));
    // A tuft: crossed cards of the blade texture, each leaning outward.
    const int cards = std::clamp(count / 3, 2, 5);
    for (int i = 0; i < cards; ++i) {
        const float yaw = Pi * (float(i) + 0.3f * r.next()) / float(cards);
        const V3 right = horizontal(yaw);
        const V3 out =
            normalize(cross(right, {0, 1, 0})) * (r.next() < 0.5f ? 1.f : -1.f);
        const float lean = r.range(part.spread_angle, 15.f) * Pi / 180 *
                           (0.5f + 0.5f * r.next());
        const float len = r.range(part.length_m, 0.4f) * scale;
        Card c;
        c.base = out * (0.03f * r.next());
        c.right = right;
        c.up = normalize(rotate({0, 1, 0}, right, out.z >= 0 ? lean : -lean));
        if (c.up.y < 0.2f) c.up = {0, 1, 0};
        c.bendDir = out;
        c.width = len * 0.9f;
        c.height = len;
        c.bend = r.range(part.bend, 0.2f) * 0.5f;
        c.segments = 3;
        c.layer = layer(m);
        c.material = m;
        c.shade = shadeFor(part, r);
        c.upBlend = 0.55f;
        card(c);
    }
}

void Builder::leafCards(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    const int count = r.count(part.count);
    for (const auto& [at, dir] : anchors(part, r, count)) {
        const float len = r.range(part.length_m, 0.12f) * scale;
        const float width = r.range(part.width_m, 0.05f) * scale;
        V3 out{dir.x, 0, dir.z};
        out = normalize(dot(out, out) < 1e-4f ? V3{1, 0, 0} : out);
        const float droop = r.range(part.droop, 0.2f);
        // Leaves rise 50 degrees from horizontal, drooping lowers them.
        const float rise = (50 - 60 * droop) * Pi / 180;
        const V3 right = normalize(cross({0, 1, 0}, out));
        Card c;
        c.base = at;
        c.right = right;
        c.up = normalize(out * std::cos(rise) + V3{0, 1, 0} * std::sin(rise));
        c.bendDir = {0, -1, 0};
        c.width = std::max(width * 2.2f, len * 0.5f);
        c.height = len;
        c.bend = droop * 0.4f;
        c.segments = 2;
        c.layer = layer(m);
        c.material = m;
        c.shade = shadeFor(part, r);
        c.upBlend = 0.4f;
        card(c);
    }
}

void Builder::petals(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    const int count = std::max(1, r.count(part.count));
    FloraPart tip = part;
    if (tip.attach == FloraAttach::Ground && !stemList.empty())
        tip.attach = FloraAttach::StemTip;
    const auto heads = anchors(tip, r, std::max<int>(1, int(stemList.size())));
    for (const auto& [at, axis] : heads) {
        const float spread = r.range(part.spread_angle, 70.f) * Pi / 180;
        const float turn = r.next() * 2 * Pi;
        for (int i = 0; i < count; ++i) {
            const V3 out = horizontal(turn + 2 * Pi * float(i) / float(count));
            const V3 right = normalize(cross({0, 1, 0}, out));
            const float len = r.range(part.length_m, 0.04f) * scale;
            const float width = r.range(part.width_m, 0.02f) * scale;
            Card c;
            c.base = at;
            c.right = right;
            c.up = normalize(V3{0, 1, 0} * std::cos(spread) +
                             out * std::sin(spread));
            c.bendDir = {0, -1, 0};
            c.width = width * 2;  // the petal fills half its card
            c.height = len;
            c.bend = 0.15f;
            c.segments = 2;
            c.layer = layer(m);
            c.material = m;
            c.shade = shadeFor(part, r);
            c.upBlend = 0.6f;
            card(c);
        }
        (void)axis;
    }
}

void Builder::heads(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    FloraPart tip = part;
    if (tip.attach == FloraAttach::Ground && !stemList.empty())
        tip.attach = FloraAttach::StemTip;
    const int count = std::max(1, r.count(part.count));
    for (const auto& [at, axis] : anchors(tip, r, count)) {
        // A disc facing up (the camera mostly looks down), just above the
        // petals.
        const float size = r.range(part.length_m, 0.05f) * scale;
        const V3 forward = horizontal(r.next() * 2 * Pi);
        Card c;
        c.right = normalize(cross(forward, {0, 1, 0}));
        c.up = forward;
        c.base = at + V3{0, 0.006f, 0} - forward * (size * 0.5f);
        c.bendDir = {0, 1, 0};
        c.width = size;
        c.height = size;
        c.layer = layer(m);
        c.material = m;
        c.shade = shadeFor(part, r);
        c.upBlend = 0.3f;
        card(c);
        (void)axis;
    }
}

void Builder::fruit(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    const int count = std::max(1, r.count(part.count));
    FloraPart attach = part;
    if (attach.attach == FloraAttach::Ground)
        attach.attach = clumpList.empty() ? FloraAttach::StemTip
                                          : FloraAttach::LeafClumpSurface;
    for (const auto& [at, out] : anchors(attach, r, count)) {
        const float size = r.range(part.card_size_m, 0.14f) * std::sqrt(scale);
        Card c;
        c.base = at + out * (size * 0.15f) - V3{0, size * 0.5f, 0};
        c.up = {0, 1, 0};
        c.bendDir = out;
        c.width = size;
        c.height = size;
        c.layer = layer(m);
        c.material = m;
        c.shade = shadeFor(part, r);
        crossed(c, 2, r.next() * Pi);
    }
}

void Builder::fronds(const FloraPart& part, size_t index, float scale) {
    auto r = rng(index);
    const auto* m = material(part);
    const int count = std::max(1, r.count(part.count));
    for (const auto& [at, dir] : anchors(part, r, count)) {
        V3 out{dir.x, 0, dir.z};
        out = normalize(dot(out, out) < 1e-4f ? V3{1, 0, 0} : out);
        const float spread = r.range(part.spread_angle, 45.f) * Pi / 180;
        const float len = r.range(part.length_m, 0.6f) * scale;
        Card c;
        c.base = at;
        c.right = normalize(cross({0, 1, 0}, out));
        c.up =
            normalize(V3{0, 1, 0} * std::cos(spread) + out * std::sin(spread));
        // Droop pulls the tip down, past horizontal for a strong droop.
        c.bendDir = normalize(out * 0.4f + V3{0, -1, 0});
        c.bend = r.range(part.droop, 0.4f) * 0.9f;
        c.width = r.range(part.width_m, 0.18f) * scale * 2.2f;
        c.height = len;
        c.segments = 5;
        c.layer = layer(m);
        c.material = m;
        c.shade = shadeFor(part, r);
        c.upBlend = 0.45f;
        c.taper = 0.3f;
        card(c);
    }
}

FloraMesh Builder::run() {
    // Stems and clumps first: other parts attach to them.
    const auto order = [](FloraPartType t) {
        return t == FloraPartType::Stem        ? 0
               : t == FloraPartType::LeafClump ? 1
                                               : 2;
    };
    clumpList.reserve(64);  // Card::clump points into it
    for (int pass = 0; pass < 3; ++pass)
        for (size_t i = 0; i < flora.parts.size(); ++i) {
            const auto& part = flora.parts[i];
            if (order(part.type) != pass) continue;
            const float scale = part.stageScale(stage);
            if (scale <= 0) {
                // Hidden parts still provide anchors for attached parts.
                if (part.type != FloraPartType::Stem) continue;
            }
            const auto before = raw.size();
            switch (part.type) {
                case FloraPartType::Stem:
                    stems(part, i, std::max(scale, 0.05f));
                    if (scale <= 0) {
                        raw.resize(before);
                        indices.erase(
                            std::remove_if(
                                indices.begin(), indices.end(),
                                [&](uint16_t v) { return v >= before; }),
                            indices.end());
                    }
                    break;
                case FloraPartType::LeafClump:
                    if (clumpList.size() < 48) clumps(part, i, scale);
                    break;
                case FloraPartType::Blade:
                    blades(part, i, scale);
                    break;
                case FloraPartType::LeafCard:
                    leafCards(part, i, scale);
                    break;
                case FloraPartType::PetalRing:
                    petals(part, i, scale);
                    break;
                case FloraPartType::FlowerHead:
                    heads(part, i, scale);
                    break;
                case FloraPartType::FruitCluster:
                    fruit(part, i, scale);
                    break;
                case FloraPartType::Frond:
                    fronds(part, i, scale);
                    break;
            }
        }
    FloraMesh mesh;
    if (indices.empty()) return mesh;
    float top = 0.01f, radius = 0;
    for (const auto& v : raw) {
        top = std::max(top, v.p.y);
        radius = std::max(radius, std::sqrt(v.p.x * v.p.x + v.p.z * v.p.z));
    }
    mesh.height = top;
    mesh.radius = radius;
    mesh.vertices.reserve(raw.size());
    for (const auto& v : raw) {
        const float height = std::clamp(v.p.y / top, 0.f, 1.f);
        const FoliagePalette palette =
            v.material ? v.material->palette : FoliagePalette{};
        const auto gradient =
            v.material ? v.material->gradient : FoliageGradient::RootToTip;
        float t = 0.5f;
        if (gradient == FoliageGradient::RootToTip) t = height;
        if (gradient == FoliageGradient::CenterToEdge)
            t = v.radial >= 0 ? v.radial : height;
        // The base colour dominates; shade only deep or low, tip only at
        // the very top (a painted volume, not a rainbow).
        Color3 c = t < 0.35f ? mix(palette.shade, palette.base, t / 0.35f)
                   : t < 0.8f
                       ? palette.base
                       : mix(palette.base, palette.tip, (t - 0.8f) / 0.2f);
        c = {c.r * v.shade, c.g * v.shade, c.b * v.shade};
        const Color3 dry = v.material && !v.material->withered.empty()
                               ? v.material->withered.front()
                               : palette.dry;
        FoliageVertex out{};
        out.x = v.p.x;
        out.y = v.p.y;
        out.z = v.p.z;
        out.nx = v.n.x;
        out.ny = v.n.y;
        out.nz = v.n.z;
        out.u = v.u;
        out.v = v.v;
        out.layer = v.layer;
        out.height = height;
        out.translucency = v.material ? v.material->translucency : 0.3f;
        out.hueJitter = v.material ? v.material->hue_jitter : 0.04f;
        out.valueJitter = v.material ? v.material->value_jitter : 0.1f;
        out.normalMode = v.shaped ? 1.f : 0.f;
        out.color = c.packed();
        out.dry = dry.packed();
        mesh.vertices.push_back(out);
    }
    mesh.indices = std::move(indices);
    return mesh;
}

}  // namespace

FloraMesh FloraMeshBuilder::build(uint8_t species, int variant,
                                  FloraStage stage) const {
    if (!assets.hasStorage<Flora>() || !assets.hasStorage<FoliageMaterial>())
        return {};
    const auto& all = assets.getStorage<Flora>();
    if (!species || species > all.items.size()) return {};
    return Builder(all.get(species), assets.getStorage<FoliageMaterial>(),
                   variant % Variants, stage)
        .run();
}
