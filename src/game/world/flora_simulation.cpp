#include "flora_simulation.hpp"

#include <algorithm>
#include <cmath>

#include "sky_height_map.hpp"

namespace {

// Calls fn(coord, index, plant, rule, volume) for plants whose volume can
// include or stand on tile (x, y, z). `changed` layers replace live ones.
template <typename F>
void forPlantsNear(const ChunkManager& chunks, int64_t x, int y, int64_t z,
                   const std::unordered_map<ChunkCoord, FloraLayer>& changed,
                   F&& fn) {
    const auto& table = chunks.floraSpecies();
    if (table.empty() || y < 0 || z < 0) return;
    const auto wrap = chunks.wrap();
    const int64_t cx = x / Chunk::SIZE_X, cz = z / Chunk::SIZE_Z;
    const int lowY = std::max(0, (y - table.maxHeight() - 1) / Chunk::SIZE_Y);
    for (int64_t dz = -1; dz <= 1; ++dz)
        for (int64_t dx = -1; dx <= 1; ++dx) {
            const int64_t nx = wrap.wrapChunk(cx + dx), nz = cz + dz;
            if (nx < 0 || nz < 0) continue;
            for (int ny = lowY; ny <= y / Chunk::SIZE_Y; ++ny) {
                const ChunkCoord coord{unsigned(nx), unsigned(ny),
                                       unsigned(nz)};
                const FloraLayer* layer = nullptr;
                FloraLayerPtr live;
                if (auto it = changed.find(coord); it != changed.end())
                    layer = &it->second;
                else if ((live = chunks.flora(coord)))
                    layer = live.get();
                if (!layer) continue;
                for (size_t i = 0; i < layer->instances.size(); ++i) {
                    const auto& p = layer->instances[i];
                    const auto* rule = table.find(p.species);
                    if (!rule) continue;
                    auto volume = floraVolume(p, coord, *rule);
                    volume.minX = x + wrap.deltaTile(volume.minX, x);
                    fn(coord, i, p, *rule, volume);
                }
            }
        }
}

}  // namespace

bool advanceFlora(FloraLayer& layer, const DataStorage<Flora>& species,
                  double today) {
    const auto before = layer.instances.size();
    bool changed = false;
    std::erase_if(layer.instances, [&](const FloraInstance& p) {
        if (!p.species || p.species > species.items.size()) return false;
        const auto& f = species.get(p.species);
        return today - p.birthDay >= flora_growth::deathAge(f, p.seed);
    });
    changed = layer.instances.size() != before;
    for (auto& p : layer.instances) {
        if (!p.species || p.species > species.items.size()) continue;
        const auto& f = species.get(p.species);
        if ((p.flags & FloraInstance::Harvested) &&
            f.growth.regrow_after_harvest_days) {
            const double regrow = f.growth.regrow_after_harvest_days->sample(
                flora_growth::unit(p.seed, 41));
            if (today >= p.harvestDay + regrow) {
                p.flags &= uint8_t(~FloraInstance::Harvested);
                changed = true;
            }
        }
    }
    return changed;
}

FloraSimulation::FloraSimulation(ChunkManager& chunks, const IAssets& assets,
                                 const World& world, const SkyHeightMap& sky)
    : chunks(chunks), assets(assets), world(world), sky(sky) {
    readyConnection = chunks.becameReady.connect([this](const ChunkCoord& c) {
        loaded.push_back(c);
        // Saved edits may have left topsoil without cover.
        if (this->chunks.isEdited(c)) {
            coverPending.insert(c);
            coverSince.try_emplace(c, lastToday);
            // A saved floor edit may affect a footprint anchored in one
            // of the neighbouring chunks, including across wrapped X.
            const auto wrap = this->chunks.wrap();
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dz) continue;
                    const auto x = wrap.wrapChunk(int64_t(c.x) + dx);
                    const auto z = int64_t(c.z) + dz;
                    if (x < 0 || z < 0) continue;
                    const ChunkCoord neighbor{unsigned(x), c.y, unsigned(z)};
                    if (this->chunks.flora(neighbor))
                        loaded.push_back(neighbor);
                }
        }
    });
    removedConnection =
        this->chunks.removed.connect([this](const ChunkCoord& c) {
            coverPending.erase(c);
            coverSince.erase(c);
        });
}

