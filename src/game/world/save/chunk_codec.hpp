#pragma once

#include "serialization/binary_stream.hpp"
#include "world/chunk.hpp"

// Compact per-chunk encoding for save files.
//
// v6: paletteCount bit 14 marks a shape section following the palette and
//     any indices: u32 byte length, u16 count, then sorted
//     {u16 cell, u8 canonical noncube code} entries. Bit 13 marks a root
//     tail, which adds a u8 canonical substrate shape code to each root.
// v5: the high bit of paletteCount marks slabs eligible for reservoir
//     projection. Derived water is omitted from the palette and rebuilt
//     from the reservoir ledger on load.
// v4: v3 layout with the shallow root shape. Flag bit 1 retains a pending
//     shape migration if a decoded chunk is encoded before reconciliation.
// v3: v2 palette and indices, with an optional tail for root terrain.
//     Tail: u8 flags (bit 0: legacy terrain), u16 rootCount,
//     {u16 cell, u64 owner, u8 block, u8 cover, u8 coverState} per root,
//     then u16 editCount and u16 per edited cell.
// v2: u8 version, u16 paletteCount, palette {block_id, liquid, liquidLevel,
//     cover, coverState}.
//     paletteCount == 1 → uniform palette, no index stream.
//     Otherwise: u8 codec, u32 storedSize, indices (u8 when the palette fits
//     in 256 entries, else u16 LE) in [z][y][x] order, zstd when codec == 1.
// v1: the same with a three-byte palette {block_id, liquid, liquidLevel};
//     decoded tiles have no ground cover.
// Lighting is derived (SunLightPropagator recomputes it), so it is not stored
// and decoded tiles have zeroed light.
namespace chunk_codec {

inline constexpr uint8_t Version = 6;
inline constexpr uint8_t MinVersion = 1;

Bytes encode(const Chunk& chunk);
// Throws SerializationError on malformed input.
void decode(ByteView data, Chunk& chunk);
// Highest local Y that holds a non-air tile, or -1 for an empty chunk.
int highestSolidY(const Chunk& chunk);

}  // namespace chunk_codec
