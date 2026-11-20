#include "terrain_edits_codec.hpp"

#include <algorithm>

#include "world/consts.hpp"

namespace terrain_edits_codec {
namespace {
constexpr uint16_t EditSectionVersion = 3;
constexpr uint16_t TilesPerSlab = Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z;
constexpr uint32_t MaxColumns = 1u << 24;
}  // namespace

Bytes encode(const EditMap& columns, const SectionContainer& extra) {
    std::vector<uint64_t> keys;
    keys.reserve(columns.size());
    for (const auto& [key, edit] : columns) keys.push_back(key);
    std::sort(keys.begin(), keys.end());  // stable, diff-friendly output

    BinaryWriter out;
    out.write(uint32_t(keys.size()));
    for (const auto key : keys) {
        const auto& edit = *columns.at(key);
        out.write(uint32_t(key >> 32));
        out.write(uint32_t(key));
        for (const auto y : edit.groundY) out.write(y);
        for (const auto block : edit.groundBlock) out.write(block);
        for (const auto y : edit.waterY) out.write(y);
        for (const auto y : edit.reservoirCeiling) out.write(y);
        out.write(uint16_t(edit.structure.size()));
        for (const auto& [slabY, blob] : edit.structure) {
            out.write(slabY);
            out.writeBlock(*blob);
        }
        out.write(uint16_t(edit.removed.size()));
        for (const auto& [slabY, cells] : edit.removed) {
            out.write(slabY);
            out.write(uint16_t(cells.size()));
            for (const auto cell : cells) out.write(cell);
        }
    }
    SectionContainer container = extra;
    container.put("EDIT", EditSectionVersion, out.take());
    return container.encode();
}

File decode(ByteView bytes) {
    File file;
    file.extra = SectionContainer::decode(bytes);
    if (const auto* section = file.extra.find("EDIT")) {
        if (section->version < 1 || section->version > EditSectionVersion)
            throw SerializationError("unsupported EDIT version");
        BinaryReader in(section->data);
        const auto count = in.read<uint32_t>();
        if (count > MaxColumns)
            throw SerializationError("too many edited columns");
        for (uint32_t i = 0; i < count; ++i) {
            const auto chunkX = in.read<uint32_t>();
            const auto chunkZ = in.read<uint32_t>();
            auto edit = std::make_shared<ColumnEdit>();
            for (auto& y : edit->groundY) {
                y = in.read<int16_t>();
                if (y < -1 || y >= WORLD_HEIGHT_Y)
                    throw SerializationError("ground height out of range");
            }
            for (auto& block : edit->groundBlock) block = in.read<uint8_t>();
            for (auto& y : edit->waterY) {
                y = in.read<int16_t>();
                if (y < (section->version >= 3 ? ColumnEdit::GeneratedWater
                                               : -1) ||
                    y > WORLD_HEIGHT_Y)
                    throw SerializationError("water height out of range");
            }
            if (section->version >= 3)
                for (auto& y : edit->reservoirCeiling) {
                    y = in.read<int16_t>();
                    if (y < -1 || y > WORLD_HEIGHT_Y)
                        throw SerializationError(
                            "reservoir ceiling out of range");
                }
            const auto slabs = in.read<uint16_t>();
            if (slabs > Y_CHUNKS)
                throw SerializationError("too many structure slabs");
            for (uint16_t s = 0; s < slabs; ++s) {
                const auto slabY = in.read<uint16_t>();
                if (slabY >= Y_CHUNKS)
                    throw SerializationError("structure slab out of range");
                const auto blob = in.readBlock(1u << 20);
                edit->structure.emplace_back(
                    slabY,
                    std::make_shared<const Bytes>(blob.begin(), blob.end()));
            }
            if (section->version >= 2) {
                const auto removedSlabs = in.read<uint16_t>();
                if (removedSlabs > Y_CHUNKS)
                    throw SerializationError("too many removal slabs");
                for (uint16_t s = 0; s < removedSlabs; ++s) {
                    const auto y = in.read<uint16_t>();
                    const auto count = in.read<uint16_t>();
                    if (y >= Y_CHUNKS || !count || count > TilesPerSlab ||
                        (!edit->removed.empty() &&
                         y <= edit->removed.back().first))
                        throw SerializationError("invalid removal slab");
                    auto& cells =
                        edit->removed.emplace_back(y, std::vector<uint16_t>{})
                            .second;
                    cells.reserve(count);
                    for (uint16_t c = 0; c < count; ++c) {
                        const auto cell = in.read<uint16_t>();
                        if (cell >= TilesPerSlab ||
                            (!cells.empty() && cell <= cells.back()))
                            throw SerializationError("invalid removed voxel");
                        cells.push_back(cell);
                    }
                }
            }
            // Decodes every blob, so corrupt payloads are rejected here.
            edit->finish();
            if (!file.columns.emplace(editKey(chunkX, chunkZ), std::move(edit))
                     .second)
                throw SerializationError("duplicate edited column");
        }
        if (!in.atEnd()) throw SerializationError("trailing EDIT data");
    }
    // Known sections are rebuilt from fields on encode.
    file.extra.remove("EDIT");
    return file;
}

}  // namespace terrain_edits_codec
