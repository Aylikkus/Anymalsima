#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "flora.hpp"
#include "flora_json.hpp"

// A tree species (docs/handoff/trees.md, docs/modding/trees.md). A tree is
// a procedural skeleton grown from a code archetype ("form") tuned by these
// parameters. Parts thicker than TreeVoxelRadius become wood blocks; thinner
// branches and the foliage are meshes. Every numeric field accepts a number
// or a [min, max] range sampled per tree.

enum class TreeForm : uint8_t {
    Excurrent,  // a single leader with whorls of branches (spruce)
    Decurrent,  // the trunk forks into a broad crown (oak)
    Palm,       // a curved unbranched trunk with a crown of fronds
    Stunted,    // wind-bent and thin (mountain pine, krummholz)
    Giant,      // buttressed bole splitting into huge limbs (landmarks)
};

enum class TreeFoliageType : uint8_t { NeedleClump, LeafClump, Frond };

// Classes by mature trunk radius, in blocks: thin trees never form blocks,
// massive ones span several chunks and may cross region borders.
enum class TreeSizeClass : uint8_t { Thin, Small, Medium, Large, Massive };

// Parts of at least this radius (m) become wood blocks; 0.5 gives the
// single column of a small trunk.
inline constexpr float TreeVoxelRadius = 0.5f;
inline constexpr float TreeMaxHeight = 512;
inline constexpr float TreeMaxTrunkRadius = 64;
// How far an ordinary tree (crown and spacing) may reach from its anchor:
// generation reads terrain only within one region of the chunk. Massive
// trees are placed from a world-wide landmark list instead.
inline constexpr float TreeMaxReach = 96;
// Massive trees: chunks look for them this far (in regions) around, so the
// cost of a larger reach is more region lookups, not a hard limit
// (ChunkManager::landmarkRadius).
inline constexpr float TreeMaxLandmarkReach = 1024;

inline TreeSizeClass treeSizeClass(float trunkRadius) {
    if (trunkRadius < TreeVoxelRadius) return TreeSizeClass::Thin;
    if (trunkRadius < 1.5f) return TreeSizeClass::Small;
    if (trunkRadius < 4.5f) return TreeSizeClass::Medium;
    if (trunkRadius < 15.5f) return TreeSizeClass::Large;
    return TreeSizeClass::Massive;
}

struct TreeSize {
    ValueRange height_m{10};
    ValueRange trunk_radius_m{0.5f};
    ValueRange crown_radius_m{3};
};

struct TreeGrowthRule {
    // Days from germination to full size, then the whole life.
    ValueRange maturity_days{365};
    ValueRange lifespan_days{36500};
    // Shape of growth: size fraction 1 - (1 - age/maturity)^curve, so larger
    // values grow fast while young and slow down toward maturity.
    float curve = 2;
};

struct TreeFormParams {
    // Whorl spacing (excurrent), fork interval (decurrent), limb tiers
    // (giant), in metres along the parent.
    ValueRange branch_spacing_m{1};
    ValueRange branch_angle_deg{60};  // from the parent axis
    ValueRange branches_per_node{3};
    ValueRange crown_base{0.3f};  // fraction of height free of branches
    ValueRange lean_deg{0};
    float droop = 0;        // [-1, 1]: tips bend down (positive) or up
    float taper = 0.85f;    // [0, 1]: 1 thins the trunk to a point
    float gnarl = 0.1f;     // [0, 1]: random bending
    float root_flare = 0;   // [0, 4]: extra base radius, times the trunk's
    int branch_orders = 2;  // [1, 4]: branches of branches
    ValueRange limbs{0};    // giant: main limbs splitting from the bole
    ValueRange bole_height{0.3f};  // giant: where limbs split (fraction)
};

struct TreeWood {
    std::string block;  // a Block with natural_growth (living wood)
    std::string bark;   // FoliageMaterial for branches thinner than blocks
};

struct TreeRoots {
    float depth_m = 0;
    float reach_m = 0;
    int branches = 0;
};

struct TreeFoliage {
    TreeFoliageType type = TreeFoliageType::LeafClump;
    std::string material;  // FoliageMaterial
    ValueRange clump_size_m{1};
    float density = 1;  // [0, 4]: clumps per metre of foliage-bearing twig
};

