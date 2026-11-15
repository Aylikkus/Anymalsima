#include "surface_sampler.hpp"

const SurfaceSampler::RegionInput& SurfaceSampler::input(int x, int z) {
    const unsigned rx = std::clamp(x, 0, width() - 1) / TerrainTuning::RegionX;
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
    x = std::clamp(x, 0, width() - 1);
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
    x = std::clamp(x, 0, width() - 1);
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
    x = std::clamp(x, 0, width() - 1);
    z = std::clamp(z, 0, depth() - 1);
    const int i = ColumnEdit::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z);
    result.waterY = edit->waterY[i];
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
    x = std::clamp(x, 0, width() - 1);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (auto it = columns.find(key); it != columns.end()) return it->second;
    if (columns.size() > 8192) columns.clear();
    const auto& data = input(x, z);
    TerrainSampler sampler(data.terrain);
    const auto terrain = sampler.sample(x, z);
    SurfaceColumn result;
    result.surfaceY = terrain.surface_y;
    result.waterY = result.surfaceY < SEA_LEVEL_Y ? SEA_LEVEL_Y + 1 : -1;
    result.soil = sampler.soil(x, z, terrain);
    result.material = material(data, result, result.surfaceY);
    auto rockOnly = result;
    rockOnly.soil = {};
    result.rockMaterial = material(data, rockOnly, result.surfaceY);
    columns[key] = result;
    return result;
}

uint16_t SurfaceSampler::materialAt(int x, int y, int z) {
    const auto value = generatedColumn(x, z);
    return material(input(x, z), value, y);
}
