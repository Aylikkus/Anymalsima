#include "surface_sampler.hpp"

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

void SurfaceSampler::setEdits(std::shared_ptr<const EditMap> value) {
    edits = std::move(value);
    lastEditKey = UINT64_MAX;
    lastEdit = nullptr;
    // Generated columns stay valid; heights include edits.
    heights.clear();
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
    if (!edit) return result;
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const int i = ColumnEdit::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z);
    result.water = waterAt(x, z);
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
    result.water = WaterSurface::sea(result.surfaceY);
    result.soil = sampler.soil(x, z, terrain);
    result.material = material(data, result, result.surfaceY);
    auto rockOnly = result;
    rockOnly.soil = {};
    result.rockMaterial = material(data, rockOnly, result.surfaceY);
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

WaterSurface SurfaceSampler::waterAt(int x, int z) {
    if (const auto* edit = editAt(x, z)) {
        const int i =
            ColumnEdit::index(wrapX(x) % Chunk::SIZE_X,
                              std::clamp(z, 0, depth() - 1) % Chunk::SIZE_Z);
        return {edit->waterY[i] > edit->groundY[i] + 1 ? edit->waterY[i] : -1};
    }
    return WaterSurface::sea(height(x, z));
}

uint16_t SurfaceSampler::materialAt(int x, int y, int z) {
    const auto value = generatedColumn(x, z);
    return material(input(x, z), value, y);
}
