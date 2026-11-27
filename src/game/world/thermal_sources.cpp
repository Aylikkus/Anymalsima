#include "thermal_sources.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "world_wrap.hpp"

namespace {
uint64_t mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}
}  // namespace

uint64_t ThermalSourceRegistry::blockKey(TilePos tile) {
    const uint64_t packed =
        uint64_t(uint32_t(tile.x)) | (uint64_t(uint32_t(tile.z)) << 32);
    return mix64(packed ^ (uint64_t(uint16_t(tile.y)) << 17)) | (1ull << 63);
}

void ThermalSourceRegistry::link(Id id, const ThermalSource& source) {
    buckets[bucketKey(bucketOf(source.position[0]),
                      bucketOf(source.position[2]))]
        .push_back(id);
    if (source.owner == ThermalSource::Owner::Block)
        byTile[source.tile] = id;
    else
        items.insert(id);
    if (source.key) ++keyCounts[source.key];
    ++revision;
}

void ThermalSourceRegistry::unlink(Id id, const ThermalSource& source) {
    const auto key =
        bucketKey(bucketOf(source.position[0]), bucketOf(source.position[2]));
    if (auto it = buckets.find(key); it != buckets.end()) {
        auto& ids = it->second;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) buckets.erase(it);
    }
    if (source.owner == ThermalSource::Owner::Block)
        byTile.erase(source.tile);
    else
        items.erase(id);
    if (source.key) {
        const auto it = keyCounts.find(source.key);
        if (it != keyCounts.end() && --it->second == 0) keyCounts.erase(it);
    }
    ++revision;
}

ThermalSourceRegistry::Id ThermalSourceRegistry::add(ThermalSource source) {
    const Id id = next++;
    if (source.key == 0) source.key = id;
    // A thermostat starts at zero output until the room simulation measures
    // its room; a fixed source is simply its configured offset.
    source.commandDeltaC =
        source.emission.thermostat() ? 0.f : source.emission.deltaC;
    source.thermostatSaturated = false;
    const auto& stored = sources.emplace(id, std::move(source)).first->second;
    link(id, stored);
    if (stored.owner == ThermalSource::Owner::Item) {
        dirty = true;
        itemChanged.emit(stored);
    }
    return id;
}

void ThermalSourceRegistry::remove(Id id) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    const ThermalSource removed = std::move(it->second);
    sources.erase(it);
    unlink(id, removed);
    if (removed.owner == ThermalSource::Owner::Item) {
        dirty = true;
        itemChanged.emit(removed);
    }
}

void ThermalSourceRegistry::place(Id id, std::array<double, 3> position) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    auto& source = it->second;
    if (source.position == position) return;
    const ThermalSource before = source;
    unlink(id, source);
    source.position = position;
    link(id, source);
    if (source.owner == ThermalSource::Owner::Item) {
        dirty = true;
        itemChanged.emit(before);
        itemChanged.emit(source);
    }
}

const ThermalSource* ThermalSourceRegistry::find(Id id) const {
    const auto it = sources.find(id);
    return it == sources.end() ? nullptr : &it->second;
}

void ThermalSourceRegistry::setCommand(Id id, float deltaC, bool saturated) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    auto& source = it->second;
    if (!source.emission.thermostat()) return;
    if (source.commandDeltaC == deltaC &&
        source.thermostatSaturated == saturated)
        return;
    source.commandDeltaC = deltaC;
    source.thermostatSaturated = saturated;
    ++revision;
}

void ThermalSourceRegistry::setBlockThermal(TilePos tile, uint8_t block,
                                            const ThermalEmission& emission) {
    if (!emission.emits()) {
        removeBlockThermal(tile);
        return;
    }
    if (const auto it = byTile.find(tile); it != byTile.end()) {
        auto& source = sources.at(it->second);
        if (source.block == block &&
            source.emission.deltaC == emission.deltaC &&
            source.emission.radius == emission.radius &&
            source.emission.falloff == emission.falloff &&
            source.emission.mode == emission.mode &&
            source.emission.targetC == emission.targetC &&
            source.emission.deadbandC == emission.deadbandC &&
            source.emission.gain == emission.gain &&
            source.emission.minDeltaC == emission.minDeltaC &&
            source.emission.maxDeltaC == emission.maxDeltaC)
            return;
        source.block = block;
        source.emission = emission;
        source.commandDeltaC = emission.thermostat() ? 0.f : emission.deltaC;
        source.thermostatSaturated = false;
        source.key = blockKey(tile);
        ++revision;
        return;
    }
    ThermalSource source;
    source.owner = ThermalSource::Owner::Block;
    source.tile = tile;
    source.block = block;
    source.emission = emission;
    source.position = {tile.x + 0.5, tile.y + 0.5, tile.z + 0.5};
    source.commandDeltaC = emission.thermostat() ? 0.f : emission.deltaC;
    source.key = blockKey(tile);
    add(std::move(source));
}

void ThermalSourceRegistry::removeBlockThermal(TilePos tile) {
    const auto it = byTile.find(tile);
    if (it != byTile.end()) remove(it->second);
}

const ThermalSource* ThermalSourceRegistry::blockThermal(TilePos tile) const {
    const auto it = byTile.find(tile);
    return it == byTile.end() ? nullptr : find(it->second);
}

std::vector<TilePos> ThermalSourceRegistry::blockThermalsIn(TilePos min,
                                                            TilePos max) const {
    std::vector<TilePos> result;
    for (int64_t bz = bucketOf(min.z); bz <= bucketOf(max.z - 1); ++bz)
        for (int64_t bx = bucketOf(min.x); bx <= bucketOf(max.x - 1); ++bx) {
            const auto it = buckets.find(bucketKey(bx, bz));
            if (it == buckets.end()) continue;
            for (const Id id : it->second) {
                const auto& source = sources.at(id);
                if (source.owner != ThermalSource::Owner::Block) continue;
                const auto& tile = source.tile;
                if (tile.x >= min.x && tile.x < max.x && tile.y >= min.y &&
                    tile.y < max.y && tile.z >= min.z && tile.z < max.z)
                    result.push_back(tile);
            }
        }
    return result;
}

void ThermalSourceRegistry::forEachNear(
    double x, double z, double range, int periodX,
    const std::function<void(Id, const ThermalSource&)>& fn) const {
    const int64_t span = int64_t(std::ceil(range / BucketSize)) + 1;
    const int64_t periodBuckets =
        periodX > 0 ? std::max<int64_t>(1, periodX / BucketSize) : 0;
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
