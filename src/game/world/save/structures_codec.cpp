#include "structures_codec.hpp"

#include "piece_codec.hpp"
#include "trees_codec.hpp"

namespace structures_codec {
namespace {
constexpr uint16_t Version = 3;
constexpr uint32_t MaxKinds = 4096;
constexpr uint32_t MaxRecords = 1u << 20;
constexpr uint32_t MaxVoxels = 1u << 22;

void validate(std::string_view kind, const structure_voxels::Override& record) {
    try {
        structure_voxels::validateOverride(kind, record);
    } catch (const std::invalid_argument& e) {
        throw SerializationError(e.what());
    }
}
}  // namespace

Bytes encode(const TreeOverrideMap& trees,
             const structure_voxels::Overrides& voxels,
             const SectionContainer& extra) {
    BinaryWriter out;
    if (voxels.size() > MaxKinds) throw SerializationError("too many kinds");
    out.write(uint32_t(voxels.size()));
    std::unordered_set<uint64_t> keys;
    for (const auto& [kind, records] : voxels) {
        if (kind.empty() || kind.size() > 64 || records.size() > MaxRecords)
            throw SerializationError("invalid structure kind");
        out.writeString(kind);
        out.write(uint32_t(records.size()));
        for (const auto& [key, record] : records) {
            if (!keys.insert(key).second)
                throw SerializationError("duplicate structure key");
            validate(kind, record);
            if (record.voxels.size() > MaxVoxels)
                throw SerializationError("too many override voxels");
            out.write(key);
            out.write(record.revision);
            out.write(uint8_t(record.mode));
            out.write(int32_t(record.anchorX));
            out.write(int32_t(record.anchorZ));
            out.write(uint8_t(record.landmark));
            out.write(uint32_t(record.voxels.size()));
            for (const auto& [pos, voxel] : record.voxels) {
                for (const int axis : pos) out.write(int32_t(axis));
                out.write(voxel.block);
                out.write(voxel.shape.code());
                out.write(uint8_t(voxel.pieces.size()));
                for (const auto& piece : voxel.pieces)
                    piece_codec::write(out, piece);
            }
        }
    }
    auto sections = extra;
    sections.put("VOXL", Version, out.take());
    return trees_codec::encode(trees, sections);
}

File decode(ByteView bytes) {
    auto trees = trees_codec::decode(bytes);
    File file{std::move(trees.trees), {}, std::move(trees.extra)};
    const auto* section = file.extra.find("VOXL");
    if (!section) return file;
    if (section->version < 1 || section->version > Version)
        throw SerializationError("unsupported VOXL version");
    BinaryReader in(section->data);
    const auto kinds = in.read<uint32_t>();
    if (kinds > MaxKinds) throw SerializationError("too many kinds");
    std::unordered_set<uint64_t> keys;
    for (uint32_t i = 0; i < kinds; ++i) {
        auto kind = in.readString(64);
        const auto count = in.read<uint32_t>();
        if (kind.empty() || count > MaxRecords || file.voxels.contains(kind))
            throw SerializationError("invalid structure kind");
        auto& records = file.voxels[kind];
        for (uint32_t j = 0; j < count; ++j) {
            const auto key = in.read<uint64_t>();
            structure_voxels::Override record;
            record.revision = in.read<uint64_t>();
            record.mode = structure_voxels::OverrideMode(in.read<uint8_t>());
            record.anchorX = in.read<int32_t>();
            record.anchorZ = in.read<int32_t>();
            const auto landmark = in.read<uint8_t>();
            const auto cells = in.read<uint32_t>();
            if (landmark > 1 || cells > MaxVoxels || !keys.insert(key).second)
                throw SerializationError("invalid structure record");
            record.landmark = landmark != 0;
            for (uint32_t c = 0; c < cells; ++c) {
                const std::array<int, 3> pos{
                    in.read<int32_t>(), in.read<int32_t>(), in.read<int32_t>()};
                structure_voxels::OverrideVoxel voxel{in.read<uint8_t>()};
                if (section->version >= 2) {
                    const auto shape =
                        TileShape::fromCanonicalCode(in.read<uint8_t>());
                    if (!shape) throw SerializationError("bad override shape");
                    voxel.shape = *shape;
                }
                if (section->version >= 3)
                    voxel.pieces = piece_codec::read(in, in.read<uint8_t>());
                if (!record.voxels.emplace(pos, std::move(voxel)).second)
                    throw SerializationError("duplicate override voxel");
            }
            validate(kind, record);
            records.emplace(key, std::move(record));
        }
    }
    if (!in.atEnd()) throw SerializationError("trailing VOXL data");
    file.extra.remove("VOXL");
    return file;
}

}  // namespace structures_codec