FloraSimulation::~FloraSimulation() {
    chunks.becameReady.disconnect(readyConnection);
    chunks.removed.disconnect(removedConnection);
    if (editor) editor->blockChanged.disconnect(editConnection);
}

void FloraSimulation::watch(WorldEditor& value) {
    if (editor) editor->blockChanged.disconnect(editConnection);
    editor = &value;
    editConnection = editor->blockChanged.connect([this](const BlockChange& c) {
        // The changed tile and the one below it can be exposed topsoil.
        for (int dy : {0, -1}) {
            const int y = c.pos.y + dy;
            if (y < 0) continue;
            const ChunkCoord coord{unsigned(c.pos.x / Chunk::SIZE_X),
                                   unsigned(y / Chunk::SIZE_Y),
                                   unsigned(c.pos.z / Chunk::SIZE_Z)};
            coverPending.insert(coord);
            // Restart the clock of a chunk only when it starts waiting.
            coverSince.try_emplace(coord, lastToday);
        }
    });
}

void FloraSimulation::update(double today) {
    lastToday = today;
    regenerations = 0;
    chunks.setFloraDay(int32_t(std::floor(today)));
    if (chunks.floraSpecies().empty()) return;
    int budget = LoadsPerUpdate;
    // Newest first; a chunk deferred by the regeneration budget waits.
    std::vector<ChunkCoord> deferred;
    while (budget > 0 && !loaded.empty()) {
        const auto coord = loaded.back();
        loaded.pop_back();
        const auto layer = chunks.flora(coord);
        if (!layer) continue;
        if (today - layer->resolvedDay >= CatchUpDays &&
            regenerations >= RegenerationsPerUpdate) {
            deferred.push_back(coord);
            continue;
        }
        const auto unsupported = [&](const FloraInstance& plant) {
            const auto* rule = chunks.floraSpecies().find(plant.species);
            return rule && !chunks.floraAnchorHeight(coord, plant, *rule);
        };
        if (std::any_of(layer->instances.begin(), layer->instances.end(),
                        unsupported)) {
            auto kept = std::make_shared<FloraLayer>(*layer);
            std::erase_if(kept->instances, unsupported);
            chunks.setFlora(coord, std::move(kept));
        }
        resolve(coord, today);
        --budget;
    }
    loaded.insert(loaded.begin(), deferred.begin(), deferred.end());
    if (sweepAt >= sweep.size()) {
        sweep.clear();
        sweepAt = 0;
        chunks.forEachReady([&](const ChunkCoord& coord, Chunk&) {
            if (chunks.flora(coord)) sweep.push_back(coord);
        });
    }
    for (int i = 0; i < SweepsPerUpdate && sweepAt < sweep.size(); ++i)
        resolve(sweep[sweepAt++], today);
    if (!coverPending.empty()) {
        std::vector<ChunkCoord> batch;
        for (const auto& c : coverPending) {
            batch.push_back(c);
            if (int(batch.size()) >= CoverChunksPerUpdate) break;
        }
        for (const auto& c : batch) {
            if (!chunks.isReady(c) || !regrowCover(c, today)) {
                coverPending.erase(c);
                coverSince.erase(c);
            }
        }
    }
}

void FloraSimulation::resolve(const ChunkCoord& coord, double today) {
    const auto live = chunks.flora(coord);
    if (!live || !sky.tile(coord.x, coord.z)) return;
    const auto day = int32_t(std::floor(today));
    if (live->resolvedDay >= day) return;
    if (today - live->resolvedDay >= CatchUpDays) {
        // Over budget: a later update or sweep re-samples it.
        if (regenerations < RegenerationsPerUpdate) regenerate(coord, today);
        return;
    }
    std::unordered_map<ChunkCoord, FloraLayer> changed;
    auto& layer = changed.emplace(coord, *live).first->second;
    bool modified = advanceFlora(layer, assets.getStorage<Flora>(), today);
    const double since = live->resolvedDay;
    // Seeds come from the plants alive before this resolve (plants that died
    // since still seeded while they lived).
    if (!spread(coord, *live, since, today, changed)) return;
    for (auto& [c, l] : changed) {
        const bool self = c == coord;
        if (self) {
            l.resolvedDay = day;
            modified = modified || l.instances != live->instances;
        }
        // The day stamp alone is bookkeeping: resolving again is harmless.
        chunks.setFlora(c, std::make_shared<const FloraLayer>(std::move(l)),
                        !self || modified);
    }
}

