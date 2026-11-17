#include "trees_codec.hpp"

#include <algorithm>
#include <cmath>

#include "world/consts.hpp"

namespace trees_codec {
namespace {
constexpr uint16_t SectionVersion = 1;
constexpr uint32_t MaxTrees = 1u << 22;
constexpr uint16_t MaxCuts = 4096;
}  // namespace

Bytes encode(const TreeOverrideMap& trees, const SectionContainer& extra) {
    std::vector<const TreeRecord*> sorted;
    sorted.reserve(trees.size());
    for (const auto& [key, record] : trees) sorted.push_back(&record);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) { return a->key < b->key; });
    BinaryWriter out;
    out.write(uint32_t(sorted.size()));
    for (const auto* r : sorted) {
        if (r->cuts.size() > MaxCuts)
            throw SerializationError("too many cuts on a tree");
        out.write(r->key);
        out.write(r->species);
        out.write(r->seed);
        out.write(int32_t(r->anchor.x));
        out.write(int32_t(r->anchor.y));
        out.write(int32_t(r->anchor.z));
        out.write(r->birthDay);
        out.write(r->headroom);
        out.write(uint16_t(r->cuts.size()));
        for (const auto& cut : r->cuts) {
            out.write(int32_t(cut.segment));
            out.write(cut.t);
            out.write(cut.age);
        }
    }
    SectionContainer container = extra;
    container.put("TREE", SectionVersion, out.take());
    return container.encode();
}

File decode(ByteView bytes) {
    File file;
    file.extra = SectionContainer::decode(bytes);
    if (const auto* section = file.extra.find("TREE")) {
        if (section->version != SectionVersion)
            throw SerializationError("unsupported TREE version");
        BinaryReader in(section->data);
        const auto count = in.read<uint32_t>();
        if (count > MaxTrees) throw SerializationError("too many trees");
        for (uint32_t i = 0; i < count; ++i) {
            TreeRecord r;
            r.key = in.read<uint64_t>();
            r.species = in.read<uint8_t>();
            r.seed = in.read<uint32_t>();
            r.anchor.x = in.read<int32_t>();
            r.anchor.y = in.read<int32_t>();
            r.anchor.z = in.read<int32_t>();
            r.birthDay = in.read<int32_t>();
            r.headroom = in.read<float>();
            const auto cuts = in.read<uint16_t>();
            if (cuts > MaxCuts) throw SerializationError("too many cuts");
            if (r.planted() && r.species == 0)
                throw SerializationError("planted tree without species");
            if (r.anchor.x < 0 || r.anchor.z < 0 || r.anchor.y < -4096 ||
                r.anchor.y > 4096 || !std::isfinite(r.headroom) ||
                r.headroom < 0 || r.headroom > 4096)
                throw SerializationError("tree record out of range");
            r.cuts.resize(cuts);
            for (auto& cut : r.cuts) {
                cut.segment = in.read<int32_t>();
                cut.t = in.read<float>();
                cut.age = in.read<float>();
                if (cut.segment < 0 || !std::isfinite(cut.t) || cut.t < 0 ||
                    cut.t > 1 || !std::isfinite(cut.age))
                    throw SerializationError("bad tree cut");
            }
            if (!file.trees.emplace(r.key, std::move(r)).second)
                throw SerializationError("duplicate tree");
        }
        if (!in.atEnd()) throw SerializationError("trailing TREE data");
    }
    file.extra.remove("TREE");
    return file;
}

}  // namespace trees_codec
