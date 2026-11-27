#include "surface_ice.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "generated_hydrology.hpp"
#include "world.hpp"
#include "world_calendar.hpp"
#include "world_climate.hpp"

namespace surface_ice {
namespace {

constexpr tile_geometry::PieceId PieceBase = 0x60000000u;

}  // namespace

float stageThickness(uint8_t stage) {
    return StageThickness[std::min<size_t>(stage, StageThickness.size() - 1)];
}

WaterVolume waterUnitsForMetres(double metres) {
    return WaterVolume(std::llround(std::max(0.0, metres) * IceSpecificGravity *
                                    WaterUnitsPerBlock));
}

bool Regional::update(double worldDays) {
    if (world.width <= 0 || world.height <= 0 || world.regions.empty())
        return false;
    const int64_t step = int64_t(std::floor(worldDays / 5.0));
    if (step == lastStep) return false;
    lastStep = step;
    width = int(world.width);
    height = int(world.height);
    values.assign(size_t(width) * height, 0.f);
    feature.assign(size_t(width) * height, 0);
    for (int rz = 0; rz < height; ++rz) {
        const float seasonal =
            world_climate::seasonalOffsetC(rz, height, worldDays);
        for (int rx = 0; rx < width; ++rx) {
            const auto& region = world.regions[size_t(rz) * width + rx];
            float target = 0.f;
            if (region.hasFeature(FeatureMask::ICE_SHEET) ||
                region.hasFeature(FeatureMask::GLACIER)) {
                target = 16.f;
                feature[size_t(rz) * width + rx] = 1;
            } else if (region.hasFeature(FeatureMask::SEA_ICE)) {
                target = 1.f;
                feature[size_t(rz) * width + rx] = 2;
            } else {
                const float temperature = region.temperature_c + seasonal;
                if (temperature < -.1f)
                    target = std::min(1.f, -temperature * .02f);
            }
            values[size_t(rz) * width + rx] = target;
        }
    }
    ++revision_;
    return true;
}

bool Regional::featureAt(int tileX, int tileZ) const {
    if (feature.empty() || width <= 0 || height <= 0) return false;
    constexpr double size = WorldWrap::RegionSize;
    const int rx = int(world.wrap().wrapRegion(
        int64_t(std::floor(world.wrap().wrapX(tileX) / size))));
    const int rz = std::clamp(int(std::floor(tileZ / size)), 0, height - 1);
    return feature[size_t(rz) * width + rx] != 0;
}

float Regional::targetAt(int tileX, int tileZ) const {
    if (values.empty() || width <= 0 || height <= 0) return 0.f;
    constexpr double size = WorldWrap::RegionSize;
    const double fx = (world.wrap().wrapX(tileX) + .5) / size - .5;
    const double fz =
        (std::clamp(tileZ, 0, int(world.height) * int(size) - 1) + .5) / size -
        .5;
    const int gx = int(std::floor(fx)), gz = int(std::floor(fz));
    const double tx = fx - gx, tz = fz - gz;
    float result = 0;
    for (int dz = 0; dz < 2; ++dz) {
        const int rz = std::clamp(gz + dz, 0, height - 1);
        for (int dx = 0; dx < 2; ++dx) {
            const int rx = int(world.wrap().wrapRegion(gx + dx));
            const float weight =
                float((dx ? tx : 1. - tx) * (dz ? tz : 1. - tz));
            result += values[size_t(rz) * width + rx] * weight;
        }
    }
    return result;
}

UpdateStats Simulation::update(Reservoirs& water, const Terrain& terrain,
                               const TemperatureFn& temperature, double day,
                               double dtDays, int centreX, int centreZ,
                               int radius, size_t budget,
                               const ManagedFn& featureManaged) {
    UpdateStats stats;
    if (dtDays <= 0 || radius < 0) return stats;
    const double hours = dtDays * 24.;
    for (int dz = -radius; dz <= radius; ++dz)
        for (int dx = -radius; dx <= radius; ++dx) {
            if (stats.visited >= budget) return stats;
            const int x = terrain.wrapX(centreX + dx);
            const int z = centreZ + dz;
            if (x < 0 || z < 0 || !terrain.inBounds(x, z)) continue;
            ++stats.visited;
            const auto sample = terrain.column(x, z);
            if (!sample) continue;
            const auto current = water.snapshot();
            const auto* prior = current->surfaceIceAt(x, z);
            if (featureManaged && featureManaged(x, z) && !prior) continue;
            const bool wet = sample->water.wet();
            if (!wet && (!prior || prior->frozen <= 0)) continue;
            const double surface =
                wet ? sample->water.height() : double(prior->waterline);
            double depth = std::max(0.0, surface - (sample->ground + 1));
            if (!wet) depth = std::max(depth, double(prior->thickness));
            if (depth <= 1e-6) continue;
            const int cellY =
                wet ? int(std::floor(surface - 1e-4)) : prior->cellY;
            if (cellY < 0 || cellY >= terrain.maxY()) continue;

            const bool flowing =
                wet ? sample->water.reach != 0 : prior->reach != 0;
            const float temp = temperature(x, z, surface, day);
            const double rate = temp < 0
                                    ? FreezeMetresPerHourPerC * -temp * hours *
                                          (flowing ? FlowFreezeFactor : 1.)
                                    : -ThawMetresPerHourPerC * temp * hours;
            const SurfaceKey key{x, z};
            int64_t progress = prior ? prior->progress : 0;
            if (const auto pending = pendingProgress.find(key);
                pending != pendingProgress.end())
                progress = pending->second;
            progress += int64_t(std::llround(rate * double(ProgressPerMetre)));

            bool stageChanged = false;
            uint8_t stage = prior ? prior->stage : 0;
            bool through = prior ? prior->through : false;
            double thickness = prior ? prior->thickness : 0.;
            double waterline = prior ? prior->waterline : surface;
            bool stranded = prior ? prior->stranded : false;
            if (!prior) waterline = surface;
            // Floating ice follows a rising mean stage; a falling stage can
            // leave it stranded on the shore once the liquid leaves its
            // underside.
            waterline = std::max(waterline, surface);
            if (surface + .005 < waterline - thickness) stranded = true;
            if (surface >= waterline - .005) stranded = false;
            // A shallower basin freezes through before a deeper one reaches
            // its first 5 cm lid.
            if (through) thickness = std::min(thickness, depth);
            thickness = std::max(0., thickness);
            // Once the column is at its depth or the 1 m stage, further cold
            // cannot add another layer. Keeping an unbounded positive budget
            // would make a later thaw take as long as the whole cold spell.
            const double maxThickness =
                std::min<double>(depth, StageThickness.back());
            if (thickness >= maxThickness - 1e-5 && progress > 0) progress = 0;
            if (progress > 0 && temp < 0) {
                for (int step = 0; step < 8; ++step) {
                    double target = 0.;
                    unsigned next = 6;
                    if (!through || thickness < depth - 1e-4) {
                        next = stage < 6 ? stage + 1 : 6;
                        target = std::min<double>(StageThickness[next], depth);
                    }
                    if (target <= thickness + 1e-5) break;
                    const auto needed = int64_t(std::llround(
                        (target - thickness) * double(ProgressPerMetre)));
                    if (progress < needed) break;
                    const auto amount = waterUnitsForMetres(target - thickness);
                    if (amount <= 0) break;
                    SurfaceIceInput input;
                    input.body =
                        wet ? (sample->water.reach ? 0 : sample->water.body)
                            : prior->body;
                    input.reach = wet ? sample->water.reach : prior->reach;
                    input.cellY = int16_t(cellY);
                    input.waterline = float(waterline);
                    input.thickness = float(target);
                    input.stage = target >= depth - 1e-4
                                      ? ThroughStage
                                      : uint8_t(std::min<unsigned>(next, 6));
                    input.progress = progress - needed;
                    input.through = target >= depth - 1e-4;
                    input.stranded = stranded;
                    const auto accepted =
                        water.freezeSurface({x, z}, amount, input, day);
                    if (accepted <= 0) break;
                    const double gained =
                        double(accepted) /
                        (IceSpecificGravity * WaterUnitsPerBlock);
                    thickness = std::min(target, thickness + gained);
                    stage = input.stage;
                    through = input.through;
                    progress = input.progress;
                    stats.frozen =
                        std::max<WaterVolume>(0, stats.frozen + accepted);
                    stageChanged = true;
                    stats.changedColumns.push_back(key);
                    ++stats.changed;
                    if (accepted < amount) break;
                }
            } else if (progress < 0 && temp > 0) {
                for (int step = 0; step < 8; ++step) {
                    double target = 0.;
                    if (through) {
                        target = 0.;
                    } else if (stage > 0) {
                        target = stage > 1 ? StageThickness[stage - 1] : 0.;
                    }
                    if (thickness <= target + 1e-5) break;
                    const auto needed = int64_t(std::llround(
                        (thickness - target) * double(ProgressPerMetre)));
                    if (-progress < needed) break;
                    const auto amount = waterUnitsForMetres(thickness - target);
                    if (amount <= 0) break;
                    SurfaceIceInput input;
                    input.body =
                        wet ? (sample->water.reach ? 0 : sample->water.body)
                            : prior->body;
                    input.reach = wet ? sample->water.reach : prior->reach;
                    input.cellY = int16_t(cellY);
                    input.waterline = float(waterline);
                    input.thickness = float(target);
                    input.stage = target <= 1e-5
                                      ? 0
                                      : uint8_t(std::max<int>(stage - 1, 1));
                    input.progress = progress + needed;
                    input.through = false;
                    input.stranded = stranded;
                    const auto thawed =
                        water.thawSurface({x, z}, amount, input, day);
                    if (thawed <= 0) break;
                    const double lost = double(thawed) / (IceSpecificGravity *
                                                          WaterUnitsPerBlock);
                    thickness = std::max(target, thickness - lost);
                    stage = input.stage;
                    through = false;
                    progress = input.progress;
                    stats.thawed =
                        std::max<WaterVolume>(0, stats.thawed + thawed);
                    stageChanged = true;
                    stats.changedColumns.push_back(key);
                    ++stats.changed;
                    if (thawed < amount) break;
                }
            }
            if (!prior && progress < 0) progress = 0;
            if (thickness <= 1e-5 && progress < 0) progress = 0;
            if (progress == 0 && thickness <= 1e-5 && !prior) {
                pendingProgress.erase(key);
                continue;
            }
            pendingProgress[key] = progress;
            bool needWrite = !prior || stageChanged;
            if (!needWrite && prior) {
                needWrite = stage != prior->stage ||
                            through != prior->through ||
                            stranded != prior->stranded ||
                            std::abs(thickness - prior->thickness) > 1e-5 ||
                            std::abs(waterline - prior->waterline) > 1e-4 ||
                            (thickness <= 1e-5 && progress == 0) ||
                            std::abs(progress - prior->progress) >=
                                int64_t(ProgressPerMetre * .01);
            }
            if (!needWrite) continue;
            SurfaceIceInput input;
            input.body = wet ? (sample->water.reach ? 0 : sample->water.body)
                             : prior->body;
            input.reach = wet ? sample->water.reach : prior->reach;
            input.cellY = int16_t(cellY);
            input.waterline = float(waterline);
            input.thickness = float(thickness);
            input.stage = stage;
            input.progress = progress;
            input.through = through;
            input.stranded = stranded;
            if (water.setSurfaceIce(key, input, day)) ++stats.changed;
        }
    return stats;
}

void IceCover::rebuild(Reservoirs& water, const Terrain& terrain,
                       uint8_t iceBlock,
                       const std::set<std::pair<unsigned, unsigned>>* changed,
                       const std::function<float(int, int)>* regionalTarget) {
    const auto current = water.snapshot();
    std::unordered_map<ChunkCoord, ChunkEnvironment> cells;
    const auto project =
        [&](const SurfaceKey& key, const SurfaceIceState& state,
            std::unordered_map<ChunkCoord, ChunkEnvironment>& out) {
            if (cutColumns_.contains({key.x, key.z})) return;
            if (state.frozen <= 0 || state.thickness <= 0) return;
            const double top = state.waterline;
            const double bottom = std::max(0., top - state.thickness);
            tops_[{key.x, key.z}] = int32_t(std::llround(top * 1'000'000.0));
            if (!iceBlock) return;
            // Only loaded columns need near geometry. The distant top above is
            // kept so an unloaded frozen shore still reaches the height field.
            if (!terrain.column(key.x, key.z)) return;
            const int first = std::max(0, int(std::floor(bottom + 1e-5)));
            const int last =
                std::min(terrain.maxY() - 1, int(std::floor(top - 1e-5)));
            for (int y = first; y <= last; ++y) {
                auto substrate = terrain.cell({key.x, y, key.z});
                if (!substrate || substrate->isFull()) continue;
                const float low = float(std::max(bottom - y, 0.));
                const float high = float(std::min(top - y, 1.));
                if (!(high > low + 1e-6f)) continue;
                auto fill = substrate->freeSpaceBetween(
                    low, high, iceBlock,
                    PieceBase + (uint32_t(key.z) << 12) +
                        uint32_t(key.x & 0xFFF));
                if (fill.empty()) continue;
                const ChunkCoord coord{unsigned(key.x / Chunk::SIZE_X),
                                       unsigned(y / Chunk::SIZE_Y),
                                       unsigned(key.z / Chunk::SIZE_Z)};
                const uint16_t index =
                    Chunk::cell(key.x % Chunk::SIZE_X, y % Chunk::SIZE_Y,
                                key.z % Chunk::SIZE_Z);
                auto& chunk = out[coord];
                const auto existing = chunk.find(index);
                if (existing != chunk.end()) fill.append(*existing->second);
                chunk.insert_or_assign(
                    index, std::make_shared<const tile_geometry::CellGeometry>(
                               std::move(fill)));
            }
        };
    const auto same = [](const ChunkEnvironment& a, const ChunkEnvironment& b) {
        return std::ranges::equal(a, b, [](const auto& x, const auto& y) {
            return x.first == y.first &&
                   (x.second == y.second || *x.second == *y.second);
        });
    };

    if (!changed) {
        tops_.clear();
        for (const auto& [key, state] : current->surfaceIce)
            project(key, state, cells);
        std::erase_if(cells,
                      [](const auto& item) { return item.second.empty(); });
        auto next = std::make_shared<EnvironmentMap>();
        changed_.clear();
        for (const auto& [coord, chunk] : *environment_) {
            (void)chunk;
            if (!cells.contains(coord)) changed_.push_back(coord);
        }
        for (auto& [coord, chunk] : cells) {
            if (const auto before = environment_->find(coord);
                before != environment_->end() && same(chunk, *before->second)) {
                next->insert_or_assign(coord, before->second);
                continue;
            }
            next->insert_or_assign(
                coord,
                std::make_shared<const ChunkEnvironment>(std::move(chunk)));
            changed_.push_back(coord);
        }
        environment_ = std::move(next);
    } else {
        std::unordered_map<ChunkCoord, std::shared_ptr<const ChunkEnvironment>>
            prior;
        auto next = std::make_shared<EnvironmentMap>(*environment_);
        for (auto it = next->begin(); it != next->end();) {
            if (changed->contains({it->first.x, it->first.z})) {
                prior.insert_or_assign(it->first, it->second);
                it = next->erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = tops_.begin(); it != tops_.end();) {
            if (changed->contains({unsigned(it->first.first / Chunk::SIZE_X),
                                   unsigned(it->first.second / Chunk::SIZE_Z)}))
                it = tops_.erase(it);
            else
                ++it;
        }
        for (const auto& [key, state] : current->surfaceIce) {
            if (!changed->contains({unsigned(key.x / Chunk::SIZE_X),
                                    unsigned(key.z / Chunk::SIZE_Z)}))
                continue;
            project(key, state, cells);
        }
        // Feature ice is derived directly from the seasonal target instead
        // of a per-column ledger record. A newly loaded sea-ice chunk shows
        // its cover immediately and the save stays bounded.
        if (regionalTarget)
            for (const auto& column : *changed) {
                const int x0 = int(column.first) * Chunk::SIZE_X;
                const int z0 = int(column.second) * Chunk::SIZE_Z;
                if ((*regionalTarget)(x0 + 8, z0 + 8) < .01f) continue;
                for (int z = z0; z < z0 + Chunk::SIZE_Z; ++z)
                    for (int x = x0; x < x0 + Chunk::SIZE_X; ++x) {
                        if (const auto* state = current->surfaceIceAt(x, z);
                            state && state->frozen > 0)
                            continue;
                        const float target = (*regionalTarget)(x, z);
                        if (target < .01f) continue;
                        const auto sample = terrain.column(x, z);
                        if (!sample || !sample->water.wet()) continue;
                        if (!sample->water.reach && sample->water.body &&
                            sample->water.body <= GeneratedHydrology::Sea &&
                            target < .75f)
                            continue;
                        const double waterline = sample->water.height();
                        const double depth =
                            std::max(0.0, waterline - (sample->ground + 1));
                        if (depth <= 1e-6) continue;
                        SurfaceIceState state;
                        state.frozen = 1;
                        state.waterline = float(waterline);
                        state.thickness =
                            float(std::min<double>(depth, target));
                        project({x, z}, state, cells);
                    }
            }
        std::erase_if(cells,
                      [](const auto& item) { return item.second.empty(); });
        changed_.clear();
        for (const auto& [coord, chunk] : prior) {
            (void)chunk;
            if (!cells.contains(coord)) changed_.push_back(coord);
        }
        for (auto& [coord, chunk] : cells) {
            const auto before = prior.find(coord);
            if (before != prior.end() && same(chunk, *before->second)) {
                next->insert_or_assign(coord, before->second);
                continue;
            }
            next->insert_or_assign(
                coord,
                std::make_shared<const ChunkEnvironment>(std::move(chunk)));
            changed_.push_back(coord);
        }
        environment_ = std::move(next);
    }
    std::sort(changed_.begin(), changed_.end(),
              [](const ChunkCoord& a, const ChunkCoord& b) {
                  return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
              });
    changed_.erase(std::unique(changed_.begin(), changed_.end()),
                   changed_.end());
    ++revision_;
}

EnvironmentCell IceCover::cellAt(TilePos pos) const {
    if (pos.x < 0 || pos.y < 0 || pos.z < 0) return {};
    const ChunkCoord coord{unsigned(pos.x / Chunk::SIZE_X),
                           unsigned(pos.y / Chunk::SIZE_Y),
                           unsigned(pos.z / Chunk::SIZE_Z)};
    const auto found = environment_->find(coord);
    if (found == environment_->end() || !found->second) return {};
    const uint16_t index = Chunk::cell(
        pos.x % Chunk::SIZE_X, pos.y % Chunk::SIZE_Y, pos.z % Chunk::SIZE_Z);
    const auto cell = found->second->find(index);
    return cell == found->second->end() ? EnvironmentCell{} : cell->second;
}

int IceCover::iceTop(int x, int z) const {
    const auto found = tops_.find({x, z});
    return found == tops_.end() ? INT_MIN
                                : int((found->second - 1) / 1'000'000);
}

void IceCover::cutColumn(int x, int z) {
    cutColumns_.insert({x, z});
    tops_.erase({x, z});
    ++cutRevision_;
}

bool IceCover::cut(TilePos pos) {
    const ChunkCoord coord{unsigned(pos.x / Chunk::SIZE_X),
                           unsigned(pos.y / Chunk::SIZE_Y),
                           unsigned(pos.z / Chunk::SIZE_Z)};
    const uint16_t index = Chunk::cell(
        pos.x % Chunk::SIZE_X, pos.y % Chunk::SIZE_Y, pos.z % Chunk::SIZE_Z);
    const auto found = environment_->find(coord);
    if (found == environment_->end() || !found->second ||
        !found->second->contains(index))
        return false;
    holes_.insert({pos.x, pos.y, pos.z});
    const bool wholeColumn = pos.y >= iceTop(pos.x, pos.z);
    if (wholeColumn) {
        cutColumns_.insert({pos.x, pos.z});
        tops_.erase({pos.x, pos.z});
    }
    auto next = std::make_shared<EnvironmentMap>(*environment_);
    changed_.clear();
    if (wholeColumn) {
        const unsigned cx = unsigned(pos.x / Chunk::SIZE_X);
        const unsigned cz = unsigned(pos.z / Chunk::SIZE_Z);
        for (unsigned cy = 0; cy < Y_CHUNKS; ++cy) {
            const ChunkCoord c{cx, cy, cz};
            const auto at = next->find(c);
            if (at == next->end() || !at->second) continue;
            auto chunk = std::make_shared<ChunkEnvironment>(*at->second);
            const size_t before = chunk->size();
            for (int ly = 0; ly < Chunk::SIZE_Y; ++ly)
                chunk->erase(Chunk::cell(pos.x % Chunk::SIZE_X, ly,
                                         pos.z % Chunk::SIZE_Z));
            if (chunk->size() == before) continue;
            changed_.push_back(c);
            if (chunk->empty())
                next->erase(c);
            else
                next->insert_or_assign(c, std::move(chunk));
        }
    } else {
        auto chunk = std::make_shared<ChunkEnvironment>(*found->second);
        chunk->erase(index);
        changed_.push_back(coord);
        if (chunk->empty())
            next->erase(coord);
        else
            next->insert_or_assign(coord, std::move(chunk));
    }
    environment_ = std::move(next);
    ++revision_;
    ++cutRevision_;
    return true;
}

void IceCover::clear() {
    changed_.clear();
    for (const auto& [coord, chunk] : *environment_) {
        (void)chunk;
        changed_.push_back(coord);
    }
    environment_ = std::make_shared<const EnvironmentMap>();
    tops_.clear();
    holes_.clear();
    cutColumns_.clear();
    ++revision_;
    ++cutRevision_;
}

void IceCover::fillTops(unsigned chunkX, unsigned chunkZ,
                        std::map<std::pair<int, int>, int32_t>& out) const {
    const int x0 = int(chunkX) * Chunk::SIZE_X;
    const int z0 = int(chunkZ) * Chunk::SIZE_Z;
    for (int x = x0; x < x0 + Chunk::SIZE_X; ++x)
        for (int z = z0; z < z0 + Chunk::SIZE_Z; ++z) {
            const auto found = tops_.find({x, z});
            if (found != tops_.end())
                out.insert_or_assign(found->first, found->second);
        }
}

size_t IceCover::cellCount() const {
    size_t total = 0;
    for (const auto& [coord, chunk] : *environment_) {
        (void)coord;
        if (chunk) total += chunk->size();
    }
    return total;
}

}  // namespace surface_ice
