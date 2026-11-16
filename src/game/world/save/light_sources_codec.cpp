#include "light_sources_codec.hpp"

#include <algorithm>
#include <tuple>
#include <unordered_set>

#include "world/consts.hpp"

namespace light_sources_codec {
namespace {
constexpr uint16_t LightSectionVersion = 2;
constexpr uint32_t MaxLights = 1u << 22;
}  // namespace

Bytes encode(std::vector<Record> lights, const SectionContainer& extra) {
    // Stable, diff-friendly output.
    std::sort(lights.begin(), lights.end(), [](const auto& a, const auto& b) {
        return std::tie(a.tile.x, a.tile.z, a.tile.y) <
               std::tie(b.tile.x, b.tile.z, b.tile.y);
    });
    BinaryWriter out;
    out.write(uint32_t(lights.size()));
    for (const auto& light : lights) {
        out.write(uint32_t(light.tile.x));
        out.write(uint16_t(light.tile.y));
        out.write(uint32_t(light.tile.z));
        out.write(light.block);
        out.write(light.openFaces);
    }
    SectionContainer container = extra;
    container.put("LITE", LightSectionVersion, out.take());
    return container.encode();
}

File decode(ByteView bytes) {
    File file;
    file.extra = SectionContainer::decode(bytes);
    if (const auto* section = file.extra.find("LITE")) {
        const auto version = section->version;
        if (version != 1 && version != LightSectionVersion)
            throw SerializationError("unsupported LITE version");
        BinaryReader in(section->data);
        const auto count = in.read<uint32_t>();
        if (count > MaxLights) throw SerializationError("too many lights");
        std::unordered_set<TilePos> seen;
        file.lights.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Record record;
            const auto x = in.read<uint32_t>();
            const auto y = in.read<uint16_t>();
            const auto z = in.read<uint32_t>();
            record.block = in.read<uint8_t>();
            if (version >= 2) {
                record.openFaces = in.read<uint8_t>();
                if (record.openFaces > 0x3f)
                    throw SerializationError("bad light faces");
            }
            if (x > uint32_t(INT32_MAX) || z > uint32_t(INT32_MAX) ||
                y >= WORLD_HEIGHT_Y)
                throw SerializationError("light position out of range");
            if (record.block == 0)
                throw SerializationError("light without a block");
            record.tile = {int(x), int(y), int(z)};
            if (!seen.insert(record.tile).second)
                throw SerializationError("duplicate light");
            file.lights.push_back(record);
        }
        if (!in.atEnd()) throw SerializationError("trailing LITE data");
    }
    file.extra.remove("LITE");
    return file;
}

}  // namespace light_sources_codec
