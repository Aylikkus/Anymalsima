#include "tree_simulation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
// Segments up to this radius are cut by removing any of their wood.
constexpr float ThinCut = 2;
// The growth sweep rebuilds its chunk list this often (updates).
constexpr size_t SweepListInterval = 600;

// Limits of the connectivity test: the window along the limb (times its
// radius, plus tiles) and the tiles it may visit.
constexpr float SlitWindowRadii = 3;
constexpr float SlitWindowExtra = 4;
constexpr size_t SlitMaxVisits = 400000;

// Eigenvector of the smallest eigenvalue of a symmetric 3x3 matrix (Jacobi).
TreeVec smallestEigenvector(std::array<std::array<double, 3>, 3> m) {
    std::array<std::array<double, 3>, 3> v{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    for (int sweep = 0; sweep < 32; ++sweep) {
        double off = 0;
        for (int p = 0; p < 3; ++p)
            for (int q = p + 1; q < 3; ++q) off += m[p][q] * m[p][q];
        if (off < 1e-18) break;
        for (int p = 0; p < 3; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(m[p][q]) < 1e-15) continue;
                const double theta = (m[q][q] - m[p][p]) / (2 * m[p][q]);
                const double t =
                    (theta >= 0 ? 1 : -1) /
                    (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double mkp = m[k][p], mkq = m[k][q];
                    m[k][p] = c * mkp - s * mkq;
                    m[k][q] = s * mkp + c * mkq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double mpk = m[p][k], mqk = m[q][k];
                    m[p][k] = c * mpk - s * mqk;
                    m[q][k] = s * mpk + c * mqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (m[i][i] < m[best][best]) best = i;
    return TreeVec{float(v[0][best]), float(v[1][best]), float(v[2][best])}
        .normalized();
}

// Milliseconds since `start`, restarting it.
double lap(std::chrono::steady_clock::time_point& start) {
    const auto now = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(now - start).count();
    start = now;
    return ms;
}

}  // namespace

TreeSimulation::TreeSimulation(ChunkManager& chunks, WorldEditor& editor)
    : chunks(chunks), editor(editor) {
    connection = editor.blockChanged.connect(
        [this](const BlockChange& change) { blockChanged(change); });
}

TreeSimulation::~TreeSimulation() {
    for (auto& job : clearJobs) job.delta.wait();
    editor.blockChanged.disconnect(connection);
}

void TreeSimulation::applyChanges(const ChunkCoord& coord,
                                  const std::vector<TreeWoodChange>& changes) {
    if (changes.empty()) return;
    std::vector<EditRequest> requests;
    requests.reserve(changes.size());
    for (const auto& c : changes) {
        const int lx = c.index % Chunk::SIZE_X;
        const int ly = (c.index / Chunk::SIZE_X) % Chunk::SIZE_Y;
        const int lz = c.index / (Chunk::SIZE_X * Chunk::SIZE_Y);
        requests.push_back({{int(coord.x) * Chunk::SIZE_X + lx,
                             int(coord.y) * Chunk::SIZE_Y + ly,
                             int(coord.z) * Chunk::SIZE_Z + lz},
                            EditMode::Replace,
                            c.block,
                            true,
                            c.rootOwner});
    }
    applying = true;
    editor.apply(requests);
    applying = false;
}

void TreeSimulation::update(double now) {
    today = now;
    clearSome(ClearBudgetMs);
    const int32_t day = int32_t(std::floor(now));
    if (sweep.empty() || chunks.treeRevision() != sweepRevision ||
        ++updates % SweepListInterval == 0) {
        sweep.clear();
        sweepRevision = chunks.treeRevision();
        chunks.forEachTreeColumn(
            [&](unsigned cx, unsigned cz, const TreeColumn& column) {
                int low = INT_MAX, high = INT_MIN;
                for (const auto& e : column.trees) {
                    const auto& roots = e.tree->rootWood;
                    const int shift = e.anchor.x - e.tree->anchor.x;
                    if (!roots.empty() &&
                        roots.x0 + shift < int(cx + 1) * Chunk::SIZE_X &&
                        roots.x1 + shift > int(cx) * Chunk::SIZE_X &&
                        roots.z0 < int(cz + 1) * Chunk::SIZE_Z &&
                        roots.z1 > int(cz) * Chunk::SIZE_Z) {
                        low = std::min(low, roots.y0);
                        high = std::max(high, roots.y1 - 1);
                    }
                    if (e.tree->wood.empty()) continue;
                    low = std::min(low, e.tree->wood.y0);
                    high = std::max(high, e.tree->wood.y1 - 1);
                }
                if (low > high) return;
                for (int cy = std::max(0, low / Chunk::SIZE_Y);
                     cy <= high / Chunk::SIZE_Y && cy < Y_CHUNKS; ++cy)
                    sweep.push_back({cx, unsigned(cy), cz});
            });
    }
    for (int n = 0; n < ChunksPerUpdate && !sweep.empty(); ++n) {
        const auto coord = sweep.front();
        sweep.pop_front();
        sweep.push_back(coord);
        const auto stamp = chunks.treeStamp(coord);
        if (!stamp.placed() || stamp.version != TreeVersion || stamp.day >= day)
            continue;
        const auto column = chunks.treeColumn(coord.x, coord.z);
        const auto tiles = chunks.getChunkShared(coord);
        if (!column || !tiles) continue;
        auto changes = treeWoodChanges(*tiles, coord, *column,
                                       chunks.treeSpecies(), stamp.day, day);
        if (const auto terrain = chunks.generatedColumns(coord.x, coord.z)) {
            auto roots =
                treeRootChanges(*tiles, coord, *column, chunks.treeSpecies(),
                                *terrain, stamp.day, day);
            changes.insert(changes.end(), roots.begin(), roots.end());
        }
        applyChanges(coord, changes);
        chunks.setTreeStamp(coord, {day, TreeVersion}, !changes.empty());
    }
}

std::vector<TreeSimulation::Owner> TreeSimulation::owners(TilePos pos,
                                                          uint8_t block) const {
    const auto column = chunks.treeColumn(unsigned(pos.x / Chunk::SIZE_X),
                                          unsigned(pos.z / Chunk::SIZE_Z));
    if (!column) return {};
    struct Candidate {
        Owner owner;
        float gap;
        bool inside;
    };
    std::vector<Candidate> found;
    for (const auto& entry : column->trees) {
        const auto& tree = *entry.tree;
        const auto* rule = chunks.treeSpecies().find(tree.species);
        if (!rule || rule->woodBlock != block) continue;
        const auto& k = *tree.skeleton;
        if (!k.formsWood()) continue;
        const float age = tree.growthAge(today);
        // The tile centre in the tree's frame (column copy of the anchor).
        const TreeVec q{float(pos.x - entry.anchor.x),
                        float(pos.y - entry.anchor.y) - 0.5f,
                        float(pos.z - entry.anchor.z)};
        for (size_t i = 0; i < k.segments.size(); ++i) {
            const auto& s = k.segments[i];
            if (!s.live()) continue;
            const float visible = k.visible(s, age);
            if (visible <= 0) continue;
            const TreeVec axis = s.b - s.a;
            const float length2 = axis.dot(axis);
            const float tu =
                length2 > 1e-6f ? (q - s.a).dot(axis) / length2 : 0.f;
            const float t = std::clamp(tu, 0.f, visible);
            const TreeVec d = q - lerp(s.a, s.b, t);
            const float r = k.radius(s, t, age, std::atan2(d.z, d.x));
            if (r < TreeVoxelRadius) continue;
            // The cut lies on the removed tile's lower face (half a tile
            // back along the axis): the face the stump keeps, where its
            // regrowth starts. At the tile centre it would float half a
            // tile above the remaining blocks.
            const float back =
                length2 > 1e-6f ? 0.5f / std::sqrt(length2) : 0.f;
            const float cutT = std::clamp(tu - back, 0.f, visible);
            const float gap = d.length() - std::sqrt(r * r + 0.4f);
            // A tile projecting inside a segment belongs to it rather than
            // to the end cap of the segment below: a cut at that joint
            // would otherwise land on the lower end and cut nothing.
            const bool inside = tu >= 0 && tu <= visible;
            if (gap < 0.75f)
                found.push_back({Owner{entry, int32_t(i), cutT}, gap, inside});
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.inside != b.inside) return a.inside;
                         return a.gap < b.gap;
                     });
    std::vector<Owner> out;
    out.reserve(found.size());
    for (auto& c : found) out.push_back(std::move(c.owner));
    return out;
}

bool TreeSimulation::severed(Owner& o, TilePos removed, bool thin) const {
    const auto& tree = *o.entry.tree;
    const auto& k = *tree.skeleton;
    const auto& s = k.segments[size_t(o.segment)];
    const float age = tree.growthAge(today);
    // Nothing grows above this point (the top of a stump left by an
    // earlier cut, or a branch tip): breaking it cuts nothing.
    if (o.t >= 0.98f * k.visible(s, age)) {
        bool above = false;
        for (const auto& c : k.segments)
            if (c.parent == o.segment && c.attachT >= o.t && c.live() &&
                k.visible(c, age) > 0)
                above = true;
        if (!above) return false;
    }
    const float r = k.radius(s, o.t, age);
    const auto* rule = chunks.treeSpecies().find(tree.species);
    // A settling trunk closes up after each cut: only a sawn-through
    // cross-section cuts it, however thin.
    const bool settles =
        rule && rule->species.cut.fell == TreeFell::Settle && s.order == 0;
    if (r <= ThinCut && !settles && thin) return true;
    if (!rule) return false;
    // Preserve trunk felling's shared ring test: many flared trunk
    // capsules and buttresses overlap, including their rounded end caps.
    if (s.order == 0) {
        const TreeVec axis = (s.b - s.a).normalized();
        const TreeVec ref =
            std::abs(axis.y) < 0.9f ? TreeVec{0, 1, 0} : TreeVec{1, 0, 0};
        const TreeVec u = axis.cross(ref).normalized();
        const TreeVec v = axis.cross(u).normalized();
        const TreeVec centre = lerp(s.a, s.b, o.t) + axis * 0.5f;
        const float reach = std::sqrt(r * r + 0.4f);
        const int steps = int(std::ceil(reach));
        const auto& a = o.entry.anchor;
        for (int i = -steps; i <= steps; ++i)
            for (int j = -steps; j <= steps; ++j) {
                if (float(i * i + j * j) > reach * reach) continue;
                const TreeVec p = centre + u * float(i) + v * float(j);
                const TilePos tile{a.x + int(std::lround(p.x)),
                                   a.y + int(std::lround(p.y + 0.5f)),
                                   a.z + int(std::lround(p.z))};
                if (tile.x == removed.x && tile.y == removed.y &&
                    tile.z == removed.z)
                    continue;
                const Tile* t = chunks.tileAt(
                    int(chunks.wrap().wrapTile(tile.x)), tile.y, tile.z);
                if (!t || t->block_id == rule->woodBlock) return false;
            }
        return true;
    }
    // Players saw tile rows, not a continuous plane perpendicular to a
    // sloping branch. Such a plane samples intact rows beside a complete
    // one-tile slit and would leave the disconnected limb standing.
    // Rasterize the owning segment on each transverse grid slice instead.
    TreeSkeleton section;
    section.trunkRadius = k.trunkRadius;
    section.curve = k.curve;
    section.flare = k.flare;
    section.segments.push_back(s);
    section.segments.back().parent = -1;
    auto bounds = tree.wood;
    bounds.x0 += o.entry.anchor.x - tree.anchor.x;
    bounds.x1 += o.entry.anchor.x - tree.anchor.x;
    const TreeVec axis = s.b - s.a;
    const float length = axis.length();
    if (length < 1e-6f) return false;
    const float direction[3]{axis.x / length, axis.y / length, axis.z / length};
    const float start[3]{s.a.x, s.a.y, s.a.z};
    const auto& a = o.entry.anchor;
    const int row[3]{removed.x, removed.y, removed.z};
    const float relative[3]{float(removed.x - a.x),
                            float(removed.y - a.y) - 0.5f,
                            float(removed.z - a.z)};
    std::array<int, 3> axes{0, 1, 2};
    std::sort(axes.begin(), axes.end(), [&](int x, int y) {
        return std::abs(direction[x]) > std::abs(direction[y]);
    });
    std::vector<uint8_t> mask;
    for (const int dim : axes) {
        // Nearly longitudinal slices do not separate the two ends of the
        // branch. Bound their oblique footprint to keep per-edit work small.
        if (std::abs(direction[dim]) < 0.25f) continue;
        const float along = (relative[dim] - start[dim]) / direction[dim];
        // Do not cut a rounded end cap of the previous segment after its
        // continuing segment has already been sawn. That would move the
        // stump down to the previous joint on another removal in this row.
        if (along < 0 || along > length * k.visible(s, age)) continue;
        const TreeVec centre = s.a + axis * (along / length);
        // Most edits leave obvious support near the axis. Test a handful
        // of exact rasterized cells before allocating the full slice;
        // otherwise sawing a massive trunk repeats a large mask per block.
        const int middle[3]{a.x + int(std::lround(centre.x)),
                            a.y + int(std::lround(centre.y + 0.5f)),
                            a.z + int(std::lround(centre.z))};
        bool obviousSupport = false;
        const int offset = std::max(1, int(r * 0.5f));
        for (int probe = 0; probe < 5 && !obviousSupport; ++probe) {
            int p[3]{middle[0], middle[1], middle[2]};
            p[dim] = row[dim];
            if (probe > 0)
                p[(dim + (probe <= 2 ? 1 : 2)) % 3] +=
                    probe % 2 ? offset : -offset;
            const Tile* tile =
                chunks.tileAt(int(chunks.wrap().wrapTile(p[0])), p[1], p[2]);
            if (tile && tile->block_id != rule->woodBlock) continue;
            markTreeWood(section, a,
                         {p[0], p[1], p[2], p[0] + 1, p[1] + 1, p[2] + 1}, age,
                         mask);
            obviousSupport = mask[0] != 0;
        }
        if (obviousSupport) continue;
        const float flare = s.order == 0 ? 1 + k.flare.amount : 1;
        const float reach =
            (std::max(s.r0, s.r1) * flare + 1) / std::abs(direction[dim]);
        TreeTileBox slice{a.x + int(std::floor(centre.x - reach)),
                          a.y + int(std::floor(centre.y + 0.5f - reach)),
                          a.z + int(std::floor(centre.z - reach)),
                          a.x + int(std::ceil(centre.x + reach)) + 1,
                          a.y + int(std::ceil(centre.y + 0.5f + reach)) + 1,
                          a.z + int(std::ceil(centre.z + reach)) + 1};
        int* low[3]{&slice.x0, &slice.y0, &slice.z0};
        int* high[3]{&slice.x1, &slice.y1, &slice.z1};
        *low[dim] = row[dim];
        *high[dim] = row[dim] + 1;
        slice = slice.intersect(bounds);
        if (slice.empty()) continue;
        // Consult actual tiles first. Rasterizing an entire massive trunk
        // slice for every removed block repeats millions of radius tests.
        // Only remaining wood (or unknown tiles) needs an ownership test;
        // include adjacent capsules so their joints also count as support.
        markTreeWood(k, a,
                     {removed.x, removed.y, removed.z, removed.x + 1,
                      removed.y + 1, removed.z + 1},
                     age, mask);
        const bool expected = mask[0] != 0;
        bool supported = false;
        for (int y = slice.y1 - 1; y >= slice.y0 && !supported; --y)
            for (int z = slice.z1 - 1; z >= slice.z0 && !supported; --z)
                for (int x = slice.x1 - 1; x >= slice.x0 && !supported; --x) {
                    const Tile* tile =
                        chunks.tileAt(int(chunks.wrap().wrapTile(x)), y, z);
                    if (tile && tile->block_id != rule->woodBlock) continue;
                    markTreeWood(k, a, {x, y, z, x + 1, y + 1, z + 1}, age,
                                 mask);
                    // Unloaded wood still counts as supporting the limb.
                    supported = mask[0] != 0;
                }
        if (expected && !supported) {
            // Put the stump on the retained face of the actual sawn row,
            // rather than projecting the last (possibly off-axis) tile.
            const float face =
                relative[dim] - std::copysign(0.5f, direction[dim]);
            o.t = std::clamp((face - start[dim]) / (direction[dim] * length),
                             0.f, k.visible(s, age));
            // The stump ends at that face: a plane across the sloping limb
            // would leave a wedge of blocks beyond the slit.
            o.normal = treeCutNormal(
                int8_t((dim + 1) * (direction[dim] < 0 ? -1 : 1)));
            return true;
        }
    }
    return severedByConnectivity(o, removed);
}

bool TreeSimulation::severedByConnectivity(Owner& o, TilePos removed) const {
    const auto& tree = *o.entry.tree;
    const auto& k = *tree.skeleton;
    const auto* rule = chunks.treeSpecies().find(tree.species);
    if (!rule || !rule->woodBlock) return false;
    const float age = tree.growthAge(today);
    const auto& s = k.segments[size_t(o.segment)];
    const auto& a = o.entry.anchor;
    // The limb's axis: the owning segment and the segments continuing it
    // (same order, attached at the joint), within the window.
    const float rCut = std::max(TreeVoxelRadius, k.radius(s, o.t, age));
    const float window = SlitWindowRadii * rCut + SlitWindowExtra;
    std::vector<int32_t> next(k.segments.size(), -1);
    for (size_t i = 0; i < k.segments.size(); ++i) {
        const auto& c = k.segments[i];
        if (c.parent >= 0 && c.live() && c.attachT >= 1 &&
            c.order == k.segments[size_t(c.parent)].order &&
            k.visible(c, age) > 0)
            next[size_t(c.parent)] = int32_t(i);
    }
    struct Part {
        int32_t segment;
        float start, length;
    };
    std::vector<Part> chain;
    const auto lengthOf = [&](int32_t i) {
        const auto& g = k.segments[size_t(i)];
        return (g.b - g.a).length();
    };
    {
        std::vector<int32_t> back;
        float reach = o.t * lengthOf(o.segment);
        for (int32_t at = o.segment; reach < window + rCut;) {
            const auto& g = k.segments[size_t(at)];
            if (g.parent < 0 || g.attachT < 1) break;
            const auto& p = k.segments[size_t(g.parent)];
            if (p.order != g.order || !p.live()) break;
            back.push_back(g.parent);
            reach += lengthOf(g.parent);
            at = g.parent;
        }
        float start = 0;
        for (auto it = back.rbegin(); it != back.rend(); ++it) {
            chain.push_back({*it, start, lengthOf(*it)});
            start += lengthOf(*it);
        }
        const float own = start;
        chain.push_back({o.segment, start, lengthOf(o.segment)});
        start += lengthOf(o.segment);
        for (int32_t at = next[size_t(o.segment)];
             at >= 0 && start - own - o.t * lengthOf(o.segment) < window + rCut;
             at = next[size_t(at)]) {
            chain.push_back({at, start, lengthOf(at)});
            start += lengthOf(at);
        }
    }
    struct Where {
        float arc = 0, distance = INFINITY, radius = 0;
        size_t part = 0;
    };
    const auto local = [&](int x, int y, int z) {
        return TreeVec{float(x - a.x), float(y - a.y) - 0.5f, float(z - a.z)};
    };
    const auto where = [&](const TreeVec& q) {
        Where w;
        for (size_t i = 0; i < chain.size(); ++i) {
            const auto& g = k.segments[size_t(chain[i].segment)];
            const TreeVec axis = g.b - g.a;
            const float l2 = axis.dot(axis);
            const float t = l2 > 1e-8f
                                ? std::clamp((q - g.a).dot(axis) / l2, 0.f, 1.f)
                                : 0.f;
            const float d = (q - lerp(g.a, g.b, t)).length();
            if (d < w.distance) {
                w.distance = d;
                w.arc = chain[i].start + t * chain[i].length;
                w.radius = std::max(TreeVoxelRadius, k.radius(g, t, age));
                w.part = i;
            }
        }
        return w;
    };
    const float chainEnd = chain.back().start + chain.back().length;
    const Where at = where(local(removed.x, removed.y, removed.z));
    const float nearLimit = std::max(0.f, at.arc - window);
    const float farLimit = std::min(chainEnd, at.arc + window);
    // A removal at the end of the limb leaves nothing hanging beyond it.
    if (farLimit - at.arc < 1.5f) return false;
    // Live tiles, read without the per-tile lock (the main thread is the
    // only writer); unloaded tiles count as wood.
    std::unordered_map<ChunkCoord, const Chunk*> loaded;
    const auto wrap = chunks.wrap();
    const auto wood = [&](int x, int y, int z) {
        if (y < 0 || y >= WORLD_HEIGHT_Y || z < 0) return false;
        const int wx = int(wrap.wrapTile(x));
        if (wx < 0) return false;
        const ChunkCoord c{unsigned(wx / Chunk::SIZE_X),
                           unsigned(y / Chunk::SIZE_Y),
                           unsigned(z / Chunk::SIZE_Z)};
        auto [it, inserted] = loaded.try_emplace(c, nullptr);
        if (inserted) it->second = chunks.getChunk(c);
        if (!it->second) return true;
        return it->second
                   ->tiles[z % Chunk::SIZE_Z][y % Chunk::SIZE_Y]
                          [wx % Chunk::SIZE_X]
                   .block_id == rule->woodBlock;
    };
    const auto inLimb = [&](const Where& w) {
        return w.distance <= w.radius + 1.5f && w.arc >= nearLimit - 2 &&
               w.arc <= farLimit + 2;
    };
    constexpr int Bias = 1 << 20;
    const auto key = [](int x, int y, int z) {
        return uint64_t(uint32_t(x + Bias)) << 42 |
               uint64_t(uint32_t(y + Bias)) << 21 |
               uint64_t(uint32_t(z + Bias));
    };
    // The seed: the limb's core at its far window end (the wood nearest the
    // axis there). If it no longer reaches the base, the far piece is off.
    struct Node {
        float arc;
        int x, y, z;
        bool operator>(const Node& o) const { return arc > o.arc; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    std::unordered_map<uint64_t, float> far;  // visited: the far piece
    {
        // The chain point at farLimit.
        TreeVec p{};
        float r = rCut;
        for (const auto& c : chain)
            if (farLimit >= c.start && farLimit <= c.start + c.length + 1e-3f) {
                const auto& g = k.segments[size_t(c.segment)];
                const float t =
                    c.length > 1e-6f
                        ? std::clamp((farLimit - c.start) / c.length, 0.f, 1.f)
                        : 0.f;
                p = lerp(g.a, g.b, t);
                r = std::max(TreeVoxelRadius, k.radius(g, t, age));
                break;
            }
        const int reach = int(std::ceil(r)) + 2;
        const int px = a.x + int(std::lround(p.x)),
                  py = a.y + int(std::lround(p.y + 0.5f)),
                  pz = a.z + int(std::lround(p.z));
        // Growing shells around that point; the first wood found.
        for (int shell = 0; shell <= reach && far.empty(); ++shell)
            for (int y = py - shell; y <= py + shell && far.empty(); ++y)
                for (int z = pz - shell; z <= pz + shell && far.empty(); ++z)
                    for (int x = px - shell; x <= px + shell; ++x) {
                        if (std::max({std::abs(x - px), std::abs(y - py),
                                      std::abs(z - pz)}) != shell)
                            continue;
                        if (!wood(x, y, z)) continue;
                        const Where w = where(local(x, y, z));
                        if (!inLimb(w) || w.arc < farLimit - 1.5f) continue;
                        far.emplace(key(x, y, z), w.arc);
                        open.push({w.arc, x, y, z});
                        break;
                    }
    }
    if (far.empty()) return false;
    // Best first toward the limb's base: an intact limb is found connected
    // after a short walk; a severed one explores only its far piece.
    static constexpr int Steps[6][3]{{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    while (!open.empty()) {
        const Node n = open.top();
        open.pop();
        if (n.arc <= nearLimit + 1) return false;  // still attached
        if (far.size() > SlitMaxVisits) return false;
        for (const auto& d : Steps) {
            const int x = n.x + d[0], y = n.y + d[1], z = n.z + d[2];
            const uint64_t id = key(x, y, z);
            if (far.contains(id)) continue;
            const Where w = where(local(x, y, z));
            if (!inLimb(w) || !wood(x, y, z)) continue;
            far.emplace(id, w.arc);
            open.push({w.arc, x, y, z});
        }
    }
    // Severed. The slit: hollow tiles inside the limb next to the far
    // piece; its near face: wood next to the slit outside the far piece.
    std::vector<TreeVec> slit, nearFace;
    std::unordered_set<uint64_t> seen;
    const auto unkey = [](uint64_t id, int& x, int& y, int& z) {
        x = int((id >> 42) & ((1u << 21) - 1)) - Bias;
        y = int((id >> 21) & ((1u << 21) - 1)) - Bias;
        z = int(id & ((1u << 21) - 1)) - Bias;
    };
    for (const auto& [id, arc] : far) {
        int x, y, z;
        unkey(id, x, y, z);
        for (const auto& d : Steps) {
            const int sx = x + d[0], sy = y + d[1], sz = z + d[2];
            const uint64_t sid = key(sx, sy, sz);
            if (far.contains(sid) || !seen.insert(sid).second) continue;
            const TreeVec q = local(sx, sy, sz);
            const Where w = where(q);
            if (wood(sx, sy, sz) || w.distance > w.radius - 0.5f) continue;
            slit.push_back(q);
            for (const auto& e : Steps) {
                const int nx = sx + e[0], ny = sy + e[1], nz = sz + e[2];
                if (far.contains(key(nx, ny, nz)) || !wood(nx, ny, nz))
                    continue;
                if (inLimb(where(local(nx, ny, nz))))
                    nearFace.push_back(local(nx, ny, nz));
            }
        }
    }
    // The axis direction at the removal, pointing toward the far piece.
    const auto& own = k.segments[size_t(chain[at.part].segment)];
    const TreeVec axis = (own.b - own.a).normalized();
    TreeVec normal = axis;
    if (slit.size() >= 3) {
        TreeVec c{};
        for (const auto& q : slit) c = c + q;
        c = c * (1.f / float(slit.size()));
        std::array<std::array<double, 3>, 3> m{};
        for (const auto& q : slit) {
            const double d[3]{q.x - c.x, q.y - c.y, q.z - c.z};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) m[i][j] += d[i] * d[j];
        }
        TreeVec fitted = smallestEigenvector(m);
        if (fitted.dot(axis) < 0) fitted = fitted * -1.f;
        // A slit nearly along the limb is no face to cut on.
        if (fitted.dot(axis) >= 0.35f) normal = fitted;
    }
    // Offset: every tile of the far piece beyond the plane; the near face
    // before it when a plane separates them (a stepped slit may cost the
    // stump a few corner tiles otherwise).
    float minFar = INFINITY, maxNear = -INFINITY;
    for (const auto& [id, arc] : far) {
        int x, y, z;
        unkey(id, x, y, z);
        minFar = std::min(minFar, normal.dot(local(x, y, z)));
    }
    for (const auto& q : nearFace) maxNear = std::max(maxNear, normal.dot(q));
    const float offset =
        maxNear < minFar ? (maxNear + minFar) * 0.5f : minFar - 0.01f;
    // Where the plane crosses the limb's axis, nearest the removal.
    float best = INFINITY;
    for (const auto& c : chain) {
        const auto& g = k.segments[size_t(c.segment)];
        const float denom = normal.dot(g.b - g.a);
        if (std::abs(denom) < 1e-6f) continue;
        const float t = (offset - normal.dot(g.a)) / denom;
        if (t < 0 || t > 1) continue;
        const float arc = c.start + t * c.length;
        if (std::abs(arc - at.arc) < best) {
            best = std::abs(arc - at.arc);
            o.segment = c.segment;
            o.t = std::min(t, k.visible(g, age));
            o.normal = normal;
        }
    }
    if (best == INFINITY) {
        // The plane misses the axis (a slit off to one side): cut across the
        // limb just before the far piece.
        float nearest = INFINITY;
        for (const auto& [id, arc] : far) nearest = std::min(nearest, arc);
        for (const auto& c : chain)
            if (nearest >= c.start && nearest <= c.start + c.length + 1e-3f) {
                const auto& g = k.segments[size_t(c.segment)];
                o.segment = c.segment;
                o.t = c.length > 1e-6f
                          ? std::clamp((nearest - 0.5f - c.start) / c.length,
                                       0.f, k.visible(g, age))
                          : 0.f;
                o.normal = {};
                break;
            }
    }
    return true;
}

void TreeSimulation::blockChanged(const BlockChange& change) {
    if (applying || !change.before.block_id ||
        change.after.block_id == change.before.block_id)
        return;
    if (!change.wasRoot) woodRemoved(change);
    if (change.before.isSolid() && !change.after.isSolid())
        checkBases(change.pos);
}

void TreeSimulation::checkBases(TilePos pos) {
    const auto wrap = chunks.wrap();
    pos.x = int(wrap.wrapTile(pos.x));
    const auto column = chunks.treeColumn(unsigned(pos.x / Chunk::SIZE_X),
                                          unsigned(pos.z / Chunk::SIZE_Z));
    if (!column) return;
    // Copy: killing a tree rebuilds the column.
    std::vector<TreeColumn::Entry> dying;
    for (const auto& entry : column->trees) {
        const auto* rule = chunks.treeSpecies().find(entry.tree->species);
        if (!rule || rule->species.immortal()) continue;
        const auto& a = entry.anchor;
        // Only the anchor layer and the first wood row form the base.
        if (pos.y < a.y || pos.y > a.y + 1) continue;
        const double reach = std::max(0.5, entry.tree->discRadius - 0.5) + 1;
        const double dx = pos.x - a.x, dz = pos.z - a.z;
        if (dx * dx + dz * dz > reach * reach) continue;
        if (baseLost(entry)) dying.push_back(entry);
    }
    for (const auto& entry : dying) kill(entry);
}

bool TreeSimulation::baseLost(const TreeColumn::Entry& entry) {
    const auto& tree = *entry.tree;
    const auto& a = entry.anchor;
    // Ground: tiles of the anchor layer under the flared trunk (at least
    // the anchor tile). Unloaded tiles count as standing.
    const double radius = std::max(0.5, tree.discRadius - 0.5);
    const int span = int(std::ceil(radius));
    bool ground = false;
    for (int z = -span; z <= span && !ground; ++z)
        for (int x = -span; x <= span && !ground; ++x) {
            if (double(x * x + z * z) > radius * radius && (x || z)) continue;
            const Tile* t = chunks.tileAt(a.x + x, a.y, a.z + z);
            if (!t) {
                ground = true;
            } else if (t->isSolid()) {
                const auto* root = chunks.rootAt(a.x + x, a.y, a.z + z);
                ground = !root || root->owner == tree.key;
            }
        }
    if (!ground) return true;
    // Wood: a trunk cut at the ground leaves no stump to grow from (a
    // settling trunk comes down onto it instead).
    const auto* species = chunks.treeSpecies().find(tree.species);
    if (species && species->species.cut.fell == TreeFell::Settle) return false;
    for (const auto& s : tree.skeleton->segments)
        if (s.live() && s.order == 0 && !(s.flags & TreeSegment::Sprout) &&
            (s.flags & TreeSegment::CutEnd) && s.b.y < 0.5f)
            return true;
    // Otherwise the first row of trunk wood the tree has now, if any. A
    // trunk that settles closes a sawn ring, even at the root.
    const auto* rule = chunks.treeSpecies().find(tree.species);
    if (!rule || tree.wood.empty() ||
        rule->species.cut.fell == TreeFell::Settle)
        return false;
    const int shift = a.x - tree.anchor.x;
    const TreeTileBox row =
        TreeTileBox{tree.wood.x0 + shift, a.y + 1, tree.wood.z0,
                    tree.wood.x1 + shift, a.y + 2, tree.wood.z1}
            .intersect({tree.wood.x0 + shift, tree.wood.y0, tree.wood.z0,
                        tree.wood.x1 + shift, tree.wood.y1, tree.wood.z1});
    if (row.empty()) return false;
    std::vector<uint8_t> mask;
    markTreeWood(*tree.skeleton, a, row, tree.growthAge(today), mask);
    bool expected = false;
    for (int z = row.z0; z < row.z1; ++z)
        for (int x = row.x0; x < row.x1; ++x) {
            if (!mask[row.index(x, row.y0, z)]) continue;
            expected = true;
            const Tile* t = chunks.tileAt(x, row.y0, z);
            if (!t || t->block_id == rule->woodBlock) return false;
        }
    return expected;
}

void TreeSimulation::kill(const TreeColumn::Entry& entry) {
    const auto tree = entry.tree;
    auto map = std::make_shared<TreeOverrideMap>(*chunks.treeOverrides());
    if (tree->key & TreeRecord::PlantedBit) {
        map->erase(tree->key);
    } else {
        TreeRecord record;
        record.key = tree->key;
        record.removed = true;
        (*map)[tree->key] = std::move(record);
    }
    chunks.setTreeOverrides(std::move(map));
    // Its remaining wood goes (loaded chunks, over the next updates;
    // unloaded ones keep theirs until edited: their trees no longer know
    // it). Chunk by chunk: a massive tree's box is billions of tiles.
    if (!tree->wood.empty()) {
        const int shift = entry.anchor.x - tree->anchor.x;
        const TreeTileBox box{tree->wood.x0 + shift, tree->wood.y0,
                              tree->wood.z0,         tree->wood.x1 + shift,
                              tree->wood.y1,         tree->wood.z1};
        queueClears(box, {entry.anchor.x, entry.anchor.y, entry.anchor.z},
                    {{}, 0, tree, entry.anchor, nullptr, 0});
    }
    chunks.removeTree(tree);
    changed = true;
    ++deaths;
}

void TreeSimulation::woodRemoved(const BlockChange& change) {
    if (!chunks.grownBlocks().test(change.before.block_id)) return;
    times = {};
    auto start = std::chrono::steady_clock::now();
    auto candidates = owners(change.pos, change.before.block_id);
    times.owner = lap(start);
    // Of the segments the removal cuts through (a few at most: the trunk,
    // a buttress, a limb's joint), the thickest: a sawn trunk takes its
    // buttresses' joint with it.
    const Owner* through = nullptr;
    float thickest = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto& c = candidates[i];
        const auto& k = *c.entry.tree->skeleton;
        const auto& segment = k.segments[size_t(c.segment)];
        // A settling trunk is not cut again before its ring has closed.
        if (segment.order == 0 && settling.contains(c.entry.tree->key))
            continue;
        const float r = k.radius(segment, c.t, c.entry.tree->growthAge(today));
        if (r > thickest && severed(c, change.pos, i == 0)) {
            through = &c;
            thickest = r;
        }
    }
    times.severed = lap(start);
    if (through) cut(*through);
}

void TreeSimulation::commitTree(TreeRecord record, const TreeInstancePtr& tree,
                                const TreeTileBox& oldBounds) {
    auto map = std::make_shared<TreeOverrideMap>(*chunks.treeOverrides());
    (*map)[record.key] = std::move(record);
    chunks.setTreeOverrides(std::move(map));
    chunks.updateTree(tree, oldBounds);
    changed = true;
}

void TreeSimulation::queueClears(const TreeTileBox& box, TilePos from,
                                 Clear clear) {
    const auto wrap = chunks.wrap();
    struct Queued {
        double distance;
        ChunkCoord coord;
    };
    std::vector<Queued> order;
    // Iterate residency, not the enormous mostly-unloaded tree box. The
    // snapshot is already available; column rebuilds cannot delay a cut.
    chunks.forEachReady([&](const ChunkCoord& coord, const Chunk&) {
        const int x = int(wrap.imageX(int(coord.x) * Chunk::SIZE_X,
                                      (double(box.x0) + box.x1) * 0.5));
        const int y = int(coord.y) * Chunk::SIZE_Y;
        const int z = int(coord.z) * Chunk::SIZE_Z;
        if (box.intersect({x, y, z, x + Chunk::SIZE_X, y + Chunk::SIZE_Y,
                           z + Chunk::SIZE_Z})
                .empty())
            return;
        const double dx = x + Chunk::SIZE_X * 0.5 - from.x,
                     dy = y + Chunk::SIZE_Y * 0.5 - from.y,
                     dz = z + Chunk::SIZE_Z * 0.5 - from.z;
        order.push_back({dx * dx + dy * dy + dz * dz, coord});
    });
    std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) {
        return l.distance < r.distance;
    });
    if (clear.settling && !order.empty())
        settling[clear.settling] += order.size();
    for (auto& q : order) {
        clear.coord = q.coord;
        clears.push_back(clear);
    }
}

void TreeSimulation::startClear(const Clear& clear) {
    if (!chunks.getChunk(clear.coord)) {
        finishClear(clear);  // unloaded meanwhile: cleared when it loads
        return;
    }
    auto& pool = chunks.threadPool();
    if (clear.removed) {
        const auto* rule = chunks.treeSpecies().find(clear.removed->species);
        clearJobs.push_back(
            {clear,
             pool.submit_task(
                 [tree = clear.removed, coord = clear.coord,
                  anchor = clear.anchor, block = rule ? rule->woodBlock : 0,
                  age = clear.removed->growthAge(today),
                  period = chunks.wrap().tiles] {
                     return treeWoodRemoval(coord, *tree, anchor, block, age,
                                            period);
                 },
                 BS::pr::high)});
        return;
    }
    const auto* rule = chunks.treeSpecies().find(clear.tree->species);
    clearJobs.push_back(
        {clear,
         pool.submit_task(
             [tree = clear.tree, coord = clear.coord, anchor = clear.anchor,
              block = rule ? rule->woodBlock : 0, day = clear.cutDay,
              settles = clear.settling != 0, period = chunks.wrap().tiles] {
                 return treeCutWoodDelta(coord, *tree, anchor, block,
                                         uint16_t(tree->cuts.size()), day,
                                         settles, period);
             },
             BS::pr::high)});
}

void TreeSimulation::finishClear(const Clear& clear) {
    if (!clear.settling) return;
    const auto it = settling.find(clear.settling);
    if (it != settling.end() && --it->second == 0) settling.erase(it);
}

void TreeSimulation::clearSome(double budgetMs) {
    using namespace std::chrono_literals;
    const auto started = std::chrono::steady_clock::now();
    const auto spent = [&] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    // Finished chunks: their edits, within the budget.
    for (size_t i = 0; i < clearJobs.size() && spent() < budgetMs;) {
        if (clearJobs[i].delta.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(clearJobs[i]);
        clearJobs.erase(clearJobs.begin() + std::ptrdiff_t(i));
        TreeWoodDelta delta;
        try {
            delta = job.delta.get();
        } catch (const std::exception&) {
        }
        if (const auto tiles = chunks.getChunkShared(job.clear.coord))
            applyChanges(job.clear.coord, filterTreeWood(*tiles, delta));
        finishClear(job.clear);
    }
    // Keep the pool busy with the next nearby chunks.
    while (!clears.empty() && clearJobs.size() < MaxClearJobs) {
        auto clear = std::move(clears.front());
        clears.pop_front();
        startClear(clear);
    }
}

void TreeSimulation::finishClears() {
    while (!clearJobs.empty() || !clears.empty()) {
        for (auto& job : clearJobs) job.delta.wait();
        clearSome(INFINITY);
    }
}

void TreeSimulation::cut(const Owner& o) {
    const auto& old = *o.entry.tree;
    const auto* rule = chunks.treeSpecies().find(old.species);
    if (!rule) return;
    TreeRecord record;
    if (const auto it = chunks.treeOverrides()->find(old.key);
        it != chunks.treeOverrides()->end())
        record = it->second;
    record.key = old.key;
    record.cuts = old.cuts;
    record.cuts.push_back({o.segment, o.t, old.age(today), o.normal});
    auto start = std::chrono::steady_clock::now();
    auto tree = std::make_shared<TreeInstance>(old);
    tree->skeleton = applyTreeCuts(*old.skeleton, rule->species, record.cuts,
                                   record.cuts.size() - 1);
    tree->variant = -1;
    tree->cuts = record.cuts;
    finishTreeInstance(*tree);
    times.rebuild = lap(start);
    const auto oldBounds = old.bounds;
    commitTree(std::move(record), tree, oldBounds);
    times.commit = lap(start);
    ++cuts;
    // The wood above the cut goes in loaded chunks over the next updates,
    // nearest first; unloaded ones clear it when they load (their stamps
    // are older than the cut).
    const double cutDay = double(tree->birthDay) + tree->cuts.back().age;
    const auto& s = old.skeleton->segments[size_t(o.segment)];
    const TreeVec at = lerp(s.a, s.b, o.t);
    const auto& a = o.entry.anchor;
    auto box =
        treeCutWoodBounds(*tree->skeleton, a, uint16_t(tree->cuts.size()));
    const bool settles =
        rule->species.cut.fell == TreeFell::Settle && s.order == 0;
    // Settling moves wood one tile along the axis as well as removing it.
    if (settles && !box.empty()) {
        --box.x0;
        --box.y0;
        --box.z0;
        ++box.x1;
        ++box.y1;
        ++box.z1;
    }
    queueClears(box,
                {a.x + int(std::lround(at.x)), a.y + int(std::lround(at.y)),
                 a.z + int(std::lround(at.z))},
                {{}, cutDay, nullptr, a, tree, settles ? tree->key : 0});
    times.clear = lap(start);
}

bool TreeSimulation::plant(TilePos ground, uint8_t species, double now) {
    today = now;
    const auto* rule = chunks.treeSpecies().find(species);
    if (!rule) return false;
    ground.x = int(chunks.wrap().wrapTile(ground.x));
    const Tile* base = chunks.tileAt(ground.x, ground.y, ground.z);
    const Tile* above = chunks.tileAt(ground.x, ground.y + 1, ground.z);
    if (!base || !base->isSolid() || !above || !above->isAir()) return false;
    // Headroom: air above the sapling up to a ceiling or the build limit.
    int top = ground.y + 1;
    while (top < WORLD_HEIGHT_Y) {
        const Tile* t = chunks.tileAt(ground.x, top, ground.z);
        if (t && !t->isAir()) break;
        ++top;
    }
    TreeRecord record;
    record.anchor = {ground.x, ground.y, ground.z};
    record.key = plantedTreeKey(record.anchor);
    if (chunks.treeOverrides()->contains(record.key)) return false;
    record.species = species;
    record.seed = uint32_t(record.key ^ (record.key >> 32));
    record.birthDay = int32_t(std::floor(now));
    record.headroom = float(top - (ground.y + 1));
    const auto tree = buildPlantedTree(chunks.treeSpecies(), record);
    if (!tree) return false;
    commitTree(std::move(record), tree, {});
    // Growth reaches its chunks through the sweep (their stamps predate
    // the tree).
    sweep.clear();
    return true;
}

std::string TreeSimulation::describe(TilePos tile, double now) const {
    const auto column = chunks.treeColumn(unsigned(tile.x / Chunk::SIZE_X),
                                          unsigned(tile.z / Chunk::SIZE_Z));
    if (!column) return {};
    // Wood boxes of neighbouring trees overlap: the nearest trunk wins.
    const TreeColumn::Entry* found = nullptr;
    int64_t nearest = INT64_MAX;
    for (const auto& entry : column->trees) {
        const auto& tree = *entry.tree;
        const int shift = entry.anchor.x - tree.anchor.x;
        const auto& b = tree.wood;
        if (tile.x < b.x0 + shift || tile.x >= b.x1 + shift || tile.y < b.y0 ||
            tile.y >= b.y1 || tile.z < b.z0 || tile.z >= b.z1)
            continue;
        const int64_t dx = tile.x - entry.anchor.x,
                      dz = tile.z - entry.anchor.z;
        if (dx * dx + dz * dz < nearest) {
            nearest = dx * dx + dz * dz;
            found = &entry;
        }
    }
    if (found) {
        const auto& tree = *found->tree;
        const auto* rule = chunks.treeSpecies().find(tree.species);
        const auto& k = *tree.skeleton;
        const double age = now - tree.birthDay;
        std::ostringstream out;
        out << (rule ? rule->species.name : "?") << ", age " << int(age)
            << " / mature " << int(k.matureAge()) << " days, " << int(k.height)
            << " m";
        if (!tree.cuts.empty()) out << ", " << tree.cuts.size() << " cut(s)";
        if (!k.alive(age)) out << ", dead";
        if (tree.key & TreeRecord::PlantedBit) out << ", planted";
        return out.str();
    }
    return {};
}
