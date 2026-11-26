#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include "thermal_sources.hpp"

class World;
class ChunkManager;

// Local °C for snow, ice and future actors (docs/handoff/weather.md, step 15).
// The sample combines the dated outdoor weather at the position with nearby
// signed thermal sources. Distance attenuation uses the source's own falloff;
// solids in the straight path attenuate the contribution by an exponential
// barrier length. The query is main-thread only.
namespace thermal {

// Blocked length in metres over which a source's contribution falls by 1/e.
// Five centimetres is enough to make a one-metre wall effectively opaque while
// leaving a thin pane or a partly open doorway useful.
inline constexpr double BarrierEfoldMetres = .05;

// Returns the length of solid material between the two world-space points.
using BarrierFn =
    std::function<double(std::array<double, 3>, std::array<double, 3>)>;

// A dated contribution for a source too large or too far away for detailed
// voxel sampling, such as a future large fire or landmark geyser. Producers
// share a `key` with their detailed ThermalSource so the two representations
// never both contribute.
struct RegionalHeat {
    uint64_t key = 0;
    std::array<double, 3> position{0, 0, 0};
    float deltaC = 0;
    float radius = 0;
    float falloff = 1;
    double startDay = std::numeric_limits<double>::lowest();
    double endDay = std::numeric_limits<double>::max();

    bool activeAt(double worldDays) const {
        return worldDays >= startDay && worldDays <= endDay;
    }
    float offsetAt(double distance) const;
};

struct Sample {
    float ambientC = 0;      // dated outdoor weather at the point
    float detailedC = 0;     // sum of local registered sources
    float regionalC = 0;     // sum of active dated regional entries
    float temperatureC = 0;  // ambient + detailed + regional
    size_t detailedSources = 0;
    size_t regionalSources = 0;
};

class Query {
   public:
    Query(const World& world, const ThermalSourceRegistry& sources,
          BarrierFn barrier = {});
    ~Query() = default;

    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&&) = delete;
    Query& operator=(Query&&) = delete;

    Sample at(std::array<double, 3> position, double worldDays) const;
    float temperatureC(std::array<double, 3> position, double worldDays) const {
        return at(position, worldDays).temperatureC;
    }

    // Session/owner-scoped dated contributions. They are not saved here; the
    // owning future system persists its own summary and calls this interface.
    void setRegional(std::vector<RegionalHeat> value);
    void addRegional(RegionalHeat value);
    bool removeRegional(uint64_t key);
    void clearRegional();
    const std::vector<RegionalHeat>& regional() const { return regional_; }

   private:
    const World& world;
    const ThermalSourceRegistry& sources;
    BarrierFn barrier;
    std::vector<RegionalHeat> regional_;
};

// Builds a barrier probe from loaded voxel geometry. Unloaded cells are
// treated as open: local sources only matter inside the resident detail area,
// and the dated regional interface covers the rest.
BarrierFn voxelBarrier(ChunkManager& chunks);

}  // namespace thermal
