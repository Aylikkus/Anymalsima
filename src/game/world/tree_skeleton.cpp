#include "tree_skeleton.hpp"

#include <algorithm>
#include <numbers>

namespace {
constexpr float Pi = std::numbers::pi_v<float>;
constexpr float GoldenAngle = 2.39996323f;
constexpr TreeVec Up{0, 1, 0};
// Share of the maturity over which tips extend; the rest only thickens.
constexpr float ExtendShare = 0.85f;

// splitmix64: deterministic everywhere, unlike std distributions.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed ^ 0x632BE59BD9B4E019ull) {}
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float unit() { return float(next() >> 40) / float(1u << 24); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
    float sample(const ValueRange& r) { return r.sample(unit()); }
    int sampleInt(const ValueRange& r) { return r.sampleInt(unit()); }
    float jitter(float amount) { return (unit() * 2 - 1) * amount; }
};

TreeVec perpendicular(TreeVec d) {
    const TreeVec ref = std::abs(d.y) < 0.9f ? Up : TreeVec{1, 0, 0};
    return d.cross(ref).normalized();
}
// The direction `angle` radians away from axis d, turned by `azimuth`
// around it. For a vertical axis, azimuth 0 points along -z.
TreeVec around(TreeVec d, float angle, float azimuth) {
    const TreeVec u = perpendicular(d), v = d.cross(u).normalized();
    const TreeVec side = u * std::cos(azimuth) + v * std::sin(azimuth);
    return (d * std::cos(angle) + side * std::sin(angle)).normalized();
}
float radians(float degrees) { return degrees * Pi / 180; }

class Builder {
   public:
    Builder(const Tree& tree, const TreeSkeletonInput& input)
        : tree(tree), p(tree.form_params), input(input), rng(input.seed) {}
    // Continues a finished skeleton (applyTreeCuts).
    Builder(const Tree& tree, const TreeSkeletonInput& input,
            const TreeSkeleton& existing)
        : Builder(tree, input) {
        skeleton = existing;
        clumpSize = existing.clumpSize;
        maturity = existing.maturity;
        windAzimuth = existing.windAzimuth;
    }

    TreeSkeletonPtr cut(const std::vector<TreeCut>& cuts, size_t first) {
        applyCuts(cuts, first);
        clampToHeadroom(skeleton.headroom);
        return finish();
    }

    TreeSkeletonPtr run() {
        auto& s = skeleton;
        s.curve = tree.growth.curve;
        s.maturity = std::max(1.f, rng.sample(tree.growth.maturity_days));
        s.lifespan =
            std::max(s.maturity, rng.sample(tree.growth.lifespan_days));
        const float sampledHeight = rng.sample(tree.size.height_m);
        s.trunkRadius = rng.sample(tree.size.trunk_radius_m);
        s.crownRadius = rng.sample(tree.size.crown_radius_m);
        clumpSize = rng.sample(tree.foliage.clump_size_m);
        // Leave room for the crown's clumps above the highest wood.
        const float margin = tree.foliage.type == TreeFoliageType::Frond
                                 ? clumpSize * 0.3f
                                 : clumpSize * 0.6f;
        const float room = input.headroom - margin;
        if (room < 1) return finish();  // no room to grow at all
        s.height = std::min(sampledHeight, room);
        if (s.height < sampledHeight) {
            // A capped tree spreads instead of growing up, and its trunk
            // stays in proportion (a sapling planted in a room).
            const float ratio = s.height / sampledHeight;
            s.crownRadius *= std::min(1.4f, 1 / std::sqrt(ratio));
            s.trunkRadius *= std::sqrt(ratio);
        }
        top = s.height;
        maturity = s.maturity;
        const float leanAzimuth = rng.range(0, 2 * Pi);
        lean = around(Up, radians(rng.sample(p.lean_deg)), leanAzimuth);
        windAzimuth = leanAzimuth;
        // Root flare (round; giants replace it with buttresses).
        s.flare = {p.root_flare, s.trunkRadius * 0.8f + 1, 0, 0};

        switch (tree.form) {
            case TreeForm::Excurrent:
                excurrent();
                break;
            case TreeForm::Decurrent:
                decurrent(false);
                break;
            case TreeForm::Stunted:
                decurrent(true);
                break;
            case TreeForm::Palm:
                palm();
                break;
            case TreeForm::Giant:
                giant();
                break;
        }
        fit(input.headroom);
        pruneByGround();
        clampToHeadroom(input.headroom);
        s.seed = input.seed;
        s.headroom = input.headroom;
        s.clumpSize = clumpSize;
        s.windAzimuth = windAzimuth;
        auto built = finish();
        if (input.rotation & 3)
            built = rotateTreeSkeleton(*built, input.rotation);
        if (!input.cuts.empty())
            built = applyTreeCuts(*built, tree, input.cuts);
        return built;
    }

