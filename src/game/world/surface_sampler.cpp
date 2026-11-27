#include "surface_sampler.hpp"

#include "generated_hydrology.hpp"
#include "ground_cover_climate.hpp"
#include "items/ground_cover.hpp"

const SurfaceSampler::RegionInput& SurfaceSampler::input(int x, int z) {
    const uint64_t key = regionKey(x, z);
    if (auto it = regions.find(key); it != regions.end()) return *it->second;
    // Large fallback meshes may touch many regions. Bound this worker-local
    // cache; samples and results never retain references into it.
    if (regions.size() >= 64) {
        samplers.clear();
        regions.clear();
    }
    const unsigned rx = wrapX(x) / TerrainTuning::RegionX;
    const unsigned rz = std::clamp(z, 0, depth() - 1) / TerrainTuning::RegionZ;
    const auto make = [&] {
        return RegionInput{
            TerrainNeighborhood::capture(world, {rx, rz}, assets),
            layers.get(world.regions[rz * world.width + rx])};
    };
    auto value = shared ? shared->get(key, make)
                        : std::make_shared<const RegionInput>(make());
    return *regions.emplace(key, std::move(value)).first->second;
}

uint64_t SurfaceSampler::regionKey(int x, int z) const {
    const unsigned rx = wrapX(x) / TerrainTuning::RegionX;
    const unsigned rz = std::clamp(z, 0, depth() - 1) / TerrainTuning::RegionZ;
    return (uint64_t(rx) << 32) | rz;
}

TerrainSampler& SurfaceSampler::samplerFor(int x, int z) {
    const uint64_t key = regionKey(x, z);
    if (auto it = samplers.find(key); it != samplers.end()) return *it->second;
    const auto& data = input(x, z);
    auto value = std::make_unique<TerrainSampler>(data.terrain);
    return *samplers.emplace(key, std::move(value)).first->second;
}

int SurfaceSampler::height(int x, int z) {
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (auto it = heights.find(key); it != heights.end()) return it->second;
    if (heights.size() > 65536) heights.clear();
    const auto* edit = editAt(x, z);
    const int value = edit ? edit->groundY[ColumnEdit::index(x % Chunk::SIZE_X,
                                                             z % Chunk::SIZE_Z)]
                           : samplerFor(x, z).tileSurface(x, z);
    heights[key] = value;
    return value;
}

void SurfaceSampler::setSnow(
    std::shared_ptr<const snow::polar::SurfaceSnapshot> value) {
    snow = std::move(value);
    packHeights.clear();
    waterSamples.clear();
    columns.clear();
    regionalIceTops.clear();
}

float SurfaceSampler::regionalIceTop(int x, int z) {
    if (!snow || !snow->iceBlock) return 0;
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    if (snow->iceTop(x, z)) return 0;
    if (snow->iceCuts && snow->iceCuts->contains({x, z})) return 0;
    const int ground = height(x, z);
    const auto water = generatedWater(x, z, ground);
    if (!water.wet()) return 0;
    if (!snow->iceTargets || snow->iceTargetWidth <= 0 ||
        snow->iceTargetHeight <= 0)
        return 0;
    constexpr double regionSize = WorldWrap::RegionSize;
    const double fx = (x + .5) / regionSize - .5;
    const double fz = (z + .5) / regionSize - .5;
    const int gx = int(std::floor(fx)), gz = int(std::floor(fz));
    const double tx = fx - gx, tz = fz - gz;
    double target = 0;
    for (int dz = 0; dz < 2; ++dz) {
        const int rz = std::clamp(gz + dz, 0, snow->iceTargetHeight - 1);
        for (int dx = 0; dx < 2; ++dx) {
            const int rx = int(WorldWrap::mod(gx + dx, snow->iceTargetWidth));
            const auto& values = *snow->iceTargets;
            target += values[size_t(rz) * snow->iceTargetWidth + rx] *
                      (dx ? tx : 1. - tx) * (dz ? tz : 1. - tz);
        }
    }
    bool feature = false;
    if (snow->iceFeature && snow->iceTargetWidth > 0 &&
        snow->iceTargetHeight > 0) {
        constexpr double regionSize = WorldWrap::RegionSize;
        const int rx = int(WorldWrap::mod(
            int(std::floor((x + .5) / regionSize - .5)), snow->iceTargetWidth));
        const int rz = std::clamp(int(std::floor((z + .5) / regionSize - .5)),
                                  0, snow->iceTargetHeight - 1);
        feature =
            (*snow->iceFeature)[size_t(rz) * snow->iceTargetWidth + rx] != 0;
    }
    const bool sea =
        !water.reach && water.body && water.body <= GeneratedHydrology::Sea;
    if (sea) {
        if (target < .75) return 0;
        return water.height() + (feature ? 1.f : 0.f);
    }
    if (target < .01) return 0;
    return water.height() + (feature ? 1.f : 0.f);
}

