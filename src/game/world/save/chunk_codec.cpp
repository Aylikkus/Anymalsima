#include "chunk_codec.hpp"

#include <unordered_map>

#include "serialization/compression.hpp"

namespace chunk_codec {
namespace {

constexpr size_t Volume = Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z;

// Saved tile fields, one byte each in palette order.
uint64_t key(const Tile& tile) {
    return uint64_t(tile.block_id) | uint64_t(uint8_t(tile.liquid)) << 8 |
           uint64_t(tile.liquidLevel) << 16 | uint64_t(tile.cover) << 24 |
           uint64_t(tile.coverState) << 32;
}
constexpr int KeyBytes = 5;

const Tile* flat(const Chunk& chunk) { return &chunk.tiles[0][0][0]; }
Tile* flat(Chunk& chunk) { return &chunk.tiles[0][0][0]; }

}  // namespace

Bytes encode(const Chunk& chunk) {
    const Tile* tiles = flat(chunk);
    std::unordered_map<uint64_t, uint16_t> lookup;
    std::vector<uint64_t> palette;
    std::vector<uint16_t> indices(Volume);
    // Neighbouring tiles usually match, so skip the hash lookup for runs.
    uint64_t lastKey = 0;
    uint16_t lastIndex = 0;
    for (size_t i = 0; i < Volume; ++i) {
        const auto k = key(tiles[i]);
        if (i == 0 || k != lastKey) {
            auto [it, inserted] =
                lookup.try_emplace(k, uint16_t(palette.size()));
            if (inserted) palette.push_back(k);
            lastKey = k;
            lastIndex = it->second;
        }
        indices[i] = lastIndex;
    }

    BinaryWriter out;
    out.write(Version);
    out.write(uint16_t(palette.size()));
    for (const auto k : palette)
        for (int b = 0; b < KeyBytes; ++b)
            out.write(uint8_t((k >> 8 * b) & 0xFF));
    if (palette.size() == 1) return out.take();

    BinaryWriter raw;
    const bool wide = palette.size() > 256;
    for (const auto index : indices) {
        if (wide)
            raw.write(index);
        else
            raw.write(uint8_t(index));
    }
    const auto packed = compression::compress(raw.bytes());
    out.write(uint8_t(1));
    out.writeBlock(packed);
    return out.take();
}

void decode(ByteView data, Chunk& chunk) {
    BinaryReader in(data);
    const auto version = in.read<uint8_t>();
    if (version < MinVersion || version > Version)
        throw SerializationError("unsupported chunk encoding version");
    const auto count = in.read<uint16_t>();
    if (count == 0 || count > Volume)
        throw SerializationError("bad chunk palette size");
    std::vector<Tile> palette(count);
    for (auto& tile : palette) {
        tile.block_id = in.read<uint8_t>();
        const auto liquid = in.read<uint8_t>();
        if (liquid > uint8_t(LiquidType::Lava))
            throw SerializationError("bad liquid type");
        tile.liquid = LiquidType(liquid);
        tile.liquidLevel = in.read<uint8_t>();
        if (version >= 2) {
            tile.cover = in.read<uint8_t>();
            tile.coverState = in.read<uint8_t>();
            if (!tile.cover && tile.coverState)
                throw SerializationError("cover state without cover");
        }
    }
    Tile* tiles = flat(chunk);
    if (count == 1) {
        std::fill(tiles, tiles + Volume, palette[0]);
        return;
    }
    const auto codec = in.read<uint8_t>();
    const bool wide = count > 256;
    const size_t rawSize = Volume * (wide ? 2 : 1);
    const auto stored = in.readBlock();
    Bytes raw;
    if (codec == 0) {
        raw.assign(stored.begin(), stored.end());
    } else if (codec == 1) {
        raw = compression::decompress(stored, rawSize);
    } else {
        throw SerializationError("unknown chunk codec");
    }
    if (raw.size() != rawSize) throw SerializationError("bad chunk size");
    BinaryReader indices(raw);
    for (size_t i = 0; i < Volume; ++i) {
        const uint16_t index =
            wide ? indices.read<uint16_t>() : indices.read<uint8_t>();
        if (index >= count) throw SerializationError("bad palette index");
        tiles[i] = palette[index];
    }
}

int highestSolidY(const Chunk& chunk) {
    for (int y = Chunk::SIZE_Y - 1; y >= 0; --y)
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x)
                if (chunk.tiles[z][y][x].block_id != 0) return y;
    return -1;
}

}  // namespace chunk_codec