   private:
    struct Axis {
        TreeVec start, dir;
        float length = 1;
        float r0 = 0.1f, r1 = 0.02f;
        uint8_t order = 0;
        float birth0 = 0, done = 1, mature = 1;
        int32_t parent = -1;
        float attachT = 0;
        float tropism = 0;  // > 0 bends toward the sky, < 0 droops
        float gnarl = 0;
        int steps = 4;
        uint8_t flags = 0;
        uint16_t cut = 0;
    };
    // A grown axis: its segments and the fractions where they start.
    struct Chain {
        std::vector<int32_t> segments;
        Axis axis;
    };
    struct Point {
        int32_t segment = -1;
        float t = 0;
        TreeVec position, direction;
        float birth = 0, radius = 0;
    };

    const Tree& tree;
    const TreeFormParams& p;
    const TreeSkeletonInput& input;
    Rng rng;
    TreeSkeleton skeleton;
    float clumpSize = 1;
    float top = 1;          // target height of the axes' tips
    float crownBase = 0;    // height where branching starts
    TreeVec pivot;          // top of the bole (crown centre axis)
    TreeVec lean{0, 1, 0};  // trunk direction
    float windAzimuth = 0;  // stunted trees grow away from it
    float maturity = 1;

    float timeAt(const Axis& a, float u) const {
        return a.birth0 + (a.done - a.birth0) * skeleton.growthTime(u);
    }

    Chain grow(const Axis& a) {
        Chain chain{{}, a};
        const int steps = std::max(1, a.steps);
        TreeVec position = a.start, dir = a.dir.normalized();
        int32_t parent = a.parent;
        float attach = a.attachT;
        for (int k = 0; k < steps; ++k) {
            const float u0 = float(k) / steps, u1 = float(k + 1) / steps;
            const float step = a.length / steps;
            TreeSegment seg;
            seg.a = position;
            seg.b = position + dir * step;
            seg.r0 = std::lerp(a.r0, a.r1, u0);
            seg.r1 = std::lerp(a.r0, a.r1, u1);
            seg.birthA = timeAt(a, u0);
            seg.birthB = timeAt(a, u1);
            seg.matureAge = a.mature;
            seg.parent = parent;
            seg.attachT = attach;
            seg.order = a.order;
            seg.flags = a.flags;
            seg.cut = a.cut;
            parent = int32_t(skeleton.segments.size());
            attach = 1;
            chain.segments.push_back(parent);
            skeleton.segments.push_back(seg);
            position = seg.b;
            // Bend: random wander, then toward the sky or down.
            const TreeVec wander{rng.jitter(1), rng.jitter(1), rng.jitter(1)};
            dir = (dir + wander * (a.gnarl * 0.35f) + Up * a.tropism)
                      .normalized();
        }
        return chain;
    }

    Point at(const Chain& chain, float u) const {
        const int n = int(chain.segments.size());
        const float scaled = std::clamp(u, 0.f, 1.f) * n;
        const int k = std::min(n - 1, int(scaled));
        const float t = scaled - k;
        const auto& seg = skeleton.segments[size_t(chain.segments[k])];
        return {chain.segments[k],
                t,
                lerp(seg.a, seg.b, t),
                (seg.b - seg.a).normalized(),
                std::lerp(seg.birthA, seg.birthB, t),
                std::lerp(seg.r0, seg.r1, t)};
    }

    void addFoliage(const Chain& chain, float u0, float u1,
                    float death = INFINITY, float sizeScale = 1) {
        if (tree.foliage.density <= 0) return;
        const float length = chain.axis.length * (u1 - u0);
        const int count =
            std::max(1, int(std::round(length * tree.foliage.density)));
        for (int i = 0; i < count; ++i) {
            const float u =
                u0 + (u1 - u0) * (i + rng.range(0.2f, 0.8f)) / count;
            const auto point = at(chain, u);
            TreeFoliageSite site;
            site.position = point.position;
            site.direction = point.direction;
            site.size = clumpSize * rng.range(0.8f, 1.15f) * sizeScale;
            // Leaves open a little after the twig reaches them.
            site.birth = point.birth + maturity * 0.01f;
            site.death = death;
            site.segment = point.segment;
            skeleton.foliage.push_back(site);
        }
    }

