#pragma once

#include <array>
#include <climits>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/geometry.hpp"
#include "serialization/section_container.hpp"
#include "world/chunk.hpp"
#include "world/chunk_coord.hpp"
#include "world/consts.hpp"
#include "world/flora.hpp"

// Immutable, encoded chunk payload (see chunk_codec.hpp).
using ChunkBlob = std::shared_ptr<const Bytes>;

// Persisted state of one region. Sections:
//   "CHNK" v1: u32 count, then {u16 localIndex, u32 size, blob} per chunk.
//   "TOPS" v1: one i32 per chunk column (CHUNKS_PER_REGION²), the highest
//              solid world Y seen in saved chunks, INT_MIN when unknown.
//   "FLOR" v1: u32 count, then {u16 localIndex, u32 size, blob} per chunk:
//              its plants (flora_codec.hpp). A saved chunk without an entry
//              (older saves) regrows its generated plants.
// Unknown sections are kept in `extra` and written back unchanged.
struct RegionData {
    static constexpr int Columns = CHUNKS_PER_REGION * CHUNKS_PER_REGION;
    static constexpr int ChunkCount = Columns * Y_CHUNKS;

    std::unordered_map<uint16_t, ChunkBlob> chunks;
    std::unordered_map<uint16_t, ChunkBlob> flora;
    std::array<int32_t, Columns> tops;
    SectionContainer extra;

    RegionData() { tops.fill(INT_MIN); }

    bool empty() const { return chunks.empty(); }

    // Local index = slabY * 64 + cz * 8 + cx.
    static uint16_t localIndex(const ChunkCoord& coord);
    static ChunkCoord worldCoord(Vec2u region, uint16_t index);
    static int column(uint16_t index) { return index % Columns; }
};

// Chunks captured from ChunkManager that changed since they were last saved.
struct RegionSnapshot {
    std::vector<std::pair<uint16_t, std::shared_ptr<const Chunk>>> chunks;
    // Plants of the same chunks (an empty layer is saved too).
    std::vector<std::pair<uint16_t, FloraLayerPtr>> flora;
    bool empty() const { return chunks.empty(); }
};

namespace region_file {

std::filesystem::path path(const std::filesystem::path& worldDir, Vec2u rc);
Bytes encode(const RegionData& data);
RegionData decode(ByteView bytes);
// Encodes snapshot chunks (on a worker) and merges them over base.
RegionData merge(const RegionData& base, const RegionSnapshot& snapshot);

}  // namespace region_file
