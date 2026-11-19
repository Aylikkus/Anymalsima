#include "sky_height_map.hpp"

#include <algorithm>
#include <climits>

#include "light_volume.hpp"
#include "tree_rasterizer.hpp"
#include "world_editor.hpp"

namespace {
// Builds in flight; each is a few milliseconds (an elder's column: one
// skeleton projection per tree overlapping it).
constexpr size_t MaxJobs = 4;

struct BuildInputs {
    std::shared_ptr<const ChunkColumnInputs> terrain;
    std::shared_ptr<const TreeColumn> trees;
    std::shared_ptr<const ColumnEdit> edit;
    std::shared_ptr<const sky::OpacityTable> opacity;
    const TreeSpeciesTable* species = nullptr;
    unsigned cx = 0, cz = 0;
    int32_t day = 0;
};

sky::Tile buildTile(const BuildInputs& in) {
    sky::TileInputs tile;
    tile.edit = in.edit;
    tile.opacity = in.opacity.get();
    for (int i = 0; i < sky::Tile::Size; ++i) {
        tile.surface[size_t(i)] = in.terrain->columns[size_t(i)].surface_y;
        tile.structureTop[size_t(i)] = INT_MIN;
    }
    // Trees, as placeTreeWood places them (anchors at the copy nearest the
    // column).
    const TreeTileBox box{int(in.cx) * Chunk::SIZE_X,
                          0,
                          int(in.cz) * Chunk::SIZE_Z,
                          int(in.cx + 1) * Chunk::SIZE_X,
                          WORLD_HEIGHT_Y,
                          int(in.cz + 1) * Chunk::SIZE_Z};
    std::vector<int> tops;
    if (in.trees && in.species)
        for (const auto& entry : in.trees->trees) {
            const auto& tree = *entry.tree;
            const auto* rule = in.species->find(tree.species);
            if (!rule || !rule->woodBlock || !tree.skeleton) continue;
            const int shift = entry.anchor.x - tree.anchor.x;
            TreeTileBox wood = tree.wood;
            wood.x0 += shift;
            wood.x1 += shift;
            const auto part = wood.intersect(box);
            if (part.empty()) continue;
            const float age = tree.growthAge(in.day);
            if (age <= 0) continue;
            treeWoodTops(*tree.skeleton, entry.anchor, part, age, tops);
            const int width = part.x1 - part.x0;
            for (int z = part.z0; z < part.z1; ++z)
                for (int x = part.x0; x < part.x1; ++x) {
                    const int top =
                        tops[size_t(z - part.z0) * width + size_t(x - part.x0)];
                    int& slot = tile.structureTop[size_t(
                        sky::Tile::index(x - box.x0, z - box.z0))];
                    slot = std::max(slot, top);
                }
        }
    return sky::build(tile);
}
}  // namespace

SkyHeightMap::SkyHeightMap(ChunkManager& chunks, TerrainEdits& edits,
                           const DataStorage<Block>& blocks, ThreadPool& pool)
    : chunks(chunks), edits(edits), blocks(blocks), pool(pool) {
    auto table = std::make_shared<sky::OpacityTable>();
    table->fill(0);
    for (size_t id = 1; id <= blocks.items.size() && id < table->size(); ++id)
        (*table)[id] = blocks.get(int(id)).properties.opacity;
    opacity = std::move(table);
    editsConnection =
        edits.changed.connect([this](uint64_t key) { invalidate(key); });
    treesConnection = chunks.columnTreesChanged.connect(
        [this](uint64_t key) { invalidate(key); });
    releasedConnection = chunks.columnReleased.connect([this](uint64_t key) {
        entries.erase(key);
        queue.erase({unsigned(key >> 32), 0, unsigned(key)});
        observations.erase(key);
    });
    // A chunk arriving may hold tiles that changed while it was unloaded
    // (tree catch-up): check the overrides in its column again.
    readyConnection = chunks.becameReady.connect([this](const ChunkCoord& c) {
        const auto key = columnKey(c.x, c.z);
        const auto it = overridden.find(key);
        if (it == overridden.end()) return;
        for (const auto& [index, column] : it->second)
            observations[key].insert(index);
    });
}

