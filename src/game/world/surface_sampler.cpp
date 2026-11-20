#include "surface_sampler.hpp"

#include "generated_hydrology.hpp"
#include "ground_cover_climate.hpp"
#include "items/ground_cover.hpp"

const SurfaceSampler::RegionInput& SurfaceSampler::input(int x, int z) {
    const unsigned rx = wrapX(x) / TerrainTuning::RegionX;
    const unsigned rz = std::clamp(z, 0, depth() - 1) / TerrainTuning::RegionZ;
    const uint64_t key = (uint64_t(rx) << 32) | rz;
    if (auto it = regions.find(key); it != regions.end()) return *it->second;
    // Large fallback meshes may touch many regions. Bound this worker-local
    // cache; samples and results never retain references into it.
    if (regions.size() >= 64) regions.clear();
    const auto make = [&] {
        return RegionInput{
            TerrainNeighborhood::capture(world, {rx, rz}, assets),
            layers.get(world.regions[rz * world.width + rx])};
    };
    auto value = shared ? shared->get(key, make)
                        : std::make_shared<const RegionInput>(make());
    return *regions.emplace(key, std::move(value)).first->second;
}

int SurfaceSampler::height(int x, int z) {
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (auto it = heights.find(key); it != heights.end()) return it->second;
    if (heights.size() > 65536) heights.clear();
    const auto* edit = editAt(x, z);
    const int value =
        edit ? edit->groundY[ColumnEdit::index(x % Chunk::SIZE_X,
                                               z % Chunk::SIZE_Z)]
             : int(std::floor(
                   TerrainSampler(input(x, z).terrain).surface(x, z)));
    heights[key] = value;
    return value;
}

const ColumnEdit* SurfaceSampler::editAt(int x, int z) {
    if (!edits || edits->empty()) return nullptr;
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key =
        editKey(unsigned(x / Chunk::SIZE_X), unsigned(z / Chunk::SIZE_Z));
    if (key != lastEditKey) {
        const auto it = edits->find(key);
        lastEditKey = key;
        lastEdit = it == edits->end() ? nullptr : it->second.get();
    }
    return lastEdit;
}

CoverTexel SurfaceSampler::cover(int x, int z) {
    const auto column = generatedColumn(x, z);
    if (!column.soil.cover || !assets.hasStorage<GroundCover>()) return {};
    if (const auto* edit = editAt(x, z)) {
        const int wx = wrapX(x), wz = std::clamp(z, 0, depth() - 1);
        if (edit->groundY[ColumnEdit::index(
                wx % Chunk::SIZE_X, wz % Chunk::SIZE_Z)] != column.surfaceY)
            return {};
    }
    const auto& definition =
        assets.getStorage<GroundCover>().get(column.soil.cover);
    const auto layer =
        assets.getMaterialAlbedoLayer(definition.overlay_material);
    if (layer >= 255) return {};
    const auto [humidity, temperature] =
        ground_cover_climate::at(world, wrapX(x) + 0.5, z + 0.5);
    return {uint8_t(layer + 1), uint8_t(column.soil.coverDensity * 17),
            ground_cover_climate::packRgb565(
                definition.tint_gradient.at(humidity, temperature))};
}

int SurfaceSampler::structureTop(int x, int z) {
    const auto* edit = editAt(x, z);
    return edit ? edit->structureTopY() : -1;
}

void SurfaceSampler::setReservoirs(
    std::shared_ptr<const ReservoirSnapshot> value) {
    reservoirs = std::move(value);
    waterSamples.clear();
}

void SurfaceSampler::setEdits(std::shared_ptr<const EditMap> value) {
    edits = std::move(value);
    lastEditKey = UINT64_MAX;
    lastEdit = nullptr;
    // Generated columns stay valid; heights include edits.
    heights.clear();
    waterSamples.clear();
}

