#include "sky_codec.hpp"

#include <algorithm>

#include "world/consts.hpp"

namespace sky_codec {
namespace {
constexpr uint16_t Version = 2;
constexpr uint32_t MaxColumns = 1u << 24;

bool validY(int y) { return y >= 0 && y < WORLD_HEIGHT_Y; }
}  // namespace

Bytes encode(const sky::Overrides& overrides, const SectionContainer& extra) {
    std::vector<uint64_t> keys;
    for (const auto& [key, columns] : overrides)
        if (!columns.empty()) keys.push_back(key);
    std::sort(keys.begin(), keys.end());  // stable, diff-friendly output
    BinaryWriter out;
    out.write(uint32_t(keys.size()));
    for (const auto key : keys) {
        const auto& columns = overrides.at(key);
        std::vector<int> indices;
        for (const auto& [index, column] : columns) indices.push_back(index);
        std::sort(indices.begin(), indices.end());
        out.write(uint32_t(key >> 32));
        out.write(uint32_t(key));
        out.write(uint16_t(indices.size()));
        for (const int index : indices) {
            const auto& column = columns.at(index);
            out.write(uint8_t(index));
            out.write(column.opaque);
            out.write(column.opaqueShape.code());
            out.write(uint8_t(column.overflow));
            out.write(uint8_t(column.spans.size()));
            for (const auto& span : column.spans) {
                out.write(span.top);
                out.write(span.bottom);
                out.write(span.opacity);
                out.write(span.shape.code());
            }
        }
    }
    SectionContainer container = extra;
    container.put("SKYO", Version, out.take());
    return container.encode();
}

File decode(ByteView bytes) {
    File file;
    file.extra = SectionContainer::decode(bytes);
    if (const auto* section = file.extra.find("SKYO")) {
        if (section->version != 1 && section->version != Version)
            throw SerializationError("unsupported SKYO version");
        BinaryReader in(section->data);
        const auto readShape = [&] {
            if (section->version == 1) return TileShape{};
            const auto shape = TileShape::fromCanonicalCode(in.read<uint8_t>());
            if (!shape) throw SerializationError("bad sky shape");
            return *shape;
        };
        const auto count = in.read<uint32_t>();
        if (count > MaxColumns)
            throw SerializationError("too many sky override columns");
        for (uint32_t i = 0; i < count; ++i) {
            const auto cx = in.read<uint32_t>();
            const auto cz = in.read<uint32_t>();
            const uint64_t key = (uint64_t(cx) << 32) | cz;
            if (file.overrides.contains(key))
                throw SerializationError("duplicate sky override column");
            auto& columns = file.overrides[key];
            const auto n = in.read<uint16_t>();
            if (n == 0 || n > sky::Tile::Size)
                throw SerializationError("bad sky override count");
            for (uint16_t c = 0; c < n; ++c) {
                const int index = in.read<uint8_t>();
                sky::Column column;
                column.opaque = in.read<int16_t>();
                if (column.opaque != sky::None && !validY(column.opaque))
                    throw SerializationError("sky top out of range");
                column.opaqueShape = readShape();
                if (column.opaqueShape.code() &&
                    (column.opaque == sky::None ||
                     voxel_light::verticalLoss(
                         {sky::Opaque, column.opaqueShape}) < sky::Opaque))
                    throw SerializationError("bad opaque sky shape");
                const auto overflow = in.read<uint8_t>();
                if (overflow > 1) throw SerializationError("bad sky flags");
                column.overflow = overflow != 0;
                const auto spans = in.read<uint8_t>();
                if (spans > sky::MaxSpans)
                    throw SerializationError("too many sky spans");
                int below = WORLD_HEIGHT_Y;
                for (uint8_t s = 0; s < spans; ++s) {
                    sky::Span span;
                    span.top = in.read<int16_t>();
                    span.bottom = in.read<int16_t>();
                    span.opacity = in.read<uint8_t>();
                    span.shape = readShape();
                    if (!validY(span.top) || !validY(span.bottom) ||
                        span.bottom > span.top || span.top >= below ||
                        span.opacity == 0 || span.opacity > sky::Opaque ||
                        voxel_light::verticalLoss(span.cell()) >= sky::Opaque)
                        throw SerializationError("bad sky span");
                    below = span.bottom;
                    column.spans.push_back(span);
                }
                if (column.opaque != sky::None && column.opaque >= below)
                    throw SerializationError("sky top above its spans");
                if (!columns.emplace(index, std::move(column)).second)
                    throw SerializationError("duplicate sky override");
            }
        }
        if (!in.atEnd()) throw SerializationError("trailing SKYO data");
    }
    file.extra.remove("SKYO");
    return file;
}

}  // namespace sky_codec
