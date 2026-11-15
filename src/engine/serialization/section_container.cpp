#include "section_container.hpp"

#include <algorithm>

#include "compression.hpp"

namespace {
constexpr std::string_view Magic = "ANYM";
}

const Section* SectionContainer::find(std::string_view tag) const {
    for (const auto& section : sections)
        if (section.tag == tag) return &section;
    return nullptr;
}

void SectionContainer::put(std::string tag, uint16_t version, Bytes data,
                           bool compress) {
    if (tag.size() != 4) throw SerializationError("tag must be 4 chars");
    for (auto& section : sections)
        if (section.tag == tag) {
            section.version = version;
            section.data = std::move(data);
            section.compress = compress;
            return;
        }
    sections.push_back({std::move(tag), version, std::move(data), compress});
}

bool SectionContainer::remove(std::string_view tag) {
    return std::erase_if(sections, [&](const Section& section) {
               return section.tag == tag;
           }) > 0;
}

Bytes SectionContainer::encode(size_t compressThreshold) const {
    BinaryWriter out;
    out.writeTag(Magic);
    out.write(Version);
    out.write(uint32_t(sections.size()));
    for (const auto& section : sections) {
        if (section.data.size() > UINT32_MAX)
            throw SerializationError("section exceeds 4 GiB");
        const bool zstd =
            section.compress && section.data.size() > compressThreshold;
        Bytes compressed;
        if (zstd) compressed = compression::compress(section.data);
        const ByteView stored = zstd ? ByteView(compressed) : section.data;
        out.writeTag(section.tag);
        out.write(section.version);
        out.write(zstd ? SectionCodec::Zstd : SectionCodec::Raw);
        out.write(uint32_t(section.data.size()));
        out.writeBlock(stored);
    }
    return out.take();
}

SectionContainer SectionContainer::decode(ByteView bytes) {
    BinaryReader in(bytes);
    if (in.readTag() != Magic) throw SerializationError("bad save magic");
    const auto version = in.read<uint16_t>();
    if (version > Version)
        throw SerializationError("save container is from a newer version");
    const auto count = in.read<uint32_t>();
    SectionContainer result;
    for (uint32_t i = 0; i < count; ++i) {
        Section section;
        section.tag = in.readTag();
        section.version = in.read<uint16_t>();
        const auto codec = in.read<SectionCodec>();
        const auto rawSize = in.read<uint32_t>();
        const auto stored = in.readBlock();
        switch (codec) {
            case SectionCodec::Raw:
                section.compress = false;
                if (stored.size() != rawSize)
                    throw SerializationError("raw section size mismatch");
                section.data.assign(stored.begin(), stored.end());
                break;
            case SectionCodec::Zstd:
                section.data = compression::decompress(stored, rawSize);
                break;
            default:
                throw SerializationError("unknown section codec");
        }
        result.sections.push_back(std::move(section));
    }
    return result;
}