// What the part above a trunk cut does.
enum class TreeFell : uint8_t {
    // Falls (for now: disappears; later a physics hazard that can crush
    // what it lands on). Thin trees and redwoods.
    Fall,
    // Settles onto the stump and closes the gap (massive trunks too heavy
    // to topple): the tree stays whole, one tile shorter.
    Settle,
};

struct TreeCutRule {
    // A cut above the root always regrows from the cut; a cut at the root
    // regrows only when the stump resprouts, otherwise the tree dies and
    // the stump stays.
    bool stump_resprouts = false;
    ValueRange resprout_days{30};
    // Trunk cuts (branches always fall).
    TreeFell fell = TreeFell::Fall;
};

struct Tree {
    static constexpr const std::string TypeName = "trees";
    static inline uint8_t counter = 0;

    uint8_t id = 0;
    std::string name;
    TreeForm form = TreeForm::Decurrent;
    TreeSize size;
    TreeGrowthRule growth;
    TreeFormParams form_params;
    TreeWood wood;
    TreeRoots roots;
    TreeFoliage foliage;
    FloraRules placement;
    ValueRange spacing_m{4};  // minimum distance to other trees
    TreeCutRule cut;

    TreeSizeClass sizeClass() const {
        return treeSizeClass(size.trunk_radius_m.max);
    }
    bool landmark() const { return sizeClass() == TreeSizeClass::Massive; }
    bool immortal() const { return landmark(); }
    // Widest horizontal extent from the anchor: crown or flared roots.
    float extent() const {
        return std::max(size.crown_radius_m.max,
                        size.trunk_radius_m.max * (1 + form_params.root_flare));
    }
    float reach() const {
        return std::max(extent(), roots.reach_m) + spacing_m.max;
    }
};

