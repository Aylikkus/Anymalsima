#include "save_catalog.hpp"

#include <algorithm>
#include <iostream>

#include "serialization/file_io.hpp"
#include "trees_codec.hpp"
#include "world/generated_hydrology.hpp"
#include "world/snow_settlement.hpp"
#include "world_map_codec.hpp"

namespace save_catalog {
namespace {

constexpr const char* MetaFile = "world.json";
constexpr const char* MapFile = "world_map.dat";
constexpr const char* EditsFile = "terrain_edits.dat";
constexpr const char* LightsFile = "light_sources.dat";
constexpr const char* StructuresFile = "structures.dat";
constexpr const char* TreesFile = "trees.dat";
constexpr const char* SkyFile = "sky.dat";
constexpr const char* SnowFile = "snow.dat";

std::string folderName(uint64_t seed, const std::string& createdAt) {
    std::string stamp = createdAt;  // YYYY-MM-DDTHH:MM:SS
    std::replace(stamp.begin(), stamp.end(), 'T', '_');
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    return std::to_string(seed) + "_" + stamp;
}

}  // namespace

std::vector<SaveSlot> list(const std::filesystem::path& root) {
    std::vector<SaveSlot> slots;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return slots;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const auto metaPath = entry.path() / MetaFile;
        if (!std::filesystem::is_regular_file(metaPath)) continue;
        try {
            SaveSlot slot{entry.path(), {}};
            slot.meta.deserialize(FileIO::readJson(metaPath.string()));
            if (!std::filesystem::is_regular_file(entry.path() / MapFile))
                throw std::runtime_error("missing world_map.dat");
            slots.push_back(std::move(slot));
        } catch (const std::exception& e) {
            std::cerr << "Skipping save " << entry.path() << ": " << e.what()
                      << '\n';
        }
    }
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) {
        return a.meta.lastPlayed > b.meta.lastPlayed;
    });
    return slots;
}

SaveSlot create(const World& world, Vec2u startRegion,
                const std::filesystem::path& root) {
    SaveSlot slot;
    slot.meta.seed = world.genSettings.seed;
    slot.meta.width = world.width;
    slot.meta.height = world.height;
    slot.meta.createdAt = WorldMeta::now();
    slot.meta.lastPlayed = slot.meta.createdAt;
    slot.meta.startRegion = startRegion;
    slot.meta.worldDays = 8.0 / 24.0;  // Start new worlds at 08:00.

    const auto base = folderName(slot.meta.seed, slot.meta.createdAt);
    FileIO::ensureDirectory(root);
    for (int attempt = 1;; ++attempt) {
        auto candidate =
            root / (attempt == 1 ? base : base + "_" + std::to_string(attempt));
        // create_directory returns false when the folder already exists.
        if (std::filesystem::create_directory(candidate)) {
            slot.directory = std::move(candidate);
            break;
        }
    }
    FileIO::ensureDirectory(slot.directory / "regions");
    writeWorldMap(slot, world);
    writeMeta(slot);
    return slot;
}

void writeMeta(const SaveSlot& slot) {
    FileIO::writeJsonAtomic(slot.directory / MetaFile, slot.meta.serialize());
}

void writeWorldMap(const SaveSlot& slot, const World& world) {
    FileIO::writeBytesAtomic(slot.directory / MapFile,
                             world_map_codec::encode(world));
}

std::unique_ptr<World> readWorldMap(const SaveSlot& slot) {
    const auto bytes = FileIO::readBytes(slot.directory / MapFile);
    auto world = std::make_unique<World>(world_map_codec::decode(bytes));
    world->generatorVersion = slot.meta.generatorVersion;
    if (slot.meta.generatorVersion >= WorldMeta::HydrologyVersion)
        world->hydrology = GeneratedHydrology::build(
            *world, {}, slot.meta.generatorVersion >= WorldMeta::RiverVersion,
            slot.meta.generatorVersion >= WorldMeta::NaturalRiverVersion,
            slot.meta.generatorVersion >= WorldMeta::BoundedRiverVersion,
            slot.meta.generatorVersion >= WorldMeta::RiverSedimentVersion,
            slot.meta.generatorVersion >= WorldMeta::LakeOutletVersion,
            slot.meta.generatorVersion >= WorldMeta::CoastalBasinVersion,
            slot.meta.generatorVersion >= WorldMeta::OceanShoreVersion);
    return world;
}

terrain_edits_codec::File readTerrainEdits(const SaveSlot& slot) {
    const auto path = slot.directory / EditsFile;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    return terrain_edits_codec::decode(FileIO::readBytes(path));
}

void writeTerrainEdits(const SaveSlot& slot, const EditMap& columns,
                       const SectionContainer& extra) {
    FileIO::writeBytesAtomic(slot.directory / EditsFile,
                             terrain_edits_codec::encode(columns, extra));
}

light_sources_codec::File readLightSources(const SaveSlot& slot) {
    const auto path = slot.directory / LightsFile;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    return light_sources_codec::decode(FileIO::readBytes(path));
}

void writeLightSources(const SaveSlot& slot,
                       std::vector<light_sources_codec::Record> lights,
                       const SectionContainer& extra) {
    FileIO::writeBytesAtomic(
        slot.directory / LightsFile,
        light_sources_codec::encode(std::move(lights), extra));
}

sky_codec::File readSky(const SaveSlot& slot) {
    const auto path = slot.directory / SkyFile;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    return sky_codec::decode(FileIO::readBytes(path));
}

void readSnow(const SaveSlot& slot, snow::SnowSettlement& snow) {
    const auto path = slot.directory / SnowFile;
    if (std::filesystem::exists(path) && !snow.restore(FileIO::readBytes(path)))
        throw std::runtime_error("Invalid snow.dat");
}

void writeSnow(const SaveSlot& slot, const snow::SnowSettlement& snow) {
    FileIO::writeBytesAtomic(slot.directory / SnowFile, snow.encode());
}

void writeSky(const SaveSlot& slot, const sky::Overrides& overrides,
              const SectionContainer& extra) {
    FileIO::writeBytesAtomic(slot.directory / SkyFile,
                             sky_codec::encode(overrides, extra));
}

structures_codec::File readStructures(const SaveSlot& slot) {
    const auto path = slot.directory / StructuresFile;
    if (std::filesystem::exists(path))
        return structures_codec::decode(FileIO::readBytes(path));
    const auto legacy = slot.directory / TreesFile;
    if (!std::filesystem::exists(legacy)) return {};
    auto trees = trees_codec::decode(FileIO::readBytes(legacy));
    return {std::move(trees.trees), {}, std::move(trees.extra), true};
}

void writeStructures(const SaveSlot& slot, const TreeOverrideMap& trees,
                     const structure_voxels::Overrides& voxels,
                     const SectionContainer& extra) {
    FileIO::writeBytesAtomic(slot.directory / StructuresFile,
                             structures_codec::encode(trees, voxels, extra));
}

void readReservoirs(const SaveSlot& slot, Reservoirs& reservoirs) {
    const auto path = slot.directory / "water.dat";
    if (std::filesystem::exists(path))
        reservoirs.restore(FileIO::readBytes(path));
}

void writeReservoirs(const SaveSlot& slot, const Reservoirs& reservoirs) {
    FileIO::writeBytesAtomic(slot.directory / "water.dat", reservoirs.encode());
}

}  // namespace save_catalog
