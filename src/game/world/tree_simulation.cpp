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
        [this](const BlockChange& change) { blockChanged(change); });
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
    bool bestInside = false;
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
            const float r = k.radius(s, t, age);
            if (r < TreeVoxelRadius) continue;
            const TreeVec d = q - lerp(s.a, s.b, t);
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
            if (gap < 0.75f && (!best || (inside && !bestInside) ||
                                (inside == bestInside && gap < bestGap))) {
                bestGap = gap;
                bestInside = inside;
                best = Owner{entry, int32_t(i), cutT};
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
    // The slice through the removed row: half a tile past the cut face.
    const TreeVec centre =
        lerp(s.a, s.b, o.t) + (s.b - s.a).normalized() * 0.5f;
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

void TreeSimulation::blockChanged(const BlockChange& change) {
    if (applying || !change.before.block_id ||
        change.after.block_id == change.before.block_id)
        return;
    woodRemoved(change);
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
            ground = !t || t->isSolid();
        }
    if (!ground) return true;
    // Wood: a trunk cut at the ground leaves no stump to grow from.
    for (const auto& s : tree.skeleton->segments)
        if (s.live() && s.order == 0 && !(s.flags & TreeSegment::Sprout) &&
            (s.flags & TreeSegment::CutEnd) && s.b.y < 0.5f)
            return true;
    // Otherwise the first row of trunk wood the tree has now, if any.
    const auto* rule = chunks.treeSpecies().find(tree.species);
    if (!rule || tree.wood.empty()) return false;
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
    // Its remaining wood goes (loaded chunks; unloaded ones keep theirs
    // until edited: their trees no longer know it).
    if (!tree->wood.empty()) {
        const int shift = entry.anchor.x - tree->anchor.x;
        const TreeTileBox box{tree->wood.x0 + shift, tree->wood.y0,
                              tree->wood.z0,         tree->wood.x1 + shift,
                              tree->wood.y1,         tree->wood.z1};
        std::vector<uint8_t> mask;
        markTreeWood(*tree->skeleton, entry.anchor, box, tree->growthAge(today),
                     mask);
        const auto* rule = chunks.treeSpecies().find(tree->species);
        std::vector<EditRequest> requests;
        for (int y = box.y0; y < box.y1; ++y)
            for (int z = box.z0; z < box.z1; ++z)
                for (int x = box.x0; x < box.x1; ++x) {
                    if (!mask[box.index(x, y, z)]) continue;
                    const Tile* t = chunks.tileAt(x, y, z);
                    if (rule && t && t->block_id == rule->woodBlock)
                        requests.push_back({{x, y, z}, EditMode::Replace, 0});
                }
        applying = true;
        editor.apply(requests);
        applying = false;
    }
    chunks.removeTree(tree);
    changed = true;
    ++deaths;
}

void TreeSimulation::woodRemoved(const BlockChange& change) {
    if (!chunks.grownBlocks().test(change.before.block_id)) return;
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
        if (k.dead || age >= k.lifespan) out << ", dead";
        if (tree.key & TreeRecord::PlantedBit) out << ", planted";
        return out.str();
    }
    return {};
}
