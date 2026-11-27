#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "chunk.hpp"
#include "reservoirs.hpp"
#include "tile_piece.hpp"
#include "water_surface.hpp"

class World;

// Surface ice driven by prolonged local temperature (docs/handoff/weather.md,
// step 19). Phase volume lives in Reservoirs beside the liquid it converts.
// This module only decides when a column gains or loses a stage and projects
// the resulting solid. Ice starts at the fractional mean waterline and grows
// down into the available water; a column shallower than the next stage
// freezes through first.
namespace surface_ice {

// Visible stage thicknesses in metres. Index 0 is no ice.
inline constexpr std::array<float, 7> StageThickness{0.f,  .05f, .10f, .25f,
                                                     .50f, .75f, 1.f};
inline constexpr uint8_t ThroughStage = 7;
// A feature-managed ice sheet or glacier is thicker than the ordinary 1 m
// stage ladder. Its exact thickness is the regional target.
inline constexpr uint8_t DeepStage = 8;
// Flowing reaches form ice more slowly than still water.
inline constexpr double FlowFreezeFactor = .25;
// Degrees per game hour. A prolonged cold spell advances one 5 cm stage in a
// few game hours at a few degrees below zero; the debug time controls make
// this observable without waiting a season.
inline constexpr double FreezeMetresPerHourPerC = .002;
inline constexpr double ThawMetresPerHourPerC = .004;
// Signed sub-stage progress, in millionths of a water unit per square metre.
// This keeps single-second updates non-zero while the ledger stays integral.
inline constexpr int64_t ProgressPerMetre =
    int64_t(IceSpecificGravity * WaterUnitsPerBlock * 1'000'000.0);

float stageThickness(uint8_t stage);
// Water equivalent of an ice layer over one square metre.
WaterVolume waterUnitsForMetres(double metres);

struct Column {
    int ground = -1;
    WaterSurface water;
};

// A seasonal, feature-aware target thickness in metres. Ice sheets and
// glaciers are deep, sea ice is one metre and ordinary cold water freezes to
// the 1 m stage. Bilinear sampling across region centres gives the smooth
// hand-over that the feature masks need at their boundaries.
class Regional {
   public:
    explicit Regional(const World& world) : world(world) {}

    // Recompute at most once per five game days. Returns true on a change.
    bool update(double worldDays);
    float targetAt(int tileX, int tileZ) const;
    bool featureAt(int tileX, int tileZ) const;
    bool deepAt(int tileX, int tileZ) const {
        return targetAt(tileX, tileZ) > 1.01f;
    }
    const std::vector<float>& targets() const { return values; }
    const std::vector<uint8_t>& features() const { return feature; }
    int width = 0;
    int height = 0;
    uint64_t revision() const { return revision_; }

   private:
    const World& world;
    std::vector<float> values;
    std::vector<uint8_t> feature;
    int64_t lastStep = INT64_MIN;
    uint64_t revision_ = 0;
};

// The owner supplies loaded raw solids and the current projected water at a
// column. Unloaded columns return nullopt from `column`.
class Terrain {
   public:
    virtual ~Terrain() = default;
    virtual std::optional<Column> column(int x, int z) const = 0;
    virtual std::optional<tile_geometry::CellGeometry> cell(
        TilePos pos) const = 0;
    virtual int wrapX(int x) const { return x; }
    virtual bool inBounds(int x, int z) const = 0;
    virtual int maxY() const = 0;
};

struct UpdateStats {
    size_t visited = 0;
    size_t changed = 0;
    WaterVolume frozen = 0;
    WaterVolume thawed = 0;
    std::vector<SurfaceKey> changedColumns;
};

class Simulation {
   public:
    using TemperatureFn =
        std::function<float(int x, int z, double waterline, double day)>;

    // Advances loaded water columns in a square around (centreX, centreZ).
    // `dtDays` is game time since the previous call. A zero or negative
    // temperature range does nothing.
    using ManagedFn = std::function<bool(int x, int z)>;
    UpdateStats update(Reservoirs& water, const Terrain& terrain,
                       const TemperatureFn& temperature, double day,
                       double dtDays, int centreX, int centreZ, int radius,
                       size_t budget = 4096,
                       const ManagedFn& featureManaged = {});
    // Gives cold, still-unseeded water in one chunk column an initial lid
    // from the current regional temperature. This is the broad footprint
    // that keeps ice from appearing only beside the camera; the detailed
    // update above refines it with local progress. Returns columns seeded.

   private:
    // Sub-stage progress that has not crossed the persistence threshold yet.
    // Keeping it here means the ledger write rate is bounded by visible
    // stage or 1 cm changes rather than by the frame cadence.
    std::map<SurfaceKey, int64_t> pendingProgress;
};

// Projects surface ice onto the same environment map as snow. The projection
// is rebuilt from the authoritative ledger; only changed chunk slabs compare
// unequal and trigger a remesh.
class IceCover {
   public:
    void rebuild(
        Reservoirs& water, const Terrain& terrain, uint8_t iceBlock,
        const std::set<std::pair<unsigned, unsigned>>* changed = nullptr,
        const std::function<float(int, int)>* regionalTarget = nullptr);
    void clear();
    const std::shared_ptr<const EnvironmentMap>& environment() const {
        return environment_;
    }
    EnvironmentCell cellAt(TilePos pos) const;
    int iceTop(int x, int z) const;
    // Debug/step-19 removal: cut one ice cell and remember the opening so a
    // regional rebuild does not immediately fill it again. Cuts are
    // session-local until step 20 owns their persistence.
    bool cut(TilePos pos);
    void cutColumn(int x, int z);
    const std::set<std::pair<int, int>>& cutColumns() const {
        return cutColumns_;
    }
    uint64_t cutRevision() const { return cutRevision_; }
    const std::vector<ChunkCoord>& changedChunks() const { return changed_; }
    uint64_t revision() const { return revision_; }
    // Ice tops in millionths of a metre for the distant height field.
    const std::map<std::pair<int, int>, int32_t>& tops() const { return tops_; }
    void fillTops(unsigned chunkX, unsigned chunkZ,
                  std::map<std::pair<int, int>, int32_t>& out) const;
    size_t cellCount() const;

   private:
    std::shared_ptr<const EnvironmentMap> environment_ =
        std::make_shared<const EnvironmentMap>();
    std::vector<ChunkCoord> changed_;
    std::map<std::pair<int, int>, int32_t> tops_;
    std::set<std::tuple<int, int, int>> holes_;
    std::set<std::pair<int, int>> cutColumns_;
    uint64_t revision_ = 0;
    uint64_t cutRevision_ = 0;
};

}  // namespace surface_ice