SkyHeightMap::~SkyHeightMap() {
    if (editor) editor->blockChanged.disconnect(editConnection);
    edits.changed.disconnect(editsConnection);
    chunks.columnTreesChanged.disconnect(treesConnection);
    chunks.columnReleased.disconnect(releasedConnection);
    chunks.becameReady.disconnect(readyConnection);
    for (auto& job : jobs) job.future.wait();
}

void SkyHeightMap::unwatch() {
    if (editor) editor->blockChanged.disconnect(editConnection);
    editor = nullptr;
}

void SkyHeightMap::watch(WorldEditor& source) {
    if (editor) editor->blockChanged.disconnect(editConnection);
    editor = &source;
    editConnection = source.blockChanged.connect([this](const BlockChange& c) {
        if (tileOpacity(c.before, blocks) == tileOpacity(c.after, blocks))
            return;
        const auto x = unsigned(chunks.wrap().wrapTile(c.pos.x));
        const auto z = unsigned(c.pos.z);
        observations[columnKey(x / Chunk::SIZE_X, z / Chunk::SIZE_Z)].insert(
            sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)));
    });
}

void SkyHeightMap::request(unsigned chunkX, unsigned chunkZ) {
    const auto key = columnKey(chunkX, chunkZ);
    if (entries.contains(key)) return;
    if (!chunks.isRegistered({chunkX, 0, chunkZ})) return;
    entries.emplace(key, Entry{});
    queue.insert({chunkX, 0, chunkZ});
}

void SkyHeightMap::invalidate(uint64_t key) {
    if (!entries.contains(key)) return;
    queue.insert({unsigned(key >> 32), 0, unsigned(key)});
}

void SkyHeightMap::setOverrides(Overrides value) {
    overridden = std::move(value);
    for (auto& [key, entry] : entries) refreshEffective(key, entry);
}

void SkyHeightMap::update() {
    // Wood grows with the day the chunks generate for.
    if (const int32_t today = chunks.woodDay(); today != day) {
        day = today;
        for (const auto& [key, entry] : entries)
            if (entry.hasTrees) invalidate(key);
    }
    collect();
    dispatch();
    observe();
}

void SkyHeightMap::dispatch() {
    if (queue.empty() || jobs.size() >= MaxJobs) return;
    queue.setPeriodX(chunks.wrap().tiles);
    for (const auto& coord : queue.ordered(focus, false)) {
        if (jobs.size() >= MaxJobs) break;
        const auto key = columnKey(coord.x, coord.z);
        const auto entry = entries.find(key);
        if (entry == entries.end()) {
            queue.erase(coord);
            continue;
        }
        // Waits (stays queued) until terrain and trees are ready.
        const auto sources = chunks.columnSources(coord.x, coord.z);
        if (!sources || !sources->terrain) continue;
        queue.erase(coord);
        auto& e = entry->second;
        e.trees = sources->trees;
        e.edit = edits.find(key);
        e.day = day;
        e.generation = ++nextGeneration;
        e.hasTrees = e.trees && !e.trees->trees.empty();
        BuildInputs inputs{sources->terrain,      e.trees, e.edit,  opacity,
                           &chunks.treeSpecies(), coord.x, coord.z, day};
        jobs.push_back(
            {key, e.generation,
             pool.submit_task(
                 [inputs = std::move(inputs)] { return buildTile(inputs); },
                 BS::pr::normal)});
    }
}

void SkyHeightMap::collect() {
    using namespace std::chrono_literals;
    for (size_t i = 0; i < jobs.size();) {
        if (jobs[i].future.wait_for(0s) != std::future_status::ready) {
            ++i;
            continue;
        }
        auto job = std::move(jobs[i]);
        jobs.erase(jobs.begin() + ptrdiff_t(i));
        auto tile = job.future.get();
        const auto entry = entries.find(job.key);
        if (entry == entries.end() ||
            entry->second.generation != job.generation)
            continue;
        entry->second.derived =
            std::make_shared<const sky::Tile>(std::move(tile));
        // Overrides the sources now agree with are dropped.
        if (const auto it = overridden.find(job.key); it != overridden.end()) {
            for (auto o = it->second.begin(); o != it->second.end();)
                if (entry->second.derived->column(o->first) == o->second) {
                    o = it->second.erase(o);
                    dirty = true;
                } else {
                    ++o;
                }
            if (it->second.empty()) overridden.erase(it);
        }
        refreshEffective(job.key, entry->second);
    }
}