inline void from_json(const Json& j, Tree& t) {
    using namespace flora_json;
    known(j, "tree",
          {"name", "form", "size", "growth", "form_params", "wood", "roots",
           "foliage", "placement", "cut"});
    t.name = name(j, "name");
    try {
        if (!j.contains("form")) fail("form", "missing tree form");
        t.form = keyword(j.at("form"), "form",
                         std::array<std::pair<std::string_view, TreeForm>, 5>{
                             {{"excurrent", TreeForm::Excurrent},
                              {"decurrent", TreeForm::Decurrent},
                              {"palm", TreeForm::Palm},
                              {"stunted", TreeForm::Stunted},
                              {"giant", TreeForm::Giant}}});

        if (!j.contains("size")) fail("size", "missing size");
        {
            const auto& s = j.at("size");
            known(s, "size", {"height_m", "trunk_radius_m", "crown_radius_m"});
            for (const auto* key : {"height_m", "trunk_radius_m"})
                if (!s.contains(key)) fail(key, "missing");
            range(s, "height_m", t.size.height_m, 1, TreeMaxHeight);
            range(s, "trunk_radius_m", t.size.trunk_radius_m, 0.02f,
                  TreeMaxTrunkRadius);
            range(s, "crown_radius_m", t.size.crown_radius_m, 0.2f,
                  TreeMaxLandmarkReach);
        }

        if (j.contains("growth")) {
            const auto& g = j.at("growth");
            known(g, "growth", {"maturity_days", "lifespan_days", "curve"});
            range(g, "maturity_days", t.growth.maturity_days, 1, 1000000);
            range(g, "lifespan_days", t.growth.lifespan_days, 1, 10000000);
            t.growth.curve = number(g, "curve", 2, 0.2f, 8);
        }
        if (t.growth.lifespan_days.min < t.growth.maturity_days.max)
            fail("lifespan_days", "a tree must live at least to maturity");

        if (j.contains("form_params")) {
            const auto& f = j.at("form_params");
            known(f, "form_params",
                  {"branch_spacing_m", "branch_angle_deg", "branches_per_node",
                   "crown_base", "lean_deg", "droop", "taper", "gnarl",
                   "root_flare", "branch_orders", "limbs", "bole_height"});
            auto& p = t.form_params;
            range(f, "branch_spacing_m", p.branch_spacing_m, 0.1f, 64);
            range(f, "branch_angle_deg", p.branch_angle_deg, 0, 180);
            range(f, "branches_per_node", p.branches_per_node, 0, 16);
            range(f, "crown_base", p.crown_base, 0, 0.95f);
            range(f, "lean_deg", p.lean_deg, 0, 60);
            p.droop = number(f, "droop", 0, -1, 1);
            p.taper = number(f, "taper", 0.85f, 0, 1);
            p.gnarl = number(f, "gnarl", 0.1f, 0, 1);
            p.root_flare = number(f, "root_flare", 0, 0, 4);
            if (f.contains("branch_orders")) {
                const auto& o = f.at("branch_orders");
                if (!o.is_number_integer() || o.get<int64_t>() < 1 ||
                    o.get<int64_t>() > 4)
                    fail("branch_orders", "expected an integer in [1, 4]");
                p.branch_orders = o.get<int>();
            }
            range(f, "limbs", p.limbs, 0, 16);
            range(f, "bole_height", p.bole_height, 0.05f, 0.9f);
        }

        if (!j.contains("wood")) fail("wood", "missing wood");
        {
            const auto& w = j.at("wood");
            known(w, "wood", {"block", "bark"});
            t.wood.block = name(w, "block");
            t.wood.bark = name(w, "bark");
        }

        if (!j.contains("foliage")) fail("foliage", "missing foliage");
        {
            const auto& f = j.at("foliage");
            known(f, "foliage",
                  {"type", "material", "clump_size_m", "density"});
            if (!f.contains("type")) fail("type", "missing foliage type");
            t.foliage.type = keyword(
                f.at("type"), "type",
                std::array<std::pair<std::string_view, TreeFoliageType>, 3>{
                    {{"needle_clump", TreeFoliageType::NeedleClump},
                     {"leaf_clump", TreeFoliageType::LeafClump},
                     {"frond", TreeFoliageType::Frond}}});
            t.foliage.material = name(f, "material");
            range(f, "clump_size_m", t.foliage.clump_size_m, 0.05f, 16);
            t.foliage.density = number(f, "density", 1, 0, 4);
        }

        if (j.contains("roots")) {
            const auto& r = j.at("roots");
            known(r, "roots", {"depth_m", "reach_m", "branches"});
            if (t.size.trunk_radius_m.max < TreeVoxelRadius)
                fail("roots", "roots require a voxel trunk");
            t.roots.depth_m = number(r, "depth_m", 4, 2, 64);
            t.roots.reach_m = number(r, "reach_m", 8, 1, 256);
            if (r.contains("branches")) {
                const auto& n = r.at("branches");
                if (!n.is_number_integer() || n.get<int64_t>() < 1 ||
                    n.get<int64_t>() > 12)
                    fail("branches", "expected an integer in [1, 12]");
                t.roots.branches = n.get<int>();
            } else {
                t.roots.branches = 4;
            }
        }

        if (!j.contains("placement")) fail("placement", "missing placement");
        {
            // Flora placement plus the trees' own spacing.
            Json p = j.at("placement");
            if (p.is_object() && p.contains("spacing_m")) {
                t.spacing_m = range(p.at("spacing_m"), "spacing_m", 0.5f, 4096);
                p.erase("spacing_m");
            }
            placement(p, t.placement);
            if (!t.placement.companions.empty())
                fail("companions", "trees do not have companions yet");
        }

        if (j.contains("cut")) {
            const auto& c = j.at("cut");
            known(c, "cut", {"stump_resprouts", "resprout_days", "fell"});
            t.cut.stump_resprouts = boolean(c, "stump_resprouts", false);
            range(c, "resprout_days", t.cut.resprout_days, 0, 100000);
            if (c.contains("fell"))
                t.cut.fell = keyword(
                    c.at("fell"), "cut.fell",
                    std::array<std::pair<std::string_view, TreeFell>, 2>{
                        {{"fall", TreeFell::Fall},
                         {"settle", TreeFell::Settle}}});
        }

        if (t.landmark()) {
            if (t.extent() > TreeMaxLandmarkReach)
                fail("size", "a massive tree reaches at most " +
                                 std::to_string(int(TreeMaxLandmarkReach)) +
                                 " m from its anchor");
        } else if (t.reach() > TreeMaxReach) {
            fail("size", "crown plus spacing reach at most " +
                             std::to_string(int(TreeMaxReach)) +
                             " m (only massive trees reach further)");
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(t.name + ": " + error.what());
    }
}
