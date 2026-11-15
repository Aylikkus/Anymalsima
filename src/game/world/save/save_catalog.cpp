#include "save_catalog.hpp"

#include <algorithm>
#include <iostream>

#include "serialization/file_io.hpp"
#include "world_map_codec.hpp"

namespace save_catalog {
namespace {

constexpr const char* MetaFile = "world.json";
constexpr const char* MapFile = "world_map.dat";
constexpr const char* EditsFile = "terrain_edits.dat";

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
    return std::make_unique<World>(world_map_codec::decode(bytes));
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

}  // namespace save_catalog