    // Leaves along a terminal twig and a larger clump at its tip, so crowns
    // read as full volumes rather than strings of clumps.
    void tipFoliage(const Chain& chain) {
        addFoliage(chain, 0.15f, 1);
        if (tree.foliage.density <= 0) return;
        const auto end = at(chain, 1);
        TreeFoliageSite site;
        site.position = end.position;
        site.direction = end.direction;
        site.size = clumpSize * rng.range(1.3f, 1.6f);
        site.birth = end.birth + maturity * 0.01f;
        site.segment = end.segment;
        skeleton.foliage.push_back(site);
    }

    // Recursive forking (decurrent crowns, giant limbs, sprouts).
    void forks(const Point& from, float length, float radius, uint8_t order,
               int depth, int depths, float birth, float done, float mature,
               bool stunted, uint8_t flags = 0, uint16_t cut = 0) {
        const int n = std::max(2, rng.sampleInt(p.branches_per_node));
        const float spread = radians(rng.sample(p.branch_angle_deg));
        const float levelTime = (done - birth) / float(depths - depth + 1);
        const float rotation = rng.range(0, 2 * Pi);
        for (int i = 0; i < n; ++i) {
            // The first child continues the axis (apical), the others fan out.
            const bool apical = i == 0;
            float azimuth =
                rotation + float(i) * 2 * Pi / float(n) + rng.jitter(0.4f);
            if (stunted) {
                // Wind-flagged: branches turn away from the wind.
                const float away = windAzimuth + Pi;
                azimuth = std::lerp(azimuth, away, 0.45f);
            }
            const float angle = apical ? spread * 0.35f : spread;
            Axis a;
            a.start = from.position;
            a.dir = around(from.direction, angle, azimuth);
            a.length = length * rng.range(0.8f, 1.2f) * (apical ? 1.05f : 1);
            a.r0 = std::max(0.02f, radius * std::pow(float(n), -0.5f) *
                                       (apical ? 1.15f : 0.9f));
            a.r1 = std::max(0.02f, a.r0 * 0.6f);
            a.order = order;
            a.birth0 = birth;
            a.done = birth + levelTime;
            a.mature = mature;
            a.parent = from.segment;
            a.attachT = from.t;
            a.tropism = (stunted ? 0.05f : 0.07f) - p.droop * 0.12f;
            a.gnarl = p.gnarl;
            a.steps = std::clamp(int(a.length / 1.5f), 2, 8);
            a.flags = flags;
            a.cut = cut;
            const auto chain = grow(a);
            const auto end = at(chain, 1);
            if (depth < depths)
                forks(end, length * 0.72f, a.r1, uint8_t(order + 1), depth + 1,
                      depths, a.done, done, mature, stunted, flags, cut);
            else
                tipFoliage(chain);
            // Twigs with leaves along older limbs too, so crowns are full.
            if (depth >= depths - 1 && depth < depths)
                addFoliage(chain, 0.35f, 1, INFINITY, 0.9f);
        }
    }

    Axis trunk(float startY, float length, float r1) const {
        Axis a;
        a.start = {0, startY, 0};
        a.dir = lean;
        a.length = length;
        a.r0 = skeleton.trunkRadius;
        a.r1 = r1;
        a.order = 0;
        a.birth0 = 0;
        a.done = maturity * ExtendShare;
        a.mature = maturity;
        a.tropism = tree.form == TreeForm::Palm ? 0.06f : 0.03f;
        a.gnarl = p.gnarl * 0.3f;
        a.steps = std::clamp(int(length / 1.5f), 4, 64);
        return a;
    }

