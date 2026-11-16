#include "light_sources.hpp"

#include <algorithm>
#include <cmath>

#include "world_wrap.hpp"

uint8_t LightSource::frontFaces(const LightEmission& emission,
                                const std::array<float, 3>& d) {
    if (!emission.cone) return AllFaces;
    const float dots[6] = {d[0], -d[0], d[1], -d[1], d[2], -d[2]};
    uint8_t mask = 0;
    for (int i = 0; i < 6; ++i)
        if (dots[i] > 1e-4f) mask |= uint8_t(1u << i);
    return mask;
}

void LightSourceRegistry::link(Id id, const LightSource& source) {
    buckets[bucketKey(bucketOf(source.position[0]),
                      bucketOf(source.position[2]))]
        .push_back(id);
    if (source.owner == LightSource::Owner::Block)
        byTile[source.tile] = id;
    else
        entities.insert(id);
    ++revision;
}

void LightSourceRegistry::unlink(Id id, const LightSource& source) {
    const auto key =
        bucketKey(bucketOf(source.position[0]), bucketOf(source.position[2]));
    if (auto it = buckets.find(key); it != buckets.end()) {
        auto& ids = it->second;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) buckets.erase(it);
    }
    if (source.owner == LightSource::Owner::Block)
        byTile.erase(source.tile);
    else
        entities.erase(id);
    ++revision;
}

LightSourceRegistry::Id LightSourceRegistry::add(LightSource source) {
    const Id id = next++;
    const auto& stored = sources.emplace(id, std::move(source)).first->second;
    link(id, stored);
    if (stored.owner == LightSource::Owner::Block)
        dirty = true;
    else
        entityChanged.emit(stored);
    return id;
}

void LightSourceRegistry::remove(Id id) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    const LightSource removed = std::move(it->second);
    sources.erase(it);
    unlink(id, removed);
    if (removed.owner == LightSource::Owner::Block)
        dirty = true;
    else
        entityChanged.emit(removed);
}

void LightSourceRegistry::place(Id id, std::array<double, 3> position,
                                std::array<float, 3> direction) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    auto& source = it->second;
    const LightSource before = source;
    unlink(id, source);
    source.position = position;
    source.direction = direction;
    link(id, source);
    if (source.owner == LightSource::Owner::Entity) {
        entityChanged.emit(before);
        entityChanged.emit(source);
    }
}

const LightSource* LightSourceRegistry::find(Id id) const {
    const auto it = sources.find(id);
    return it == sources.end() ? nullptr : &it->second;
}

void LightSourceRegistry::setBlockLight(TilePos tile, uint8_t block,
                                        const LightEmission& emission,
                                        uint8_t openFaces) {
    if (!emission.emits()) {
        removeBlockLight(tile);
        return;
    }
    if (const auto it = byTile.find(tile); it != byTile.end()) {
        auto& source = sources.at(it->second);
        if (source.block == block && source.openFaces == openFaces) return;
        source.block = block;
        source.emission = emission;
        source.openFaces = openFaces;
        source.direction = emission.cone ? emission.cone->direction
                                         : std::array<float, 3>{0, -1, 0};
        dirty = true;
        ++revision;
        return;
    }
    LightSource source;
    source.owner = LightSource::Owner::Block;
    source.tile = tile;
    source.block = block;
    source.emission = emission;
    source.openFaces = openFaces;
    source.position = {tile.x + 0.5, tile.y + 0.5, tile.z + 0.5};
    if (emission.cone) source.direction = emission.cone->direction;
    add(std::move(source));
}

void LightSourceRegistry::removeBlockLight(TilePos tile) {
    const auto it = byTile.find(tile);
    if (it != byTile.end()) remove(it->second);
}

const LightSource* LightSourceRegistry::blockLight(TilePos tile) const {
    const auto it = byTile.find(tile);
    return it == byTile.end() ? nullptr : find(it->second);
}

std::vector<TilePos> LightSourceRegistry::blockLightsIn(TilePos min,
                                                        TilePos max) const {
    std::vector<TilePos> result;
    for (int64_t bz = bucketOf(min.z); bz <= bucketOf(max.z - 1); ++bz)
        for (int64_t bx = bucketOf(min.x); bx <= bucketOf(max.x - 1); ++bx) {
            const auto it = buckets.find(bucketKey(bx, bz));
            if (it == buckets.end()) continue;
            for (const Id id : it->second) {
                const auto& s = sources.at(id);
                if (s.owner != LightSource::Owner::Block) continue;
                const auto& t = s.tile;
                if (t.x >= min.x && t.x < max.x && t.y >= min.y &&
                    t.y < max.y && t.z >= min.z && t.z < max.z)
                    result.push_back(t);
            }
        }
    return result;
}

void LightSourceRegistry::forEachNear(
    double x, double z, double range, int periodX,
    const std::function<void(Id, const LightSource&)>& fn) const {
    const int64_t span = int64_t(std::ceil(range / BucketSize)) + 1;
    const int64_t periodBuckets = periodX > 0 ? periodX / BucketSize : 0;
    // Few buckets in total: walking them all is cheaper than the window.
    if (buckets.size() <= size_t((2 * span + 1) * (2 * span + 1))) {
        for (const auto& [key, ids] : buckets)
            for (const Id id : ids) fn(id, sources.at(id));
        return;
    }
    const int64_t cx = bucketOf(x), cz = bucketOf(z);
    int64_t width = 2 * span + 1;
    if (periodBuckets > 0) width = std::min(width, periodBuckets);
    for (int64_t bz = cz - span; bz <= cz + span; ++bz)
        for (int64_t i = 0; i < width; ++i) {
            int64_t bx = cx - span + i;
            if (periodBuckets > 0) bx = WorldWrap::mod(bx, periodBuckets);
            const auto it = buckets.find(bucketKey(bx, bz));
            if (it == buckets.end()) continue;
            for (const Id id : it->second) fn(id, sources.at(id));
        }
}

std::vector<FarLightView> visibleFarLights(const LightSourceRegistry& registry,
                                           std::array<double, 3> eye,
                                           double viewRange, int periodX) {
    std::vector<FarLightView> result;
    // Far lights are rare; the radius can be anything up to 8192, so the
    // whole registry is scanned rather than a bucket window.
    registry.forEach(
        [&](LightSourceRegistry::Id id, const LightSource& source) {
            if (!source.emission.isFar() || source.sealed()) return;
            FarLightView view;
            view.source = &source;
            view.id = id;
            view.position = source.position;
            view.position[0] =
                eye[0] + WorldWrap::deltaf(source.position[0], eye[0], periodX);
            const double dx = view.position[0] - eye[0],
                         dy = view.position[1] - eye[1],
                         dz = view.position[2] - eye[2];
            view.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            // Lights beyond the view range have no loaded surroundings to light
            // or shadow.
            if (view.distance >= viewRange) return;
            result.push_back(view);
        });
    auto reach = [](const FarLightView& v) {
        return std::max(0.0, v.distance - v.source->emission.radius);
    };
    std::sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        const double ra = reach(a), rb = reach(b);
        return ra != rb ? ra < rb : a.distance < b.distance;
    });
    return result;
}