void SkyHeightMap::refreshEffective(uint64_t key, Entry& entry) {
    if (!entry.derived) return;
    const auto it = overridden.find(key);
    if (it == overridden.end()) {
        entry.effective = entry.derived;
    } else {
        auto tile = std::make_shared<sky::Tile>(*entry.derived);
        for (const auto& [index, column] : it->second) tile->set(index, column);
        entry.effective = std::move(tile);
    }
    changed.emit(key);
}

sky::Column SkyHeightMap::scanLoaded(unsigned cx, unsigned cz, int lx, int lz,
                                     const sky::Column& known) {
    const int from =
        std::min(WORLD_HEIGHT_Y - 1,
                 std::max(known.top(), chunks.getColumnTopY(cx, cz)));
    // Opacity of the loaded slab being scanned, top-down; unloaded slabs
    // keep what the map knew there.
    int cachedSlab = -1;
    bool loaded = false;
    std::array<uint8_t, Chunk::SIZE_Y> slab{};
    return sky::scanColumn(from, 0, [&](int y) -> int {
        const int s = y / Chunk::SIZE_Y;
        if (s != cachedSlab) {
            cachedSlab = s;
            loaded = chunks.withChunkLocked(
                {cx, unsigned(s), cz}, [&](const Chunk& chunk) {
                    for (int ly = 0; ly < Chunk::SIZE_Y; ++ly)
                        slab[size_t(ly)] =
                            tileOpacity(chunk.tiles[lz][ly][lx], blocks);
                });
        }
        return loaded ? slab[size_t(y % Chunk::SIZE_Y)] : known.opacityAt(y);
    });
}

void SkyHeightMap::observe() {
    for (auto it = observations.begin(); it != observations.end();) {
        const auto key = it->first;
        const auto entry = entries.find(key);
        // Not built yet: observed once it is (the column may also be
        // outside the requested area; then only loaded overrides change).
        if (entry == entries.end() || !entry->second.derived) {
            if (entry == entries.end() &&
                !chunks.isRegistered({unsigned(key >> 32), 0, unsigned(key)}))
                it = observations.erase(it);
            else
                ++it;
            continue;
        }
        const auto cx = unsigned(key >> 32), cz = unsigned(key);
        bool any = false;
        for (const int index : it->second) {
            const auto& derived = *entry->second.derived;
            const auto before = entry->second.effective->column(index);
            const auto now = scanLoaded(cx, cz, index % Chunk::SIZE_X,
                                        index / Chunk::SIZE_X, before);
            if (now == before) continue;
            auto& columns = overridden[key];
            if (now == derived.column(index))
                columns.erase(index);
            else
                columns[index] = now;
            if (columns.empty()) overridden.erase(key);
            dirty = true;
            any = true;
        }
        if (any) refreshEffective(key, entry->second);
        it = observations.erase(it);
    }
}

sky::TilePtr SkyHeightMap::tile(unsigned chunkX, unsigned chunkZ) const {
    const auto it = entries.find(columnKey(chunkX, chunkZ));
    return it == entries.end() ? nullptr : it->second.effective;
}

std::optional<sky::Column> SkyHeightMap::column(int64_t x, int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0) return std::nullopt;
    const auto t =
        tile(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (!t) return std::nullopt;
    return t->column(
        sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)));
}

std::optional<int> SkyHeightMap::top(int64_t x, int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0) return std::nullopt;
    const auto t =
        tile(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (!t) return std::nullopt;
    return t->top(
        sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)));
}

std::optional<uint8_t> SkyHeightMap::directSky(int64_t x, int y,
                                               int64_t z) const {
    x = chunks.wrap().wrapTile(x);
    if (x < 0 || z < 0) return std::nullopt;
    const auto t =
        tile(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (!t) return std::nullopt;
    return t->direct(
        sky::Tile::index(int(x % Chunk::SIZE_X), int(z % Chunk::SIZE_Z)), y);
}

bool SkyHeightMap::isIdle() const {
    return queue.empty() && jobs.empty() && observations.empty();
}