    void excurrent() {
        const float h = skeleton.height, sink = 1;
        crownBase = rng.sample(p.crown_base) * h;
        const auto trunkChain =
            grow(trunk(-sink, h + sink,
                       std::max(0.03f, skeleton.trunkRadius * (1 - p.taper))));
        pivot = {0, crownBase, 0};
        const float spacing = std::max(0.2f, rng.sample(p.branch_spacing_m));
        const float spread = rng.sample(p.branch_angle_deg);
        int whorl = 0;
        for (float y = std::max(crownBase, 0.4f); y < h * 0.97f;
             y += spacing * rng.range(0.85f, 1.15f), ++whorl) {
            const float u = (y + sink) / (h + sink);
            const auto base = at(trunkChain, u);
            const int n = std::max(1, rng.sampleInt(p.branches_per_node));
            const float rel =
                std::clamp((h - y) / std::max(0.5f, h - crownBase), 0.f, 1.f);
            for (int j = 0; j < n; ++j) {
                const float azimuth = whorl * GoldenAngle +
                                      float(j) * 2 * Pi / float(n) +
                                      rng.jitter(0.3f);
                Axis a;
                a.start = base.position;
                a.dir = around(Up, radians(spread + rng.jitter(8)), azimuth);
                a.length = skeleton.crownRadius * std::pow(rel, 0.9f) *
                               rng.range(0.85f, 1.15f) +
                           0.3f;
                a.r0 = std::min(base.radius * 0.5f, 0.06f + 0.04f * a.length);
                a.r1 = 0.02f;
                a.order = 1;
                a.birth0 = base.birth;
                a.done = base.birth + (maturity - base.birth) * 0.8f;
                a.mature = maturity;
                a.parent = base.segment;
                a.attachT = base.t;
                a.tropism = -p.droop * 0.15f;
                a.gnarl = p.gnarl;
                a.steps = std::clamp(int(a.length / 1.2f), 2, 8);
                const auto branch = grow(a);
                if (p.branch_orders >= 2) {
                    const TreeVec side = a.dir.cross(Up).normalized();
                    const float subSpacing = spacing * 1.4f;
                    int k = 0;
                    for (float v = 0.2f; v < 0.9f;
                         v += subSpacing / std::max(0.5f, a.length), ++k) {
                        const auto twigBase = at(branch, v);
                        const float sign = k % 2 ? 1.f : -1.f;
                        Axis t;
                        t.start = twigBase.position;
                        t.dir = (twigBase.direction * 0.64f +
                                 side * (0.77f * sign) + Up * -0.1f)
                                    .normalized();
                        t.length = 0.35f * a.length * (1 - v) + 0.2f;
                        t.r0 = std::max(0.02f, twigBase.radius * 0.5f);
                        t.r1 = 0.02f;
                        t.order = 2;
                        t.birth0 = twigBase.birth;
                        t.done =
                            twigBase.birth + (a.done - twigBase.birth) * 0.9f;
                        t.mature = maturity;
                        t.parent = twigBase.segment;
                        t.attachT = twigBase.t;
                        t.tropism = -p.droop * 0.1f;
                        t.gnarl = p.gnarl;
                        t.steps = 2;
                        addFoliage(grow(t), 0.1f, 1);
                    }
                    addFoliage(branch, 0.4f, 1);
                } else {
                    addFoliage(branch, 0.15f, 1);
                }
            }
        }
        // Leader tip, and juvenile foliage before the first whorls form.
        addFoliage(trunkChain, 0.9f, 1);
        if (crownBase > 0.5f) {
            const float death =
                at(trunkChain, (crownBase + sink) / (h + sink)).birth +
                maturity * 0.25f;
            addFoliage(trunkChain, 0.05f, (crownBase + sink) / (h + sink),
                       death);
        }
    }

    void decurrent(bool stunted) {
        const float h = skeleton.height, sink = 1;
        crownBase = std::max(0.3f, rng.sample(p.crown_base) * h);
        // The bole is the lower part of a virtual leader of height h: its tip
        // reaches the crown base when the tree has that fraction of its size.
        Axis bole = trunk(-sink, crownBase + sink,
                          skeleton.trunkRadius * (1 - 0.3f * p.taper));
        bole.done = maturity * ExtendShare *
                    skeleton.growthTime((crownBase + sink) / (h + sink));
        bole.steps = std::clamp(int(bole.length / 1.5f), 2, 32);
        const auto boleChain = grow(bole);
        const auto fork = at(boleChain, 1);
        pivot = fork.position;
        const int depths = p.branch_orders + 1;
        forks(fork, (h - crownBase) * 0.42f, bole.r1, 1, 1, depths, bole.done,
              maturity * ExtendShare, maturity, stunted);
        // Saplings carry leaves along the stem until the crown forms.
        addFoliage(boleChain, 0.2f, 1, bole.done + maturity * 0.2f);
    }

    void palm() {
        const float h = skeleton.height, sink = 0.5f;
        crownBase = h;
        const auto stem = grow(trunk(
            -sink, h + sink,
            std::max(0.05f, skeleton.trunkRadius * (1 - 0.5f * p.taper))));
        pivot = at(stem, 1).position;
        palmCrown(stem, 0.05f, 10);
    }