uint16_t SurfaceSampler::material(const RegionInput& data,
                                  const SurfaceColumn& column, int y) const {
    const auto& region =
        data.terrain.at(data.terrain.center.x, data.terrain.center.y).region;
    const RegionLayer* selected = &data.rocks.back();
    // Beaches may rise above a water region's ceiling.
    const int rockDepth = std::max(0, region.land_height_m - y);
    for (const auto& layer : data.rocks)
        if (rockDepth >= layer.depth_from_surface_m &&
            rockDepth < layer.depth_from_surface_m + layer.thickness_m) {
            selected = &layer;
            break;
        }
    const int soilDepth = column.surfaceY - y;
    for (int i = 0; i < column.soil.count; ++i) {
        const auto& layer = column.soil.layers[i];
        if (soilDepth >= layer.depth_from_surface_m &&
            soilDepth < layer.depth_from_surface_m + layer.thickness_m) {
            selected = &layer;
            break;
        }
    }
    return assets.getMaterialAlbedoLayer(
        assets.getStorage<Block>().get(selected->block_id).material);
}

SurfaceColumn SurfaceSampler::column(int x, int z) {
    auto result = generatedColumn(x, z);
    const auto* edit = editAt(x, z);
    result.water = waterAt(x, z);
    if (!edit) return result;
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const int i = ColumnEdit::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z);
    // Unchanged tiles keep generated soil and rock materials; a replaced
    // block at the original height is not shown.
    if (edit->groundY[i] != result.surfaceY) {
        result.surfaceY = edit->groundY[i];
        if (const auto block = edit->groundBlock[i])
            result.material = result.rockMaterial =
                assets.getMaterialAlbedoLayer(
                    assets.getStorage<Block>().get(block).material);
    }
    return result;
}

SurfaceColumn SurfaceSampler::generatedColumn(int x, int z) {
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (auto it = columns.find(key); it != columns.end()) return it->second;
    if (columns.size() > 8192) columns.clear();
    const auto& data = input(x, z);
    TerrainSampler sampler(data.terrain);
    const auto terrain = sampler.sample(x, z);
    SurfaceColumn result;
    result.surfaceY = terrain.surface_y;
    result.water = generatedWater(x, z, result.surfaceY);
    result.soil = sampler.soil(x, z, terrain, result.water);
    result.material = material(data, result, result.surfaceY);
    // The smooth LOD uses one side material per column. Use the sediment
    // below the surface here; stepped LODs sample the actual layers by Y.
    if (result.soil.riverSediment) {
        result.rockMaterial =
            material(data, result, result.surfaceY - result.soil.depth_m + 1);
    } else {
        auto rockOnly = result;
        rockOnly.soil = {};
        result.rockMaterial = material(data, rockOnly, result.surfaceY);
    }
    columns[key] = result;
    return result;
}

bool SurfaceSampler::hasEdits(int x0, int z0, int x1, int z1) {
    if (!edits || edits->empty()) return false;
    for (int z = z0 / Chunk::SIZE_Z; z <= (z1 - 1) / Chunk::SIZE_Z; ++z)
        for (int x = x0 / Chunk::SIZE_X; x <= (x1 - 1) / Chunk::SIZE_X; ++x)
            if (editAt(x * Chunk::SIZE_X, z * Chunk::SIZE_Z)) return true;
    return false;
}

int SurfaceSampler::channelFloorLevel(int x, int z, int radius) const {
    return world.hydrology ? world.hydrology->channelFloorLevel(x, z, radius)
                           : -1;
}

bool SurfaceSampler::hasChannel(int x0, int z0, int x1, int z1) const {
    return world.hydrology && world.hydrology->hasChannel(x0, z0, x1, z1);
}

bool SurfaceSampler::hasUnrefinedWater(int x0, int z0, int x1, int z1) const {
    return provisionalWater() &&
           world.hydrology->needsRefinement(x0, z0, x1, z1);
}

