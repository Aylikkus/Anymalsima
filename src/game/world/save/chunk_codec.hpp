#pragma once

#include "serialization/binary_stream.hpp"
#include "world/chunk.hpp"

// Compact per-chunk encoding for save files.
//
// v1: u8 version, u16 paletteCount, palette {block_id, liquid, liquidLevel}.
//     paletteCount == 1 → uniform chunk, nothing follows.
//     Otherwise: u8 codec, u32 storedSize, indices (u8 when the palette fits
//     in 256 entries, else u16 LE) in [z][y][x] order, zstd when codec == 1.
// Lighting is derived (SunLightPropagator recomputes it), so it is not stored
// and decoded tiles have zeroed light.
namespace chunk_codec {

inline constexpr uint8_t Version = 1;

Bytes encode(const Chunk& chunk);
// Throws SerializationError on malformed input.
void decode(ByteView data, Chunk& chunk);
// Highest local Y that holds a non-air tile, or -1 for an empty chunk.
int highestSolidY(const Chunk& chunk);

}  // namespace chunk_codec