    // Rings of fronds along a stem; each ring lives while the growing tip is
    // within two rings of it, so the crown rises with the stem.
    void palmCrown(const Chain& stem, float u0, int rings) {
        const int n = std::max(3, rng.sampleInt(p.branches_per_node));
        std::vector<Point> points;
        for (int k = 0; k < rings; ++k)
            points.push_back(
                at(stem, u0 + (1 - u0) * float(k) / float(rings - 1)));
        for (int k = 0; k < rings; ++k) {
            // Only the top ring lives: a ring is shed once the stem has
            // grown past the next one (older rings would stick out of the
            // stem's side once it is block wood).
            const float death =
                k + 1 < rings ? points[size_t(k + 1)].birth : INFINITY;
            const float rotation = k * GoldenAngle;
            for (int i = 0; i < n; ++i) {
                TreeFoliageSite site;
                site.direction = around(
                    Up, radians(rng.sample(p.branch_angle_deg)),
                    rotation + float(i) * 2 * Pi / float(n) + rng.jitter(0.2f));
                site.position = points[size_t(k)].position + Up * 0.2f;
                site.size = clumpSize * rng.range(0.85f, 1.1f);
                site.birth = points[size_t(k)].birth;
                site.death = death;
                site.segment = points[size_t(k)].segment;
                skeleton.foliage.push_back(site);
            }
        }
    }

    void giant() {
        auto& s = skeleton;
        const float h = s.height, sink = 1.5f;
        crownBase = std::max(2.f, rng.sample(p.bole_height) * h);
        s.flare = {p.root_flare, s.trunkRadius * 0.8f + 2,
                   5 + int(rng.next() % 4), rng.range(0, 2 * Pi)};
        Axis bole = trunk(-sink, crownBase + sink,
                          s.trunkRadius * (1 - 0.35f * p.taper));
        bole.done = maturity * ExtendShare *
                    s.growthTime((crownBase + sink) / (h + sink));
        bole.steps = std::clamp(int(bole.length / 3), 3, 24);
        bole.gnarl = p.gnarl * 0.1f;
        const auto boleChain = grow(bole);
        pivot = at(boleChain, 1).position;
        const int limbs = std::max(2, rng.sampleInt(p.limbs));
        const float spacing = rng.sample(p.branch_spacing_m);
        const float spread = rng.sample(p.branch_angle_deg);
        const int depths = p.branch_orders + 1;
        const float rotation = rng.range(0, 2 * Pi);
        for (int i = 0; i < limbs; ++i) {
            // Tiers: limbs leave the bole at a few heights.
            const float y = std::max(crownBase * 0.6f,
                                     crownBase - float(i % 3) * spacing * 0.5f);
            const auto base = at(boleChain, (y + sink) / (crownBase + sink));
            Axis a;
            a.start = base.position;
            a.dir = around(Up, radians(spread + rng.jitter(6)),
                           rotation + float(i) * 2 * Pi / float(limbs) +
                               rng.jitter(0.25f));
            a.length = s.crownRadius * 0.55f * rng.range(0.85f, 1.1f);
            a.r0 = std::min(base.radius * 0.8f,
                            s.trunkRadius * 0.9f / std::sqrt(float(limbs)));
            a.r1 = a.r0 * 0.45f;
            a.order = 1;
            a.birth0 = base.birth;
            a.done = base.birth + (maturity * ExtendShare - base.birth) * 0.45f;
            a.mature = maturity;
            a.parent = base.segment;
            a.attachT = base.t;
            a.tropism = 0.04f - p.droop * 0.1f;
            a.gnarl = p.gnarl * 0.5f;
            a.steps = std::clamp(int(a.length / 4), 3, 16);
            const auto limb = grow(a);
            forks(at(limb, 1), s.crownRadius * 0.3f, a.r1, 2, 2, depths, a.done,
                  maturity * ExtendShare, maturity, false);
        }
        // A central leader rising from the bole to the top.
        Axis leader;
        leader.start = pivot;
        leader.dir = Up;
        leader.length = std::max(1.f, (h - crownBase) * 0.6f);
        leader.r0 = s.trunkRadius * 0.35f;
        leader.r1 = leader.r0 * 0.4f;
        leader.order = 1;
        leader.birth0 = bole.done;
        leader.done = bole.done + (maturity * ExtendShare - bole.done) * 0.5f;
        leader.mature = maturity;
        leader.parent = boleChain.segments.back();
        leader.attachT = 1;
        leader.tropism = 0.05f;
        leader.gnarl = p.gnarl * 0.3f;
        leader.steps = std::clamp(int(leader.length / 4), 3, 16);
        const auto leaderChain = grow(leader);
        forks(at(leaderChain, 1), s.crownRadius * 0.25f, leader.r1, 2, 2,
              depths, leader.done, maturity * ExtendShare, maturity, false);
        addFoliage(boleChain, 0.3f, 1, bole.done + maturity * 0.1f);
    }