bool SurfaceSampler::submerged(int x, int z, float y) {
    if (!editAt(x, z) && (!reservoirs || reservoirs->cells.empty())) {
        const int ceiling = world.hydrology
                                ? world.hydrology->waterCeilingAt(x, z)
                                : WaterSurface::SeaBoundary;
        if (y >= ceiling) return false;
    }
    return waterAt(x, z).submerged(y, height(x, z));
}

bool SurfaceSampler::uniformWater(int x0, int z0, int x1, int z1,
                                  WaterSurface sample) {
    if (hasChannel(x0, z0, x1, z1) || hasEdits(x0, z0, x1, z1)) return false;
    if (reservoirs) {
        if (std::any_of(reservoirs->localTops.begin(),
                        reservoirs->localTops.end(), [&](const auto& local) {
                            const auto x = int(local.first >> 32),
                                       z = int(uint32_t(local.first));
                            return (x >= x0 && x < x1) && (z >= z0 && z < z1);
                        }))
            return false;
        if (world.hydrology)
            for (const auto& [body, level] : reservoirs->levels)
                if (world.hydrology->mayContainBody(body, x0, z0, x1, z1)) {
                    if (body == GeneratedHydrology::Sea &&
                        sample.body == body &&
                        world.hydrology->uniformLoweredSea(x0, z0, x1, z1,
                                                           level))
                        continue;
                    return false;
                }
    }
    if (world.hydrology && waterDetail != WaterDetail::Preview)
        return world.hydrology->uniformWater(x0, z0, x1, z1, sample.body,
                                             provisionalWater());
    for (int x : {x0, (x0 + x1) / 2, x1 - 1})
        for (int z : {z0, (z0 + z1) / 2, z1 - 1})
            if (waterAt(x, z).surfaceY != sample.surfaceY) return false;
    return true;
}

WaterSurface SurfaceSampler::waterAt(int x, int z) {
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (auto it = waterSamples.find(key); it != waterSamples.end())
        return it->second;
    const bool needsHeight = !world.hydrology ||
                             world.hydrology->riverAt(x, z) ||
                             (waterDetail == WaterDetail::Preview &&
                              world.hydrology->potentialSeaAt(x, z));
    auto result = generatedWater(x, z, needsHeight ? height(x, z) : -1);
    if (reservoirs && (reservoirs->levels.contains(result.body) ||
                       reservoirs->localTop(x, z) >= 0)) {
        const int ground = height(x, z);
        // A provisional coastal sample does not prove membership as the
        // ocean recedes. Resolve the actual baseline before applying a level.
        if (provisionalWater() && world.hydrology &&
            reservoirs->levels.contains(result.body))
            result = world.hydrology->waterAt(x, z, ground);
        result =
            reservoirs->project(world.hydrology.get(), x, z, ground, result);
    }
    if (const auto* edit = editAt(x, z)) {
        const int i = ColumnEdit::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z);
        result = edit->waterAt(i, result);
    }
    if (waterSamples.size() >= 65536) waterSamples.clear();
    waterSamples.emplace(key, result);
    return result;
}

WaterSurface SurfaceSampler::generatedWater(int x, int z, int groundY) const {
    if (!world.hydrology) return WaterSurface::sea(groundY);
    if (!provisionalWater()) return world.hydrology->waterAt(x, z, groundY);
    if (waterDetail == WaterDetail::Preview)
        return world.hydrology->previewWaterAt(x, z, groundY);
    // Include the exact interface with a standing body at river mouths,
    // even while the wider shoreline of a fallback patch is pending.
    if (const auto channel = world.hydrology->riverAt(x, z);
        channel && channel->distance <= channel->radius)
        return world.hydrology->waterAt(x, z, groundY);
    return world.hydrology->certifiedSeaAt(x, z);
}

uint16_t SurfaceSampler::materialAt(int x, int y, int z) {
    const auto value = generatedColumn(x, z);
    return material(input(x, z), value, y);
}