void FloraSimulation::regenerate(const ChunkCoord& coord, double today) {
    ++regenerations;
    const auto day = int32_t(std::floor(today));
    auto layer =
        chunks.regenerateFlora(coord, ChunkManager::floraEpochSalt(day));
    const auto live = chunks.flora(coord);
    if (layer.empty() && live && !chunks.generatedColumns(coord.x, coord.z)) {
        // Without generation inputs, replay as far as allowed instead.
        FloraLayer advanced = *live;
        advanceFlora(advanced, assets.getStorage<Flora>(), today);
        advanced.resolvedDay = day;
        chunks.setFlora(coord, std::make_shared<const FloraLayer>(advanced));
        return;
    }
    // Ages of generated plants count back from day 0; move them to today.
    for (auto& p : layer.instances) p.birthDay += day;
    layer.resolvedDay = day;
    // Large plants must not overlap those of neighbours resolved otherwise.
    const std::unordered_map<ChunkCoord, FloraLayer> replaced{
        {coord, FloraLayer{}}};
    const auto& table = chunks.floraSpecies();
    std::erase_if(layer.instances, [&](const FloraInstance& p) {
        const auto* rule = table.find(p.species);
        const int x = int(coord.x) * Chunk::SIZE_X + int(p.localX());
        const int y = int(coord.y) * Chunk::SIZE_Y + p.y + 1;
        const int z = int(coord.z) * Chunk::SIZE_Z + int(p.localZ());
        if (sky.directSky(x, y, z).value_or(0) < 10) return true;
        if (rule && !chunks.floraAnchorHeight(coord, p, *rule)) return true;
        if (!rule || !rule->large) return false;
        const auto v = floraVolume(p, coord, *rule);
        for (int dz = 0; dz < v.sizeZ; ++dz)
            for (int dx = 0; dx < v.sizeX; ++dx)
                if (inFootprint(v.minX + dx, v.anchorY + 1, v.minZ + dz,
                                replaced))
                    return true;
        return false;
    });
    chunks.setFlora(coord, std::make_shared<const FloraLayer>(layer));
}

bool FloraSimulation::spread(
    const ChunkCoord& coord, const FloraLayer& layer, double since,
    double today, std::unordered_map<ChunkCoord, FloraLayer>& changed) {
    bool skyPending = false;
    const auto& species = assets.getStorage<Flora>();
    const auto& table = chunks.floraSpecies();
    for (const auto& p : layer.instances) {
        if (!p.species || p.species > species.items.size()) continue;
        const auto& f = species.get(p.species);
        const auto* rule = table.find(p.species);
        const auto& g = f.growth;
        if (!rule || g.spread_chance_per_cycle <= 0 || g.spread_radius_m <= 0)
            continue;
        const double cycle = flora_growth::cycle(f, p.seed);
        if (cycle <= 0) continue;
        // Seeds of cycle k drop at birth + juvenile + k * cycle + drop,
        // while the plant lives.
        const double first = p.birthDay + flora_growth::juvenile(f, p.seed) +
                             flora_growth::seedDrop(f, p.seed);
        const double last =
            std::min(today, p.birthDay + flora_growth::deathAge(f, p.seed));
        if (last < first) continue;
        const auto lastK = int64_t(std::floor((last - first) / cycle));
        const auto firstK = std::max<int64_t>(
            {0, int64_t(std::floor((since - first) / cycle)) + 1,
             lastK - MaxCyclesPerResolve + 1});
        const int anchorY = int(coord.y) * Chunk::SIZE_Y + p.y;
        for (int64_t k = firstK; k <= lastK; ++k) {
            if (flora_growth::unit(p.seed, int(100 + k)) >=
                g.spread_chance_per_cycle)
                continue;
            const double dropDay = first + double(k) * cycle;
            const int seeds = g.spread_seeds.sampleInt(
                flora_growth::unit(p.seed, int(150 + k)));
            for (int j = 0; j < seeds; ++j) {
                const int salt = int(k * 64 + j);
                const double angle =
                    flora_growth::unit(p.seed, 200 + salt) * 6.283185307179586;
                const double distance =
                    0.3 + (g.spread_radius_m - 0.3) *
                              std::sqrt(flora_growth::unit(p.seed, 300 + salt));
                const double x = double(coord.x) * Chunk::SIZE_X + p.localX() +
                                 std::cos(angle) * distance;
                const double z = double(coord.z) * Chunk::SIZE_Z + p.localZ() +
                                 std::sin(angle) * distance;
                const uint16_t child =
                    flora_growth::nextSeed(p.seed, uint32_t(salt + 1));
                // Dormant for a few days after falling.
                const auto birth = int32_t(
                    std::ceil(dropDay + 1 + 9 * flora_growth::unit(child, 31)));
                plant(*rule, x, z, anchorY, birth, child,
                      flora_growth::unit(child, 400), g.spread_germination,
                      changed, skyPending);
                if (skyPending) return false;
            }
        }
    }
    return true;
}