float SurfaceSampler::packTop(int x, int z) {
    if (!snow || !snow->block) return 0;
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    double top = 0;
    if (const auto pinned = snow->editedTop(x, z))
        top = double(*pinned) / snow::UnitsPerCubicMetre;
    else if (snow->initialPacks)
        top = seasonalTop(x, z);
    if (const auto loose = snow->looseTop(x, z))
        top = std::max(top, double(*loose) / snow::UnitsPerCubicMetre);
    if (const auto ice = snow->iceTop(x, z))
        top = std::max(top, double(*ice) / snow::UnitsPerCubicMetre);
    else if (const auto regional = regionalIceTop(x, z); regional > 0)
        top = std::max(top, double(regional));
    return float(top);
}

float SurfaceSampler::seasonalTop(int x, int z) {
    if (snow->fixture)
        if (const auto fixture =
                snow::polar::fixturePack(world, *snow->fixture, x, z))
            return float(double(fixture->top) / snow::UnitsPerCubicMetre);
    if (!snow::polar::supported(world)) return 0;

    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (const auto cached = packHeights.find(key); cached != packHeights.end())
        return cached->second;
    const double depth = polar.depth(x, z, snow->day);
    if (depth <= 0) {
        if (packHeights.size() >= 65536) packHeights.clear();
        return packHeights.emplace(key, 0.f).first->second;
    }
    auto& sampler = samplerFor(x, z);
    const auto ground = sampler.sample(x, z);
    const auto shape = sampler.surfaceShape(x, z);
    std::optional<snow::PackRun> pack;
    // The chunk stores its starting pack from the *generated* water. The
    // rendered lake/reservoir projection may temporarily cover a shore cell;
    // using it here made the LOD bare where a loaded chunk had snow.
    if ((shape.code() || ground.slope < 1) &&
        !generatedWater(x, z, ground.surface_y).wet()) {
        ColumnGenerationInput stock;
        stock.surface_y = ground.surface_y;
        stock.slope = ground.slope;
        stock.snowSlope = sampler.snowSlope(x, z);
        stock.groundShape = shape;
        pack = snow::polar::Baselines::run(stock, depth);
    }
    if (packHeights.size() >= 65536) packHeights.clear();
    return packHeights
        .emplace(key, pack ? float(double(snow::projectedPackTop(*pack)) /
                                   snow::UnitsPerCubicMetre)
                           : 0.f)
        .first->second;
}

float SurfaceSampler::exteriorHeight(int x, int z) {
    return std::max(float(height(x, z) + 1), packTop(x, z));
}

