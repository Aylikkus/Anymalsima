#include "local_temperature.hpp"

#include <algorithm>
#include <cmath>

#include "chunk.hpp"
#include "chunk_manager.hpp"
#include "world_weather.hpp"
#include "world_wrap.hpp"

namespace thermal {
namespace {

double solidFraction(ChunkManager& chunks, const int (&cell)[3],
                     const std::array<double, 3>& from,
                     const std::array<double, 3>& delta, double tEnter,
                     double tLeave) {
    const TilePos tile{cell[0], cell[1], cell[2]};
    const auto snapshot = chunks.solidCellAt(tile);
    if (!snapshot || !snapshot->cell.hasSolid()) return 0;
    if (!snapshot->cell.usesGeometry() && snapshot->cell.base.tile.isSolid())
        return 1;

    const auto geometry = snapshot->cell.geometry();
    const double span = (tLeave - tEnter) *
                        std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] +
                                  delta[2] * delta[2]);
    const int samples = std::clamp(int(std::ceil(span / .02)), 1, 64);
    int inside = 0;
    for (int i = 0; i < samples; ++i) {
        const double t = tEnter + (i + .5) / samples * (tLeave - tEnter);
        const Vec3 point{float(from[0] + delta[0] * t),
                         float(from[1] + delta[1] * t),
                         float(from[2] + delta[2] * t)};
        const Vec3 local{point.x - cell[0], point.y - cell[1],
                         point.z - cell[2]};
        if (geometry.contains(local)) ++inside;
    }
    return double(inside) / samples;
}

double barrierLength(ChunkManager& chunks, const std::array<double, 3>& from,
                     const std::array<double, 3>& to) {
    const std::array<double, 3> delta{to[0] - from[0], to[1] - from[1],
                                      to[2] - from[2]};
    const double length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] +
                                    delta[2] * delta[2]);
    if (length <= 1e-9) return 0;

    int cell[3] = {int(std::floor(from[0])), int(std::floor(from[1])),
                   int(std::floor(from[2]))};
    int step[3] = {0, 0, 0};
    double tMax[3] = {1e300, 1e300, 1e300};
    double tDelta[3] = {1e300, 1e300, 1e300};
    for (int axis = 0; axis < 3; ++axis) {
        const double d = delta[axis];
        if (std::abs(d) <= 1e-12) continue;
        step[axis] = d > 0 ? 1 : -1;
        tDelta[axis] = 1.0 / std::abs(d);
        const double boundary = step[axis] > 0
                                    ? double(cell[axis] + 1) - from[axis]
                                    : from[axis] - double(cell[axis]);
        tMax[axis] = std::max(0.0, boundary) / std::abs(d);
    }

    double blocked = 0;
    double tEnter = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        double tLeave = std::min({tMax[0], tMax[1], tMax[2], 1.0});
        if (tLeave < tEnter) tLeave = tEnter;
        if (tLeave > tEnter)
            blocked +=
                solidFraction(chunks, cell, from, delta, tEnter, tLeave) *
                (tLeave - tEnter) * length;
        if (tLeave >= 1.0) break;
        for (int axis = 0; axis < 3; ++axis)
            if (std::abs(tMax[axis] - tLeave) < 1e-12) {
                cell[axis] += step[axis];
                tMax[axis] += tDelta[axis];
            }
        tEnter = tLeave;
    }
    return blocked;
}

}  // namespace

float RegionalHeat::offsetAt(double distance) const {
    if (!(radius > 0) || distance >= radius || deltaC == 0) return 0;
    const float t = float(1.0 - std::max(distance, 0.0) / radius);
    return deltaC * std::pow(t, falloff);
}

Query::Query(const World& world, const ThermalSourceRegistry& sources,
             BarrierFn barrier)
    : world(world), sources(sources), barrier(std::move(barrier)) {}

void Query::setRegional(std::vector<RegionalHeat> value) {
    regional_ = std::move(value);
}

void Query::addRegional(RegionalHeat value) { regional_.push_back(value); }

bool Query::removeRegional(uint64_t key) {
    const auto it =
        std::find_if(regional_.begin(), regional_.end(),
                     [key](const auto& entry) { return entry.key == key; });
    if (it == regional_.end()) return false;
    regional_.erase(it);
    return true;
}

void Query::clearRegional() { regional_.clear(); }

Sample Query::at(std::array<double, 3> position, double worldDays) const {
    Sample sample;
    const auto weather = world_weather::at(world, position[0], position[2],
                                           worldDays, position[1]);
    sample.ambientC = weather.temperatureC;

    const auto wrap = world.wrap();
    const double x = position[0], y = position[1], z = position[2];
    RoomSample roomSample;
    if (room && room(position, worldDays, roomSample) && roomSample.indoors) {
        sample.indoors = true;
        sample.roomC = roomSample.airC;
        sample.wallC = roomSample.wallC;
        sample.roomGroundC = roomSample.groundC;
        sample.roomVolume = roomSample.volume;
        sample.roomWallFaces = roomSample.wallFaces;
        sample.roomLeakFaces = roomSample.leakFaces;
        sample.roomApproximate = roomSample.approximate;
        sample.roomSources = roomSample.sources;
        sample.roomCommandC = roomSample.commandC;
        sample.roomSaturated = roomSample.saturated;
    } else {
        sources.forEachNear(
            x, z, ThermalEmission::MaxRadius, wrap.tiles,
            [&](ThermalSourceRegistry::Id, const ThermalSource& source) {
                if (!source.emission.emits()) return;
                const double dx = wrap.deltaX(source.position[0], x);
                const double dy = source.position[1] - y;
                const double dz = source.position[2] - z;
                const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance >= source.emission.radius) return;
                float value = source.offsetAt(float(distance));
                if (barrier) {
                    const std::array<double, 3> from{x + dx, y + dy, z + dz};
                    const double blocked = barrier(from, position);
                    value *= float(std::exp(-blocked / BarrierEfoldMetres));
                }
                sample.detailedC += value;
                ++sample.detailedSources;
            });
    }

    for (const auto& regional : regional_) {
        if (!regional.activeAt(worldDays)) continue;
        if (regional.key && sources.containsKey(regional.key)) continue;
        const double dx = wrap.deltaX(regional.position[0], x);
        const double dy = regional.position[1] - y;
        const double dz = regional.position[2] - z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= regional.radius) continue;
        sample.regionalC += regional.offsetAt(distance);
        ++sample.regionalSources;
    }

    sample.temperatureC =
        (sample.indoors ? sample.roomC : sample.ambientC + sample.detailedC) +
        sample.regionalC;
    return sample;
}

BarrierFn voxelBarrier(ChunkManager& chunks) {
    return [&chunks](std::array<double, 3> from, std::array<double, 3> to) {
        return barrierLength(chunks, from, to);
    };
}

}  // namespace thermal