    // Scales the crown (everything above the crown base) so its tips reach
    // the sampled crown radius and height, and stay below the headroom.
    void fit(float headroom) {
        auto& s = skeleton;
        if (s.segments.empty()) return;
        // Branches and foliage belong to the crown wherever they hang (a
        // drooping branch may dip below the crown base); the trunk only
        // above the crown base.
        float reach = 0, high = crownBase;
        const auto measure = [&](TreeVec v, bool crown) {
            if (!crown && v.y <= crownBase) return;
            const float dx = v.x - pivot.x, dz = v.z - pivot.z;
            reach = std::max(reach, std::sqrt(dx * dx + dz * dz));
            if (v.y > crownBase) high = std::max(high, v.y);
        };
        for (const auto& seg : s.segments) measure(seg.b, seg.order > 0);
        float foliagePad = 0;
        for (const auto& f : s.foliage) {
            const float pad =
                f.size *
                (tree.foliage.type == TreeFoliageType::Frond ? 0.3f : 0.55f);
            foliagePad = std::max(foliagePad, pad);
            measure(f.position, true);
        }
        const float targetTop = std::min(s.height, headroom - foliagePad);
        const float sy =
            high > crownBase + 0.01f
                ? std::clamp((targetTop - crownBase) / (high - crownBase),
                             0.05f, 4.f)
                : 1;
        const float targetReach = std::max(0.3f, s.crownRadius - foliagePad);
        const float sx =
            reach > 0.1f ? std::clamp(targetReach / reach, 0.05f, 3.f) : 1;
        const float blend = std::max(1.f, 0.15f * (top - crownBase));
        // Branch points take the full horizontal scale (they start on the
        // trunk's axis, which stays put); the trunk blends into it above
        // the crown base.
        const auto apply = [&](TreeVec& v, bool crown) {
            float w = std::clamp((v.y - crownBase) / blend, 0.f, 1.f);
            w = crown ? 1.f : w * w * (3 - 2 * w);
            const float k = std::lerp(1.f, sx, w);
            v.x = pivot.x + (v.x - pivot.x) * k;
            v.z = pivot.z + (v.z - pivot.z) * k;
            if (v.y > crownBase) v.y = crownBase + (v.y - crownBase) * sy;
        };
        for (auto& seg : s.segments) {
            apply(seg.a, seg.order > 0);
            apply(seg.b, seg.order > 0);
        }
        for (auto& f : s.foliage) apply(f.position, true);
    }

    // Nothing rises past the headroom: wood (with its radius) or foliage.
    // Runs after cuts too, since sprouts are grown after the fit.
    void clampToHeadroom(float headroom) {
        auto& s = skeleton;
        for (auto& seg : s.segments) {
            seg.a.y = std::min(seg.a.y, headroom - std::max(seg.r0, 0.05f));
            seg.b.y = std::min(seg.b.y, headroom - std::max(seg.r1, 0.05f));
        }
        for (auto& f : s.foliage) {
            const float pad =
                f.size *
                (tree.foliage.type == TreeFoliageType::Frond ? 0.3f : 0.55f);
            f.position.y = std::min(f.position.y, headroom - pad);
        }
    }

    void pruneByGround() {
        auto& s = skeleton;
        if (!input.ground) return;
        for (auto& seg : s.segments) {
            const bool parentGone =
                seg.parent >= 0 && !s.segments[size_t(seg.parent)].live();
            const bool buried = seg.order > 0 &&
                                seg.b.y < input.ground(seg.b.x, seg.b.z) - 0.3f;
            if (parentGone || buried) seg.flags |= TreeSegment::Pruned;
        }
        for (auto& f : s.foliage)
            if (f.segment >= 0 && !s.segments[size_t(f.segment)].live())
                f.pruned = true;
            else if (f.position.y <
                     input.ground(f.position.x, f.position.z) + 0.2f)
                f.pruned = true;
    }