TileShape SurfaceSampler::groundShape(int x, int z) {
    return column(x, z).groundShape;
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
    // Any settled snow hides the ground cover, including a centimetre layer
    // that the LOD draws at the ground top rather than as a raised block.
    if (packTop(x, z) > height(x, z) + .005f) return {};
    const auto column = generatedColumn(x, z);
    if (!column.soil.cover || !assets.hasStorage<GroundCover>()) return {};
    if (const auto* edit = editAt(x, z)) {
        const int wx = wrapX(x), wz = std::clamp(z, 0, depth() - 1);
        const int i = ColumnEdit::index(wx % Chunk::SIZE_X, wz % Chunk::SIZE_Z);
        // A shape-only edit clears the generated cover just like a moved
        // ground tile does.
        if (edit->groundY[i] != column.surfaceY ||
            edit->groundShape[i] != column.groundShape)
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
    if (edit) {
        x = wrapX(x);
        z = std::clamp(z, 0, depth() - 1);
        const int i = ColumnEdit::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z);
        const bool heightChanged = edit->groundY[i] != result.surfaceY;
        result.surfaceY = edit->groundY[i];
        result.groundShape = edit->groundShape[i];
        if (const auto block = edit->groundBlock[i]) {
            result.material = assets.getMaterialAlbedoLayer(
                assets.getStorage<Block>().get(block).material);
            if (heightChanged) result.rockMaterial = result.material;
        }
    }
    const float top = packTop(x, z);
    const auto ice = snow
                         ? snow->iceTop(wrapX(x), std::clamp(z, 0, depth() - 1))
                         : std::nullopt;
    const bool iceSurface =
        ice && snow->iceBlock &&
        double(*ice) / snow::UnitsPerCubicMetre >= double(top) - .001;
    if (top > result.surfaceY + 1) {
        result.snowTop = top;
        const auto block = iceSurface ? snow->iceBlock : snow->block;
        result.material = assets.getMaterialAlbedoLayer(
            assets.getStorage<Block>().get(block).material);
    } else if (top > result.surfaceY + .005f) {
        // A layer thinner than one block stays at the terrain height but must
        // still read as snow or ice. Otherwise sparse packs look white nearby
        // and bare in the LOD.
        const auto block = iceSurface ? snow->iceBlock : snow->block;
        result.material = assets.getMaterialAlbedoLayer(
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
    auto& sampler = samplerFor(x, z);
    const auto& data = input(x, z);
    const auto terrain = sampler.sample(x, z);
    SurfaceColumn result;
    result.surfaceY = terrain.surface_y;
    result.water = generatedWater(x, z, result.surfaceY);
    result.soil = sampler.soil(x, z, terrain, result.water);
    result.groundShape = sampler.surfaceShape(x, z);
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
    if (reservoirs && reservoirs->poolIntersects(x0, z0, x1, z1, width()))
        return true;
    return world.hydrology && world.hydrology->hasChannel(x0, z0, x1, z1);
}

bool SurfaceSampler::hasUnrefinedWater(int x0, int z0, int x1, int z1) const {
    return provisionalWater() &&
           world.hydrology->needsRefinement(x0, z0, x1, z1);
}

bool SurfaceSampler::submerged(int x, int z, float y, bool preview) {
    if (!editAt(x, z) && (!reservoirs || (reservoirs->cells.empty() &&
                                          reservoirs->pools.empty()))) {
        const int ceiling = world.hydrology
                                ? world.hydrology->waterCeilingAt(x, z)
                                : WaterSurface::SeaBoundary;
        if (y >= ceiling) return false;
    }
    const auto water = sampleWater(x, z, preview);
    return water.wet() && water.submerged(y, height(x, z));
}

bool SurfaceSampler::uniformWater(int x0, int z0, int x1, int z1,
                                  WaterSurface sample) {
    if (hasChannel(x0, z0, x1, z1) || hasEdits(x0, z0, x1, z1)) return false;
    if (reservoirs) {
        if (reservoirs->cells.intersects(x0, z0, x1, z1)) return false;
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
            if (waterAt(x, z).height() != sample.height()) return false;
    return true;
}

WaterSurface SurfaceSampler::waterAt(int x, int z) {
    return sampleWater(x, z, false);
}

WaterSurface SurfaceSampler::sampleWater(int x, int z, bool preview) {
    x = wrapX(x);
    z = std::clamp(z, 0, depth() - 1);
    const uint64_t key = (uint64_t(x) << 32) | unsigned(z);
    if (!preview)
        if (auto it = waterSamples.find(key); it != waterSamples.end())
            return it->second;
    const bool needsHeight = !world.hydrology || world.hydrology->riverAt(x, z);
    const int baseGround = needsHeight ? height(x, z) : -1;
    auto result = preview && world.hydrology
                      ? world.hydrology->previewWaterAt(x, z, baseGround)
                      : generatedWater(x, z, baseGround);
    if (reservoirs &&
        (reservoirs->levels.contains(result.body) ||
         reservoirs->riverStages.contains(result.reach) ||
         reservoirs->poolIntersects(x, z, x + 1, z + 1, width()))) {
        const int ground = height(x, z);
        // A provisional coastal sample does not prove membership as the
        // ocean recedes. Resolve the actual baseline before applying a level.
        if (!preview && provisionalWater() &&
            reservoirs->levels.contains(result.body))
            result = world.hydrology->waterAt(x, z, ground);
        const auto body = result.body;
        // A lowered standing body may need a new connectivity survey.
        // Wait for the resident voxel's answer before applying that effect.
        if (preview && body && !result.reach &&
            reservoirs->level(body, result.surfaceY) < result.surfaceY)
            result = {};
        else
            result = reservoirs->projectStanding(
                preview ? nullptr : world.hydrology.get(), x, z, ground,
                result);
        if (!preview && !result.wet())
            result = reservoirs->projectLakeInflow(world.hydrology.get(), x, z,
                                                   ground, body);
    }
    if (const auto* edit = editAt(x, z)) {
        const int i = ColumnEdit::index(x % Chunk::SIZE_X, z % Chunk::SIZE_Z);
        result = edit->waterAt(i, result);
    }
    // Edits can cap the generated reservoir, not the exact water projected
    // by the live ledger into the newly dug opening.
    if (reservoirs &&
        (reservoirs->poolIntersects(x, z, x + 1, z + 1, width()) ||
         reservoirs->localTop(x, z) >= 0 || reservoirs->streams.contains(key)))
        result = reservoirs->projectLocal(x, z, height(x, z), result);
    // A frozen lid hides the liquid below it from the distant water pass.
    // The ice itself is drawn by the height field above the waterline.
    if (snow && snow->iceBlock)
        if (const auto ice = snow->iceTop(x, z);
            ice &&
            (!result.wet() ||
             double(*ice) / snow::UnitsPerCubicMetre + .01 >= result.height()))
            result = {};
    if (!preview) {
        if (waterSamples.size() >= 65536) waterSamples.clear();
        waterSamples.emplace(key, result);
    }
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