const FloraLayer* FloraSimulation::layerOf(
    const ChunkCoord& coord,
    const std::unordered_map<ChunkCoord, FloraLayer>& changed) const {
    if (auto it = changed.find(coord); it != changed.end()) return &it->second;
    const auto live = chunks.flora(coord);
    return live.get();  // owned by the chunk handle during this call
}

bool FloraSimulation::inFootprint(
    int64_t x, int y, int64_t z,
    const std::unordered_map<ChunkCoord, FloraLayer>& changed) const {
    bool inside = false;
    forPlantsNear(chunks, x, y, z, changed,
                  [&](const ChunkCoord&, size_t, const FloraInstance&,
                      const FloraSpeciesRule& rule, const FloraVolume& v) {
                      inside = inside || (rule.large && v.contains(x, y, z));
                  });
    return inside;
}

bool FloraSimulation::plant(const FloraSpeciesRule& rule, double x, double z,
                            int nearY, int32_t birthDay, uint16_t seed,
                            double roll, double germination,
                            std::unordered_map<ChunkCoord, FloraLayer>& changed,
                            bool& skyPending) {
    // Cheapest test first: most seeds fail on their own.
    if (roll >= germination) return false;
    if (world.regions.empty() || world.width <= 0) return false;
    const auto wrap = chunks.wrap();
    // Large plants take the footprint around the point, like generation.
    int64_t minX = int64_t(std::floor(x)), minZ = int64_t(std::floor(z));
    if (rule.large) {
        minX -= rule.footprintX / 2;
        minZ -= rule.footprintZ / 2;
        x = double(minX) + rule.footprintX * 0.5;
        z = double(minZ) + rule.footprintZ * 0.5;
    }
    const int64_t tx = wrap.wrapTile(int64_t(std::floor(x)));
    const int64_t tz = int64_t(std::floor(z));
    if (tx < 0 || tz < 0 || tz >= int64_t(world.height) * 128) return false;
    // Use the same quantized anchor that will be stored and rendered.
    const int subX = int((x - std::floor(x)) * 16);
    const int subZ = int((z - std::floor(z)) * 16);
    const float anchorX = (subX + .5f) / 16, anchorZ = (subZ + .5f) / 16;
    // The ground near the parent's height: solid with open air above.
    int anchorY = -1;
    Tile ground;
    const auto open = [&](int px, int y, int pz) {
        if (const Tile* tile = chunks.tileAt(px, y, pz)) return tile->isAir();
        const auto column = sky.column(px, pz);
        if (!column) {
            skyPending = true;
            return false;
        }
        return column->opacityAt(y) == 0;
    };
    for (int y = nearY + 3; y >= std::max(0, nearY - 3); --y) {
        const auto cell = chunks.cellAt(int(tx), y, int(tz));
        if (cell && floraSupportHeight(rule, *cell, anchorX, anchorZ) &&
            open(int(tx), y + 1, int(tz))) {
            anchorY = y;
            ground = cell->tile;
            break;
        }
    }
    if (anchorY < 0) return false;
    // Germination and catch-up use direct overhead light, including the
    // attenuation of translucent roofs.
    const auto direct = sky.directSky(tx, anchorY + 1, tz);
    if (!direct) {
        skyPending = true;
        return false;
    }
    if (*direct < 10) return false;
    FloraSite site;
    site.surfaceY = anchorY;
    // The open-air test above covers new worlds. Preserve the historical
    // elevation filter for spreading in saves with legacy generation.
    site.submerged = !world.hydrology && WaterSurface::sea(anchorY).wet();
    const auto [soil, topsoil] =
        chunks.floraSpecies().soilOfBlock(ground.block_id);
    site.soil = soil;
    site.topsoil = topsoil;
    site.cover = ground.cover;
    site.coverDensity = ground.coverDensity();
    if (const auto columns = chunks.generatedColumns(
            unsigned(tx / Chunk::SIZE_X), unsigned(tz / Chunk::SIZE_Z)))
        site.slope = columns
                         ->columns[(tz % Chunk::SIZE_Z) * Chunk::SIZE_X +
                                   tx % Chunk::SIZE_X]
                         .slope;
    site.region = &world.regions[size_t(tz / 128) * world.width + tx / 128];
    if (!floraSuits(chunks.floraSpecies(), rule, site)) return false;
    // Room. Under the footprint: level, solid ground with air up to the
    // plant's height. Around a large footprint (its canopy overhangs it):
    // nothing solid at the plant's level. No large footprint anywhere in
    // that area (with clearance for large plants).
    const int gap = rule.large ? std::max(1, int(std::ceil(rule.spacing))) : 0;
    const int sizeX = rule.large ? rule.footprintX : 1;
    const int sizeZ = rule.large ? rule.footprintZ : 1;
    const int64_t baseX = rule.large ? minX : int64_t(std::floor(x));
    const int64_t baseZ = rule.large ? minZ : tz;
    for (int64_t dz = -gap; dz < sizeZ + gap; ++dz)
        for (int64_t dx = -gap; dx < sizeX + gap; ++dx) {
            const int px = int(wrap.wrapTile(baseX + dx));
            const int pz = int(baseZ + dz);
            if (inFootprint(baseX + dx, anchorY + 1, pz, changed)) return false;
            const bool under = dx >= 0 && dz >= 0 && dx < sizeX && dz < sizeZ;
            if (under) {
                if (rule.large && (px != tx || pz != tz)) {
                    const auto support = chunks.cellAt(px, anchorY, pz);
                    if (!support ||
                        !floraSupportHeight(rule, *support, anchorX, anchorZ))
                        return false;
                }
                for (int h = 1; h <= rule.heightTiles; ++h)
                    if (!open(px, anchorY + h, pz)) return false;
            } else if (dx >= -1 && dx <= sizeX && dz >= -1 && dz <= sizeZ) {
                // The ring right next to the footprint.
                const Tile* beside = chunks.tileAt(px, anchorY + 1, pz);
                if (beside && beside->isSolid()) return false;
            }
        }
    const ChunkCoord target{unsigned(tx / Chunk::SIZE_X),
                            unsigned(anchorY / Chunk::SIZE_Y),
                            unsigned(tz / Chunk::SIZE_Z)};
    // Position in the target chunk (tx is the canonical tile of x).
    FloraInstance child;
    child.species = rule.id;
    child.seed = seed;
    child.x = uint8_t((tx % Chunk::SIZE_X) * 16 + subX);
    child.z = uint8_t((tz % Chunk::SIZE_Z) * 16 + subZ);
    child.y = uint8_t(anchorY - int(target.y) * Chunk::SIZE_Y);
    child.rotation = uint8_t(seed >> 5);
    child.birthDay = birthDay;
    const FloraLayer* current = layerOf(target, changed);
    // Deterministic seeds: the same seed never germinates twice.
    if (current)
        for (const auto& p : current->instances)
            if (p.species == rule.id &&
                (p.seed == child.seed ||
                 (p.x == child.x && p.z == child.z && p.y == child.y)))
                return false;
    // Crowding: the chance falls linearly to 0 as the neighbourhood reaches
    // the species' generated density there. The neighbourhood holds about
    // CrowdPlants at full density, so sparse species (ferns, bushes) are not
    // crowded out by their own parent.
    constexpr double Pi = 3.141592653589793;
    const double radius =
        std::max({CrowdRadius, rule.large ? double(rule.cell) : 0.0,
                  std::sqrt(CrowdPlants * 100.0 /
                            (Pi * std::max(1e-7f, rule.density.max)))});
    const double area = Pi * radius * radius;
    const double target_count =
        rule.density.max * area / 100.0 *
        floraClusterMask(rule, chunks.floraSeed(), x, z, wrap.tiles);
    if (target_count <= 0) return false;
    const int local = crowd(rule, x, z, anchorY, radius, changed);
    if (roll >= germination * std::max(0.0, 1.0 - local / target_count))
        return false;
    auto& out = changed.try_emplace(target, current ? *current : FloraLayer{})
                    .first->second;
    out.instances.push_back(child);
    return true;
}

