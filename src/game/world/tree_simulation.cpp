#include "tree_simulation.hpp"

#include <cmath>
#include <sstream>

namespace {
// Segments up to this radius are cut by removing any of their wood.
constexpr float ThinCut = 2;
// The growth sweep rebuilds its chunk list this often (updates).
constexpr size_t SweepListInterval = 600;

TreeVec perpendicular(TreeVec d) {
    const TreeVec ref =
        std::abs(d.y) < 0.9f ? TreeVec{0, 1, 0} : TreeVec{1, 0, 0};
    return d.cross(ref).normalized();
}
}  // namespace

TreeSimulation::TreeSimulation(ChunkManager& chunks, WorldEditor& editor)
    : chunks(chunks), editor(editor) {
    connection = editor.blockChanged.connect(
        [this](const BlockChange& change) { woodRemoved(change); });
}

TreeSimulation::~TreeSimulation() {
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
                            c.block});
    }
    applying = true;
    editor.apply(requests);
    applying = false;
}

void TreeSimulation::update(double now) {
    today = now;
    const int32_t day = int32_t(std::floor(now));
    if (sweep.empty() || chunks.treeRevision() != sweepRevision ||
        ++updates % SweepListInterval == 0) {
        sweep.clear();
        sweepRevision = chunks.treeRevision();
        chunks.forEachTreeColumn(
            [&](unsigned cx, unsigned cz, const TreeColumn& column) {
                int low = INT_MAX, high = INT_MIN;
                for (const auto& e : column.trees) {
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
        const auto changes = treeWoodChanges(
            *tiles, coord, *column, chunks.treeSpecies(), stamp.day, day);
        applyChanges(coord, changes);
        chunks.setTreeStamp(coord, {day, TreeVersion}, !changes.empty());
    }
}

std::optional<TreeSimulation::Owner> TreeSimulation::owner(
    TilePos pos, uint8_t block) const {
    const auto column = chunks.treeColumn(unsigned(pos.x / Chunk::SIZE_X),
                                          unsigned(pos.z / Chunk::SIZE_Z));
    if (!column) return std::nullopt;
    std::optional<Owner> best;
    float bestGap = INFINITY;
    for (const auto& entry : column->trees) {
        const auto& tree = *entry.tree;
        const auto* rule = chunks.treeSpecies().find(tree.species);
        if (!rule || rule->woodBlock != block) continue;
        const auto& k = *tree.skeleton;
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
            const float t =
                length2 > 1e-6f
                    ? std::clamp((q - s.a).dot(axis) / length2, 0.f, visible)
                    : 0.f;
            const float r = k.radius(s, t, age);
            if (r < TreeVoxelRadius) continue;
            const TreeVec d = q - lerp(s.a, s.b, t);
            const float gap = d.length() - std::sqrt(r * r + 0.4f);
            if (gap < 0.75f && gap < bestGap) {
                bestGap = gap;
                best = Owner{entry, int32_t(i), t};
            }
        }
    }
    return best;
}

bool TreeSimulation::severed(const Owner& o, TilePos removed) const {
    const auto& tree = *o.entry.tree;
    const auto& k = *tree.skeleton;
    const auto& s = k.segments[size_t(o.segment)];
    const float age = tree.growthAge(today);
    // Nothing grows above this point (the top of a stump left by an
    // earlier cut, or a branch tip): breaking it cuts nothing.
    if (o.t >= 0.98f * k.visible(s, age)) {
        bool above = false;
        for (const auto& c : k.segments)
            if (c.parent == o.segment && c.live() && k.visible(c, age) > 0)
                above = true;
        if (!above) return false;
    }
    const float r = k.radius(s, o.t, age);
    if (r <= ThinCut) return true;
    // Thick parts: the cross-section at the cut must hold no wood at all.
    const auto* rule = chunks.treeSpecies().find(tree.species);
    const TreeVec axis = (s.b - s.a).normalized();
    const TreeVec u = perpendicular(axis), v = axis.cross(u).normalized();
    const TreeVec centre = lerp(s.a, s.b, o.t);
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
            const Tile* t = chunks.tileAt(tile.x, tile.y, tile.z);
            // Unloaded wood cannot be checked: the part still stands.
            if (!t || t->block_id == rule->woodBlock) return false;
        }
    return true;
}

void TreeSimulation::woodRemoved(const BlockChange& change) {
    if (applying || !change.before.block_id ||
        change.after.block_id == change.before.block_id ||
        !chunks.grownBlocks().test(change.before.block_id))
        return;
    const auto o = owner(change.pos, change.before.block_id);
    if (!o || !severed(*o, change.pos)) return;
    cut(*o);
}

void TreeSimulation::commitTree(TreeRecord record, const TreeInstancePtr& tree,
                                const TreeTileBox& oldBounds) {
    auto map = std::make_shared<TreeOverrideMap>(*chunks.treeOverrides());
    (*map)[record.key] = std::move(record);
    chunks.setTreeOverrides(std::move(map));
    chunks.updateTree(tree, oldBounds);
    changed = true;
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
    record.cuts.push_back({o.segment, o.t, old.age(today)});
    auto tree = std::make_shared<TreeInstance>(old);
    tree->skeleton = applyTreeCuts(*old.skeleton, rule->species, record.cuts,
                                   record.cuts.size() - 1);
    tree->variant = -1;
    tree->cuts = record.cuts;
    finishTreeInstance(*tree);
    const auto oldBounds = old.bounds;
    commitTree(std::move(record), tree, oldBounds);
    ++cuts;
    // The wood above the cut goes now in loaded chunks; unloaded ones clear
    // it when they load (their stamps are older than the cut).
    const double cutDay = double(tree->birthDay) + tree->cuts.back().age;
    const auto& box = tree->wood;
    const auto wrap = chunks.wrap();
    for (int cz = std::max(0, box.z0 / Chunk::SIZE_Z);
         cz <= (box.z1 - 1) / Chunk::SIZE_Z; ++cz)
        for (int cxi = int(std::floor(double(box.x0) / Chunk::SIZE_X));
             cxi <= int(std::floor(double(box.x1 - 1) / Chunk::SIZE_X));
             ++cxi) {
            const int64_t cx = wrap.enabled() ? wrap.wrapChunk(cxi) : cxi;
            if (cx < 0) continue;
            const auto column = chunks.treeColumn(unsigned(cx), unsigned(cz));
            if (!column) continue;
            for (int cy = std::max(0, box.y0 / Chunk::SIZE_Y);
                 cy <= (box.y1 - 1) / Chunk::SIZE_Y && cy < Y_CHUNKS; ++cy) {
                const ChunkCoord coord{unsigned(cx), unsigned(cy),
                                       unsigned(cz)};
                const auto tiles = chunks.getChunkShared(coord);
                if (!tiles) continue;
                applyChanges(coord, treeWoodChanges(*tiles, coord, *column,
                                                    chunks.treeSpecies(),
                                                    cutDay - 1e-3, cutDay));
            }
        }
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
    for (const auto& entry : column->trees) {
        const auto& tree = *entry.tree;
        const int shift = entry.anchor.x - tree.anchor.x;
        const auto& b = tree.wood;
        if (tile.x < b.x0 + shift || tile.x >= b.x1 + shift || tile.y < b.y0 ||
            tile.y >= b.y1 || tile.z < b.z0 || tile.z >= b.z1)
            continue;
        const auto* rule = chunks.treeSpecies().find(tree.species);
        const auto& k = *tree.skeleton;
        const double age = now - tree.birthDay;
        std::ostringstream out;
        out << (rule ? rule->species.name : "?") << ", age " << int(age)
            << " / mature " << int(k.matureAge()) << " days, " << int(k.height)
            << " m";
        if (!tree.cuts.empty()) out << ", " << tree.cuts.size() << " cut(s)";
        if (k.dead || age >= k.lifespan) out << ", dead";
        if (tree.key & TreeRecord::PlantedBit) out << ", planted";
        return out.str();
    }
    return {};
}
