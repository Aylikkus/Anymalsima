#include "light_sources.hpp"

#include <algorithm>
#include <cmath>

#include "world_wrap.hpp"

LightSourceRegistry::Id LightSourceRegistry::add(LightSource source) {
    const Id id = next++;
    const auto& stored = sources.emplace(id, std::move(source)).first->second;
    if (stored.owner == LightSource::Owner::Block) {
        byTile[stored.tile] = id;
        dirty = true;
    } else {
        entityChanged.emit(stored);
    }
    return id;
}

void LightSourceRegistry::remove(Id id) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    const LightSource removed = std::move(it->second);
    sources.erase(it);
    if (removed.owner == LightSource::Owner::Block) {
        byTile.erase(removed.tile);
        dirty = true;
    } else {
        entityChanged.emit(removed);
    }
}

void LightSourceRegistry::place(Id id, std::array<double, 3> position,
                                std::array<float, 3> direction) {
    const auto it = sources.find(id);
    if (it == sources.end()) return;
    auto& source = it->second;
    const LightSource before = source;
    source.position = position;
    source.direction = direction;
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
                                        const LightEmission& emission) {
    if (!emission.isFar()) {
        removeBlockLight(tile);
        return;
    }
    if (const auto it = byTile.find(tile); it != byTile.end()) {
        auto& source = sources.at(it->second);
        if (source.block == block) return;
        source.block = block;
        source.emission = emission;
        dirty = true;
        return;
    }
    LightSource source;
    source.owner = LightSource::Owner::Block;
    source.tile = tile;
    source.block = block;
    source.emission = emission;
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
    for (const auto& [tile, id] : byTile)
        if (tile.x >= min.x && tile.x < max.x && tile.y >= min.y &&
            tile.y < max.y && tile.z >= min.z && tile.z < max.z)
            result.push_back(tile);
    return result;
}

std::vector<FarLightView> visibleFarLights(const LightSourceRegistry& registry,
                                           std::array<double, 3> eye,
                                           double viewRange, int periodX) {
    std::vector<FarLightView> result;
    registry.forEach([&](LightSourceRegistry::Id, const LightSource& source) {
        if (!source.emission.isFar()) return;
        FarLightView view;
        view.source = &source;
        view.position = source.position;
        view.position[0] =
            eye[0] + WorldWrap::deltaf(source.position[0], eye[0], periodX);
        const double dx = view.position[0] - eye[0],
                     dy = view.position[1] - eye[1],
                     dz = view.position[2] - eye[2];
        view.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (view.distance - source.emission.radius >= viewRange) return;
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
