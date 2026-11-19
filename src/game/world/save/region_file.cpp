#include "region_file.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

#include "chunk_codec.hpp"
#include "flora_codec.hpp"
#include "world/structure_voxels.hpp"

uint16_t RegionData::localIndex(const ChunkCoord& coord) {
    return uint16_t(coord.y * Columns +
                    (coord.z % CHUNKS_PER_REGION) * CHUNKS_PER_REGION +
                    coord.x % CHUNKS_PER_REGION);
}

ChunkCoord RegionData::worldCoord(Vec2u region, uint16_t index) {
    const unsigned col = index % Columns;
    return {region.x * CHUNKS_PER_REGION + col % CHUNKS_PER_REGION,
            unsigned(index / Columns),
            region.y * CHUNKS_PER_REGION + col / CHUNKS_PER_REGION};
}

namespace region_file {
namespace {
constexpr uint16_t ChunkSectionVersion = 1;
constexpr uint16_t TopsSectionVersion = 1;
constexpr uint16_t FloraSectionVersion = 1;

void writeBlobs(SectionContainer& container, const char* tag, uint16_t version,
                const std::unordered_map<uint16_t, ChunkBlob>& blobs) {
    std::vector<uint16_t> order;
    order.reserve(blobs.size());
    for (const auto& [index, blob] : blobs) order.push_back(index);
    std::sort(order.begin(), order.end());  // stable, diff-friendly output
    BinaryWriter out;
    out.write(uint32_t(order.size()));
    for (const auto index : order) {
        out.write(index);
        out.writeBlock(*blobs.at(index));
    }
    container.put(tag, version, out.take(), false);
}

void readBlobs(const SectionContainer& container, const char* tag,
               uint16_t version,
               std::unordered_map<uint16_t, ChunkBlob>& blobs) {
    const auto* section = container.find(tag);
    if (!section) return;
    if (section->version != version)
        throw SerializationError(std::string("unsupported ") + tag +
                                 " version");
    BinaryReader in(section->data);
    const auto count = in.read<uint32_t>();
    if (count > RegionData::ChunkCount)
        throw SerializationError("too many chunks in region");
    for (uint32_t i = 0; i < count; ++i) {
        const auto index = in.read<uint16_t>();
        if (index >= RegionData::ChunkCount)
            throw SerializationError("chunk index out of range");
        const auto blob = in.readBlock(1u << 20);
        blobs[index] = std::make_shared<const Bytes>(blob.begin(), blob.end());
    }
}
}  // namespace

std::filesystem::path path(const std::filesystem::path& worldDir, Vec2u rc) {
    return worldDir / "regions" /
           ("r." + std::to_string(rc.x) + "." + std::to_string(rc.y) + ".dat");
}

Bytes encode(const RegionData& data) {
    SectionContainer container = data.extra;

    writeBlobs(container, "CHNK", ChunkSectionVersion, data.chunks);
    if (!data.flora.empty())
        writeBlobs(container, "FLOR", FloraSectionVersion, data.flora);

    if (!data.structureBaselines.empty())
        writeBlobs(container, "STRB", 1, data.structureBaselines);

    BinaryWriter tops;
    for (const auto top : data.tops) tops.write(top);
    container.put("TOPS", TopsSectionVersion, tops.take());
    return container.encode();
}

RegionData decode(ByteView bytes) {
    RegionData data;
    data.extra = SectionContainer::decode(bytes);

    readBlobs(data.extra, "CHNK", ChunkSectionVersion, data.chunks);
    readBlobs(data.extra, "FLOR", FloraSectionVersion, data.flora);
    readBlobs(data.extra, "STRB", 1, data.structureBaselines);
    if (const auto* section = data.extra.find("TOPS")) {
        if (section->version != TopsSectionVersion)
            throw SerializationError("unsupported TOPS version");
        BinaryReader in(section->data);
        for (auto& top : data.tops) top = in.read<int32_t>();
    }
    // Known sections are rebuilt from fields on encode.
    data.extra.remove("CHNK");
    data.extra.remove("TOPS");
    data.extra.remove("FLOR");
    data.extra.remove("STRB");
    return data;
}

RegionData merge(const RegionData& base, const RegionSnapshot& snapshot) {
    RegionData result = base;
    for (const auto& [index, chunk] : snapshot.chunks) {
        result.chunks[index] =
            std::make_shared<const Bytes>(chunk_codec::encode(*chunk));
        const int top = chunk_codec::highestSolidY(*chunk);
        if (top >= 0) {
            const int worldY =
                int(index / RegionData::Columns) * Chunk::SIZE_Y + top;
            auto& column = result.tops[RegionData::column(index)];
            column = std::max(column, int32_t(worldY));
        }
    }
    std::unordered_map<uint16_t, TreeStamp> stamps(snapshot.trees.begin(),
                                                   snapshot.trees.end());
    for (const auto& [index, layer] : snapshot.flora) {
        const auto stamp = stamps.find(index);
        result.flora[index] = std::make_shared<const Bytes>(flora_codec::encode(
            layer ? *layer : FloraLayer{},
            stamp != stamps.end() ? stamp->second : TreeStamp{}));
    }
    static const auto empty = std::make_shared<const Bytes>();
    for (const auto& [index, slab] : snapshot.structureBaselines) {
        if (!slab) {
            result.structureBaselines[index] = empty;
            continue;
        }
        Chunk baseline;
        structure_voxels::stamp(baseline, *slab);
        result.structureBaselines[index] =
            std::make_shared<const Bytes>(chunk_codec::encode(baseline));
    }
    return result;
}

}  // namespace region_file
