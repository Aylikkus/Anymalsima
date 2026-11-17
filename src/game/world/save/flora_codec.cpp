#include "flora_codec.hpp"

namespace flora_codec {

Bytes encode(const FloraLayer& layer, const TreeStamp& trees) {
    if (layer.instances.size() > MaxInstances)
        throw SerializationError("too many plants in a chunk");
    BinaryWriter out;
    out.write(Version);
    out.write(layer.resolvedDay);
    out.write(trees.day);
    out.write(trees.version);
    out.write(uint16_t(layer.instances.size()));
    for (const auto& p : layer.instances) {
        out.write(p.species);
        out.write(p.flags);
        out.write(p.seed);
        out.write(p.x);
        out.write(p.z);
        out.write(p.y);
        out.write(p.rotation);
        out.write(p.birthDay);
        out.write(p.harvestDay);
    }
    return out.take();
}

FloraLayer decode(ByteView data, TreeStamp* trees) {
    BinaryReader in(data);
    const auto version = in.read<uint8_t>();
    if (version < 1 || version > Version)
        throw SerializationError("unsupported flora encoding version");
    FloraLayer layer;
    if (version >= 2) layer.resolvedDay = in.read<int32_t>();
    TreeStamp stamp;
    if (version >= 3) {
        stamp.day = in.read<int32_t>();
        stamp.version = in.read<uint16_t>();
    }
    if (trees) *trees = stamp;
    const auto count = in.read<uint16_t>();
    if (count > MaxInstances) throw SerializationError("too many plants");
    layer.instances.resize(count);
    for (auto& p : layer.instances) {
        p.species = in.read<uint8_t>();
        p.flags = in.read<uint8_t>();
        p.seed = in.read<uint16_t>();
        p.x = in.read<uint8_t>();
        p.z = in.read<uint8_t>();
        p.y = in.read<uint8_t>();
        p.rotation = in.read<uint8_t>();
        p.birthDay = in.read<int32_t>();
        if (version >= 2) p.harvestDay = in.read<int32_t>();
        if (!p.species) throw SerializationError("plant without species");
        if (p.y >= Chunk::SIZE_Y) throw SerializationError("bad plant anchor");
    }
    if (!in.atEnd()) throw SerializationError("trailing flora data");
    return layer;
}

}  // namespace flora_codec