    void applyCuts(const std::vector<TreeCut>& cuts, size_t first) {
        auto& s = skeleton;
        for (size_t i = first; i < cuts.size(); ++i) {
            const auto& cut = cuts[i];
            if (cut.segment < 0 || size_t(cut.segment) >= s.segments.size())
                continue;
            if (!s.segments[size_t(cut.segment)].live()) continue;
            const float t = std::clamp(cut.t, 0.f, 1.f);
            const uint16_t tag = uint16_t(i + 1);
            const size_t index = size_t(cut.segment);
            // Remaining length of the axis above the cut, for the sprout.
            std::vector<std::vector<int32_t>> children(s.segments.size());
            for (size_t k = 0; k < s.segments.size(); ++k)
                if (s.segments[k].parent >= 0)
                    children[size_t(s.segments[k].parent)].push_back(
                        int32_t(k));
            float remaining =
                (s.segments[index].b - s.segments[index].a).length() * (1 - t);
            for (int32_t k = int32_t(index);;) {
                int32_t next = -1;
                for (const int32_t c : children[size_t(k)])
                    if (s.segments[size_t(c)].attachT >= 1 &&
                        s.segments[size_t(c)].order ==
                            s.segments[index].order &&
                        s.segments[size_t(c)].live())
                        next = c;
                if (next < 0) break;
                remaining +=
                    (s.segments[size_t(next)].b - s.segments[size_t(next)].a)
                        .length();
                k = next;
            }
            // Prune everything attached above the cut.
            std::vector<int32_t> stack;
            for (const int32_t c : children[index])
                if (s.segments[size_t(c)].attachT > t) stack.push_back(c);
            while (!stack.empty()) {
                const int32_t k = stack.back();
                stack.pop_back();
                auto& seg = s.segments[size_t(k)];
                if (!seg.live()) continue;
                seg.flags |= TreeSegment::Pruned;
                seg.cut = tag;
                for (const int32_t c : children[size_t(k)]) stack.push_back(c);
            }
            // Split the cut segment: the part above becomes a pruned piece.
            TreeSegment above = s.segments[index];
            auto& below = s.segments[index];
            const TreeVec cutPoint = lerp(below.a, below.b, t);
            const float r = std::lerp(below.r0, below.r1, t);
            const float birth = std::lerp(below.birthA, below.birthB, t);
            const TreeVec axis = below.b - below.a;
            above.a = cutPoint;
            above.r0 = r;
            above.birthA = birth;
            above.parent = int32_t(index);
            above.attachT = 1;
            above.flags |= TreeSegment::Pruned;
            above.cut = tag;
            below.b = cutPoint;
            below.r1 = r;
            below.birthB = birth;
            below.flags |= TreeSegment::CutEnd;
            for (auto& f : s.foliage) {
                if (f.pruned || f.segment < 0) continue;
                if (!s.segments[size_t(f.segment)].live()) {
                    f.pruned = true;
                } else if (size_t(f.segment) == index &&
                           axis.dot(axis) > 1e-6f &&
                           (f.position - cutPoint).dot(axis) > 0) {
                    f.pruned = true;
                }
            }
            const uint8_t order = below.order;
            s.segments.push_back(above);  // invalidates `below`

            const bool root =
                order == 0 &&
                cutPoint.y < std::max(1.5f, s.flare.height * 0.5f);
            if (root && !tree.cut.stump_resprouts) {
                s.dead = true;
                continue;
            }
            // A cut trunk regrows to the tree's height; a cut branch to its
            // former length.
            const float length =
                order == 0 ? std::max(remaining, s.height - cutPoint.y)
                           : remaining;
            sprout(int32_t(index), cutPoint, axis.normalized(), r, order,
                   std::max(1.f, length), cut.age, tag);
        }
    }

    // Regrowth from a cut: a new leader of the remaining length, forking
    // like a broadleaf crown (palms grow a new crown of fronds).
    void sprout(int32_t from, TreeVec origin, TreeVec dir, float radius,
                uint8_t order, float length, float age, uint16_t tag) {
        auto& s = skeleton;
        Rng local(skeleton.seed ^ (uint64_t(tag) * 0x9E3779B97F4A7C15ull));
        std::swap(rng, local);
        const float start = age + rng.sample(tree.cut.resprout_days);
        const float span =
            maturity * std::clamp(length / std::max(1.f, s.height), 0.2f, 1.f);
        Axis a;
        a.start = origin;
        a.dir = (order == 0 ? (dir + Up).normalized() : dir);
        a.length = length;
        a.r0 = std::max(0.03f, radius * 0.7f);
        a.r1 = std::max(0.02f, a.r0 * (1 - p.taper));
        a.order = order;
        a.birth0 = start;
        a.done = start + span * ExtendShare;
        a.mature = start + span;
        a.parent = from;
        a.attachT = 1;
        a.tropism = 0.05f;
        a.gnarl = p.gnarl;
        a.steps = std::clamp(int(length / 1.5f), 2, 32);
        a.flags = TreeSegment::Sprout;
        a.cut = tag;
        const auto chain = grow(a);
        if (tree.form == TreeForm::Palm) {
            palmCrown(chain, 0.3f, 4);
        } else if (order < p.branch_orders) {
            forks(at(chain, 0.7f), length * 0.4f, a.r1, uint8_t(order + 1),
                  order + 2, p.branch_orders + 1,
                  std::lerp(a.birth0, a.done, 0.7f), a.done, a.mature,
                  tree.form == TreeForm::Stunted, TreeSegment::Sprout, tag);
            addFoliage(chain, 0.5f, 1);
        } else {
            addFoliage(chain, 0.2f, 1);
        }
        std::swap(rng, local);
    }