int FloraSimulation::crowd(
    const FloraSpeciesRule& rule, double x, double z, int y, double radius,
    const std::unordered_map<ChunkCoord, FloraLayer>& changed) const {
    const auto wrap = chunks.wrap();
    const int reach = int(std::ceil(radius / Chunk::SIZE_X)) + 1;
    const int64_t cx = int64_t(std::floor(x)) / Chunk::SIZE_X;
    const int64_t cz = int64_t(std::floor(z)) / Chunk::SIZE_Z;
    const int slab = y / Chunk::SIZE_Y;
    int count = 0;
    const auto countIn = [&](const ChunkCoord& c, const FloraLayer& layer) {
        if (std::abs(int(c.y) - slab) > 1) return;  // nearby heights only
        // The chunk's origin in the frame of x (X wraps).
        const double ox =
            std::floor(x) + double(wrap.deltaTile(int64_t(c.x) * Chunk::SIZE_X,
                                                  int64_t(std::floor(x))));
        for (const auto& p : layer.instances) {
            if (p.species != rule.id) continue;
            const double ddx = ox + p.localX() - x;
            const double ddz = double(c.z) * Chunk::SIZE_Z + p.localZ() - z;
            if (ddx * ddx + ddz * ddz <= radius * radius) ++count;
        }
    };
    if (reach > 4) {
        // Rare species look far: visit only chunks that have plants.
        std::unordered_set<ChunkCoord> seen;
        const auto visit = [&](const ChunkCoord& c) {
            if (!seen.insert(c).second) return;
            if (std::abs(wrap.deltaChunk(c.x, cx)) > reach ||
                std::abs(int64_t(c.z) - cz) > reach)
                return;
            if (const FloraLayer* layer = layerOf(c, changed))
                countIn(c, *layer);
        };
        for (const auto& [c, layer] : changed) visit(c);
        for (const auto& c : sweep) visit(c);
        for (const auto& c : loaded) visit(c);
        return count;
    }
    for (int64_t dz = -reach; dz <= reach; ++dz)
        for (int64_t dx = -reach; dx <= reach; ++dx) {
            const int64_t nz = cz + dz;
            if (nz < 0) continue;
            const int64_t nx = wrap.wrapChunk(cx + dx);
            for (int dy = -1; dy <= 1; ++dy) {
                if (slab + dy < 0) continue;
                const ChunkCoord c{unsigned(nx), unsigned(slab + dy),
                                   unsigned(nz)};
                if (const FloraLayer* layer = layerOf(c, changed))
                    countIn(c, *layer);
            }
        }
    return count;
}

