#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chunk_manager.hpp"
#include "core/signal.hpp"
#include "local_temperature.hpp"
#include "tile_pos.hpp"

class World;
class ChunkManager;
class WorldEditor;
struct BlockChange;

// Lumped thermal model for connected indoor air (docs/handoff/weather.md,
// step 16). Only air cells that are sheltered by a solid ceiling are part of
// a room; an open doorway or a missing roof piece becomes a leak face rather
// than merging the room with the whole outdoors. Each room has one air
// temperature and one wall temperature. The wall is a finite heat store with
// its own capacity, so it warms and cools more slowly than the air and can
// release heat after the source is removed.
//
// The simulation is main-thread only and scans a bounded window around the
// focus. A flood fill stops at the scan budget; every face that touches an
// unloaded cell is counted as an approximate leak. Geometry notifications
// invalidate the rooms, and the owner rescan is coalesced.
//
// Units: temperatures in °C, time in game seconds, capacities in J/K and
// conductances in W/K. The constants below are deliberately effective values
// for a game cube rather than a building-physics model; they are documented
// in docs/modding/temperature.md.
namespace room {

struct Definition {
    uint64_t key = 0;  // stable representative key (minimum cell)
    TilePos minCell{};
    int airCells = 0;
    int wallFaces = 0;
    int leakFaces = 0;
    int approximateFaces = 0;
    std::array<double, 3> centroid{0, 0, 0};
    std::array<int, 6> bounds{};  // min x,y,z, max x,y,z (inclusive)
    std::vector<TilePos> cells;
};

struct State {
    double airC = 0;
    double wallC = 0;
    double groundC = 0;
    double lastDay = 0;
    bool initialized = false;
};

struct Runtime {
    size_t sources = 0;
    float commandC = 0;
    bool saturated = false;
};

struct Record {
    int x = 0;
    int y = 0;
    int z = 0;
    float airC = 0;
    float wallC = 0;
    double lastDay = 0;
    bool operator==(const Record&) const = default;
};

struct Parameters {
    // Effective heat capacities. Air includes furnishings; the wall store is
    // the participating near-surface layer rather than a whole mountain of
    // stone, so a small room responds within minutes instead of hours.
    double airCapacityPerVolume = 60'000.;  // J/K per m³
    double wallCapacityPerArea = 120'000.;  // J/K per m² of wall face
    // Air-to-wall, air-to-outdoors (leak) and wall-to-deep-ground
    // conductances per square metre of contact.
    double wallAirConductance = 25.;  // W/K per m²
    double leakConductance = 1500.;   // W/K per m² for an open face
    double groundConductance = 5.;    // W/K per m² (sheltered insulation)
    // A source offset is a target temperature; the air approaches it with
    // this coupling time instead of instantaneously.
    double sourceCouplingSeconds = 120.;
    // A room cell is sheltered if a solid exists within this many metres
    // above it. Taller halls are outside V1.
    int ceilingHeight = 12;
    // Active scan window around the focus and its hard work bounds.
    int scanRadius = 24;
    size_t maxRoomCells = 8192;
    size_t maxScanCells = 32768;
    size_t maxRooms = 256;
    // Rescan when the focus moves this far or after a geometry change.
    double rescanDistance = 6.;
    // Only rooms whose air or wall store differs from the ground baseline by
    // more than this are persisted.
    double storeThresholdC = .25;
    // Thermostat/air integration cadence. Commands are recalculated at each
    // step, so an accelerated clock cannot leave a step large enough to
    // oscillate the controller.
    double maxStepSeconds = 5.;
    int maxSubsteps = 4096;
};

class RoomAir {
   public:
    RoomAir(const World& world, ChunkManager& chunks,
            ThermalSourceRegistry& sources, thermal::BarrierFn barrier = {});
    ~RoomAir();

    RoomAir(const RoomAir&) = delete;
    RoomAir& operator=(const RoomAir&) = delete;

    // Geometry and tile changes invalidate the current rooms. Chunk arrival
    // and unload also change the bounded approximation.
    void watch(WorldEditor& editor);
    void invalidate();
    bool needsRebuild() const { return rebuildPending; }

    // Advances all known rooms to `worldDays` using the game-time gap since
    // the room's own last day. `focus` bounds the next scan. Call at the room
    // cadence (a few times per second), not every frame.
    void update(std::array<double, 3> focus, double worldDays);

    // Room provider for thermal::Query. Returns false outdoors or when the
    // point is outside the currently scanned rooms.
    bool sample(std::array<double, 3> position, double worldDays,
                thermal::RoomSample& out) const;

    // Inspection helper for the tile panel and the fixture controls.
    struct Debug {
        bool valid = false;
        uint64_t key = 0;
        float airC = 0;
        float wallC = 0;
        float groundC = 0;
        int volume = 0;
        int wallFaces = 0;
        int leakFaces = 0;
        int approximateFaces = 0;
        size_t sources = 0;
        float commandC = 0;
        bool saturated = false;
        double lastDay = 0;
    };
    Debug debugAt(std::array<double, 3> position) const;
    // Nearest scanned room within `maxDistance` of a point, for inspecting a
    // room from outside it (the fixture camera or a hovered boundary block).
    Debug nearest(std::array<double, 3> position, double maxDistance) const;
    size_t roomCount() const { return definitions.size(); }
    const std::vector<Definition>& rooms() const { return definitions; }

    // Significant stored heat and its timestamp. Rooms that have cooled to
    // the ground baseline are omitted; loading starts them there.
    std::vector<Record> saveRecords() const;
    void loadRecords(const std::vector<Record>& records);
    bool dirty() const { return stateDirty; }
    void clearDirty() { stateDirty = false; }

    const Parameters& parameters() const { return params; }

   private:
    void rebuild(std::array<double, 3> focus);
    bool sheltered(int x, int y, int z);
    bool loadedAir(int x, int y, int z, bool& loaded) const;
    void simulate(const Definition& definition, State& state, double worldDays,
                  std::unordered_set<uint64_t>& claimedThermostats);
    void updateUnclaimedThermostats(
        double worldDays, const std::unordered_set<uint64_t>& claimed);
    double outdoorAt(double x, double y, double z, double worldDays) const;
    double groundAt(double x, double y, double z) const;
    State& stateFor(const Definition& definition, double worldDays);
    void fillDebug(const Definition& definition, Debug& debug) const;

    const World& world;
    ChunkManager& chunks;
    ThermalSourceRegistry& sources;
    thermal::BarrierFn barrier;
    Parameters params;

    std::vector<Definition> definitions;
    std::unordered_map<uint64_t, State> states;
    std::unordered_map<uint64_t, Runtime> runtime;
    std::unordered_map<uint64_t, ThermalControllerState> controllers;
    std::unordered_map<TilePos, size_t> cellRoom;
    std::unordered_set<TilePos> shelterCache;
    std::unordered_set<TilePos> exposedCache;

    std::array<double, 3> lastFocus{0, 0, 0};
    bool hasFocus = false;
    bool rebuildPending = true;
    bool stateDirty = false;
    uint64_t topology = 0;

    Signal<const ChunkCoord&>::Connection readyConnection;
    Signal<const ChunkCoord&>::Connection removedConnection;
    Signal<const BlockChange&>::Connection editorConnection;
    WorldEditor* watchedEditor = nullptr;
};

}  // namespace room