    TreeSkeletonPtr finish() {
        auto& s = skeleton;
        TreeVec lo{INFINITY, INFINITY, INFINITY},
            hi{-INFINITY, -INFINITY, -INFINITY};
        const auto grow = [&](TreeVec v, float pad) {
            lo = {std::min(lo.x, v.x - pad), std::min(lo.y, v.y - pad),
                  std::min(lo.z, v.z - pad)};
            hi = {std::max(hi.x, v.x + pad), std::max(hi.y, v.y + pad),
                  std::max(hi.z, v.z + pad)};
        };
        for (const auto& seg : s.segments) {
            if (!seg.live()) continue;
            const float flare = seg.order == 0 ? 1 + s.flare.amount : 1;
            grow(seg.a, seg.r0 * flare);
            grow(seg.b, seg.r1 * flare);
        }
        for (const auto& f : s.foliage)
            if (!f.pruned) grow(f.position, f.size * 0.6f);
        if (lo.x > hi.x) lo = hi = {};
        s.boundsMin = lo;
        s.boundsMax = hi;
        return std::make_shared<const TreeSkeleton>(std::move(skeleton));
    }
};
}  // namespace

float TreeSkeleton::fullRadius(const TreeSegment& s, float t,
                               float angle) const {
    float r = std::lerp(s.r0, s.r1, t);
    if (s.order == 0 && !(s.flags & TreeSegment::Sprout) && flare.amount > 0) {
        const float y = std::max(0.f, std::lerp(s.a.y, s.b.y, t));
        float extra =
            flare.amount * std::exp(-y / std::max(0.1f, flare.height));
        if (flare.lobes > 0)
            extra *= 0.55f +
                     0.45f * std::cos(float(flare.lobes) * angle + flare.phase);
        r *= 1 + extra;
    }
    return r;
}

float TreeSkeleton::radius(const TreeSegment& s, float t, float age,
                           float angle) const {
    const float born = std::lerp(s.birthA, s.birthB, t);
    if (age <= born) return 0;
    const float span = std::max(1e-3f, s.matureAge - born);
    return fullRadius(s, t, angle) * growth((age - born) / span);
}

float TreeSkeleton::matureAge() const {
    float age = 0;
    for (const auto& s : segments)
        if (s.live()) age = std::max(age, s.matureAge);
    return age;
}

TreeSkeletonPtr buildTreeSkeleton(const Tree& species,
                                  const TreeSkeletonInput& input) {
    return Builder(species, input).run();
}

TreeSkeletonPtr applyTreeCuts(const TreeSkeleton& skeleton, const Tree& species,
                              const std::vector<TreeCut>& cuts, size_t first) {
    if (first >= cuts.size()) return std::make_shared<TreeSkeleton>(skeleton);
    TreeSkeletonInput input;
    input.seed = skeleton.seed;
    input.headroom = skeleton.headroom;
    return Builder(species, input, skeleton).cut(cuts, first);
}

TreeSkeletonPtr rotateTreeSkeleton(const TreeSkeleton& skeleton,
                                   int quarterTurns) {
    const int k = ((quarterTurns % 4) + 4) % 4;
    auto out = std::make_shared<TreeSkeleton>(skeleton);
    if (k == 0) return out;
    // Exact for quarter turns: c and s are 0 or +-1.
    const float c = k == 2 ? -1.f : k == 0 ? 1.f : 0.f;
    const float s = k == 1 ? 1.f : k == 3 ? -1.f : 0.f;
    const auto turn = [&](TreeVec v) {
        return TreeVec{c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
    };
    for (auto& seg : out->segments) {
        seg.a = turn(seg.a);
        seg.b = turn(seg.b);
    }
    for (auto& f : out->foliage) {
        f.position = turn(f.position);
        f.direction = turn(f.direction);
    }
    // Buttress lobes turn with the trunk: cos(lobes * angle + phase) at the
    // turned angle (atan2(z, x) decreases by k quarter turns).
    out->flare.phase += float(out->flare.lobes) * float(k) * Pi * 0.5f;
    const TreeVec a = turn(out->boundsMin), b = turn(out->boundsMax);
    out->boundsMin = {std::min(a.x, b.x), a.y, std::min(a.z, b.z)};
    out->boundsMax = {std::max(a.x, b.x), b.y, std::max(a.z, b.z)};
    return out;
}