bool FloraSimulation::regrowCover(const ChunkCoord& coord, double today) {
    const auto columns = chunks.generatedColumns(coord.x, coord.z);
    if (!columns) return false;
    const auto skyTile = sky.tile(coord.x, coord.z);
    if (!skyTile) return true;
    const auto since = coverSince.try_emplace(coord, today).first->second;
    const double elapsed = today - since;
    if (elapsed < CoverAppearDays) return true;
    const int levels = 1 + int((elapsed - CoverAppearDays) * CoverLevelsPerDay);
    const auto& table = chunks.floraSpecies();
    // Cover needs open sky: the tile above, possibly in the chunk above.
    const Chunk* upper = chunks.getChunk({coord.x, coord.y + 1, coord.z});
    bool pending = false, modified = false;
    chunks.withChunkLocked(coord, [&](Chunk& chunk) {
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int y = 0; y < Chunk::SIZE_Y; ++y)
                for (int x = 0; x < Chunk::SIZE_X; ++x) {
                    Tile& tile = chunk.tiles[z][y][x];
                    if (!tile.isSolid() ||
                        !table.soilOfBlock(tile.block_id).second)
                        continue;
                    const auto& column =
                        columns->columns[z * Chunk::SIZE_X + x].soil;
                    if (!column.cover || !column.coverDensity) continue;
                    if (tile.cover && tile.cover != column.cover) continue;
                    const Tile* above = y + 1 < Chunk::SIZE_Y
                                            ? &chunk.tiles[z][y + 1][x]
                                        : upper ? &upper->tiles[z][0][x]
                                                : nullptr;
                    const int worldY = int(coord.y) * Chunk::SIZE_Y + y + 1;
                    const int index = sky::Tile::index(x, z);
                    if (above && !above->isAir()) continue;
                    const int density = tile.cover ? tile.coverDensity() : 0;
                    if (density >= column.coverDensity) continue;
                    if (skyTile->direct(index, worldY) < 15) {
                        pending = true;
                        continue;
                    }
                    if (!above && skyTile->top(index) == worldY) continue;
                    const int next = std::min<int>(column.coverDensity, levels);
                    if (next > density) {
                        tile.setCover(column.cover, uint8_t(next));
                        modified = true;
                    }
                    if (next < column.coverDensity) pending = true;
                }
    });
    if (modified) chunks.markModified(coord);
    return pending;
}

