#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "light_sources_codec.hpp"
#include "sky_codec.hpp"
#include "structures_codec.hpp"
#include "terrain_edits_codec.hpp"
#include "thermal_sources_codec.hpp"
#include "world/world.hpp"
#include "world_meta.hpp"

// One world folder on disk.
struct SaveSlot {
    std::filesystem::path directory;
    WorldMeta meta;
};

namespace snow {
class SnowSettlement;
}

// Discovers, creates, and reads world folders:
//   saves/<seed>_<YYYY-MM-DD_HH-MM-SS>/
//     world.json      WorldMeta
//     world_map.dat   world_map_codec
//     regions/        region_file per region
namespace save_catalog {

inline const std::filesystem::path DefaultRoot = "saves";

// Valid slots, most recently played first. Broken folders are skipped.
std::vector<SaveSlot> list(const std::filesystem::path& root = DefaultRoot);
// Creates a unique folder for a new world and fills createdAt/lastPlayed.
SaveSlot create(const World& world, Vec2u startRegion,
                const std::filesystem::path& root = DefaultRoot);
void writeMeta(const SaveSlot& slot);
void readReservoirs(const SaveSlot& slot, Reservoirs& reservoirs);
void writeReservoirs(const SaveSlot& slot, const Reservoirs& reservoirs);
void readSnow(const SaveSlot& slot, snow::SnowSettlement& snow);
void writeSnow(const SaveSlot& slot, const snow::SnowSettlement& snow);
void writeWorldMap(const SaveSlot& slot, const World& world);
// Loads the world map. Throws on missing or corrupt data.
std::unique_ptr<World> readWorldMap(const SaveSlot& slot);
// terrain_edits.dat: empty when the file does not exist yet. Throws on
// corrupt data.
terrain_edits_codec::File readTerrainEdits(const SaveSlot& slot);
void writeTerrainEdits(const SaveSlot& slot, const EditMap& columns,
                       const SectionContainer& extra = {});
// light_sources.dat: empty when the file does not exist yet. Throws on
// corrupt data.
light_sources_codec::File readLightSources(const SaveSlot& slot);
void writeLightSources(const SaveSlot& slot,
                       std::vector<light_sources_codec::Record> lights,
                       const SectionContainer& extra = {});
// thermal_sources.dat: movable item sources. Block sources are derived from
// tiles. Empty when the file does not exist yet; throws on corrupt data.
thermal_sources_codec::File readThermalSources(const SaveSlot& slot);
void writeThermalSources(const SaveSlot& slot,
                         std::vector<thermal_sources_codec::Record> records,
                         const SectionContainer& extra = {});
// sky.dat (sky heightmap overrides): empty when the file does not exist
// yet. Throws on corrupt data.
sky_codec::File readSky(const SaveSlot& slot);
void writeSky(const SaveSlot& slot, const sky::Overrides& overrides,
              const SectionContainer& extra = {});
// structures.dat takes precedence. A missing file imports legacy trees.dat.
structures_codec::File readStructures(const SaveSlot& slot);
void writeStructures(const SaveSlot& slot, const TreeOverrideMap& trees,
                     const structure_voxels::Overrides& voxels,
                     const SectionContainer& extra = {});

}  // namespace save_catalog
