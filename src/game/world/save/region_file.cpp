#include "region_file.hpp"

#include <algorithm>
#include <string>

#include "chunk_codec.hpp"

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
}  // namespace

std::filesystem::path path(const std::filesystem::path& worldDir, Vec2u rc) {
    return worldDir / "regions" /
           ("r." + std::to_string(rc.x) + "." + std::to_string(rc.y) + ".dat");
}

Bytes encode(const RegionData& data) {
    SectionContainer container = data.extra;

    std::vector<uint16_t> order;
    order.reserve(data.chunks.size());
    for (const auto& [index, blob] : data.chunks) order.push_back(index);
    std::sort(order.begin(), order.end());  // stable, diff-friendly output

    BinaryWriter chunks;
    chunks.write(uint32_t(order.size()));
    for (const auto index : order) {
        chunks.write(index);
        chunks.writeBlock(*data.chunks.at(index));
    }
    container.put("CHNK", ChunkSectionVersion, chunks.take(), false);

    BinaryWriter tops;
    for (const auto top : data.tops) tops.write(top);
    container.put("TOPS", TopsSectionVersion, tops.take());
    return container.encode();
}

RegionData decode(ByteView bytes) {
    RegionData data;
    data.extra = SectionContainer::decode(bytes);

    if (const auto* section = data.extra.find("CHNK")) {
        if (section->version != ChunkSectionVersion)
            throw SerializationError("unsupported CHNK version");
        BinaryReader in(section->data);
        const auto count = in.read<uint32_t>();
        if (count > RegionData::ChunkCount)
            throw SerializationError("too many chunks in region");
        for (uint32_t i = 0; i < count; ++i) {
            const auto index = in.read<uint16_t>();
            if (index >= RegionData::ChunkCount)
                throw SerializationError("chunk index out of range");
            const auto blob = in.readBlock(1u << 20);
            data.chunks[index] =
                std::make_shared<const Bytes>(blob.begin(), blob.end());
        }
    }
    if (const auto* section = data.extra.find("TOPS")) {
        if (section->version != TopsSectionVersion)
            throw SerializationError("unsupported TOPS version");
        BinaryReader in(section->data);
        for (auto& top : data.tops) top = in.read<int32_t>();
    }
    // Known sections are rebuilt from fields on encode.
    data.extra.remove("CHNK");
    data.extra.remove("TOPS");
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
    return result;
}

}  // namespace region_file