std::vector<ForageYield> FloraSimulation::harvest(TilePos tile, double today) {
    std::vector<ForageYield> yields;
    const auto& species = assets.getStorage<Flora>();
    const auto day = int32_t(std::floor(today));
    std::unordered_map<ChunkCoord, FloraLayer> changed;
    std::unordered_map<ChunkCoord, std::vector<size_t>> removed;
    const int64_t x = chunks.wrap().wrapTile(tile.x);
    forPlantsNear(
        chunks, x, tile.y, tile.z, {},
        [&](const ChunkCoord& coord, size_t index, const FloraInstance& p,
            const FloraSpeciesRule&, const FloraVolume& v) {
            if (!v.contains(x, tile.y, tile.z) &&
                !v.supportedBy(x, tile.y, tile.z))
                return;
            if (p.flags & FloraInstance::Harvested) return;
            const auto& f = species.get(p.species);
            const auto state = flora_growth::at(f, p.seed, today - p.birthDay);
            if (!state.alive) return;
            bool picked = false;
            for (size_t i = 0; i < f.forage.size(); ++i) {
                const auto& entry = f.forage[i];
                if (entry.stage != state.stage) continue;
                const int count = entry.count.sampleInt(
                    flora_growth::unit(p.seed, int(50 + i + day)));
                if (count <= 0) continue;
                yields.push_back({entry.item, count});
                picked = true;
            }
            if (!picked) return;
            auto& layer =
                changed.try_emplace(coord, *chunks.flora(coord)).first->second;
            if (f.growth.regrow_after_harvest_days) {
                layer.instances[index].flags |= FloraInstance::Harvested;
                layer.instances[index].harvestDay = day;
            } else {
                removed[coord].push_back(index);
            }
        });
    for (auto& [coord, layer] : changed) {
        auto& gone = removed[coord];
        std::sort(gone.rbegin(), gone.rend());
        for (const auto i : gone)
            layer.instances.erase(layer.instances.begin() + long(i));
        chunks.setFlora(coord, std::make_shared<const FloraLayer>(layer));
    }
    return yields;
}

std::vector<FloraSimulation::PlantInfo> FloraSimulation::plantsAt(
    TilePos tile, double today) const {
    std::vector<PlantInfo> out;
    if (!assets.hasStorage<Flora>()) return out;
    const auto& species = assets.getStorage<Flora>();
    const int64_t x = chunks.wrap().wrapTile(tile.x);
    forPlantsNear(
        chunks, x, tile.y, tile.z, {},
        [&](const ChunkCoord&, size_t, const FloraInstance& p,
            const FloraSpeciesRule&, const FloraVolume& v) {
            if (!v.contains(x, tile.y, tile.z) &&
                !v.supportedBy(x, tile.y, tile.z))
                return;
            const auto& f = species.get(p.species);
            out.push_back(
                {&f, p, flora_growth::at(f, p.seed, today - p.birthDay)});
        });
    return out;
}
