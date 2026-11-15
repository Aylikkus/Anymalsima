#pragma once

#include <optional>
#include <string>

#include "binary_stream.hpp"

// Versioned, tagged binary container used by save files.
//
// Layout (little-endian):
//   "ANYM" u16 containerVersion u32 sectionCount
//   per section: fourcc tag, u16 version, u8 codec, u32 rawSize,
//                u32 storedSize, storedSize payload bytes
//
// Readers look sections up by tag and keep unknown ones untouched, so newer
// data survives being re-saved by an older build.
enum class SectionCodec : uint8_t { Raw = 0, Zstd = 1 };

struct Section {
    std::string tag;  // exactly four characters
    uint16_t version = 1;
    Bytes data;  // always uncompressed in memory
    // Payloads that are already compressed (e.g. per-chunk blobs) opt out.
    bool compress = true;
};

class SectionContainer {
   public:
    static constexpr uint16_t Version = 1;

    std::vector<Section> sections;

    const Section* find(std::string_view tag) const;
    // Inserts or replaces the section with this tag.
    void put(std::string tag, uint16_t version, Bytes data,
             bool compress = true);
    bool remove(std::string_view tag);

    // Sections larger than compressThreshold are zstd-compressed.
    Bytes encode(size_t compressThreshold = 256) const;
    static SectionContainer decode(ByteView bytes);
};
