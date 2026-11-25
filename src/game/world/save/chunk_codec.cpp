#include "chunk_codec.hpp"

#include <unordered_map>

#include "serialization/compression.hpp"

namespace chunk_codec {
namespace {

constexpr size_t Volume = Chunk::SIZE_X * Chunk::SIZE_Y * Chunk::SIZE_Z;

// Saved tile fields, one byte each in palette order.
uint64_t key(const Tile& tile) {
    if (tile.waterOrigin != WaterOrigin::Independent) return tile.block_id;
    return uint64_t(tile.block_id) | uint64_t(uint8_t(tile.liquid)) << 8 |
           uint64_t(tile.liquidLevel) << 16 | uint64_t(tile.cover) << 24 |
           uint64_t(tile.coverState) << 32;
}
constexpr int KeyBytes = 5;
constexpr uint16_t ProjectsWater = uint16_t(1) << 15;
constexpr uint16_t HasShapes = uint16_t(1) << 14;
constexpr uint16_t HasRootTerrain = uint16_t(1) << 13;

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
    auto count = uint16_t(palette.size());
    if (chunk.projectsWater) count |= ProjectsWater;
    if (chunk.hasShapes()) count |= HasShapes;
    const bool hasRootTerrain =
        chunk.legacyRootTerrain || chunk.legacyRootShape || chunk.rootTerrain;
    if (hasRootTerrain) count |= HasRootTerrain;
    out.write(count);
    for (const auto k : palette)
        for (int b = 0; b < KeyBytes; ++b)
            out.write(uint8_t((k >> 8 * b) & 0xFF));
    if (palette.size() > 1) {
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
    }
    if (chunk.hasShapes()) {
        BinaryWriter shapes;
        shapes.write(uint16_t(chunk.shapeCount()));
        chunk.forEachShape([&](uint16_t cell, TileShape shape) {
            shapes.write(cell);
            shapes.write(shape.code());
        });
        out.writeBlock(shapes.bytes());
    }
    if (hasRootTerrain) {
        out.write(uint8_t((chunk.legacyRootTerrain ? 1 : 0) |
                          (chunk.legacyRootShape ? 2 : 0)));
        static const RootTerrain empty;
        const auto& data = chunk.rootTerrain ? *chunk.rootTerrain : empty;
        out.write(uint16_t(data.roots.size()));
        for (const auto& [cell, root] : data.roots) {
            out.write(cell);
            out.write(root.owner);
            out.write(root.block);
            out.write(root.cover);
            out.write(root.coverState);
            out.write(root.shape.code());
        }
        out.write(uint16_t(data.edited.size()));
        for (const auto cell : data.edited) out.write(cell);
    }
    return out.take();
}

void decode(ByteView data, Chunk& output) {
    Chunk chunk;
    BinaryReader in(data);
    const auto version = in.read<uint8_t>();
    if (version < MinVersion || version > Version)
        throw SerializationError("unsupported chunk encoding version");
    auto count = in.read<uint16_t>();
    chunk.projectsWater = version >= 5 && (count & ProjectsWater);
    if (chunk.projectsWater) count &= ~ProjectsWater;
    const bool hasShapes = version >= 6 && (count & HasShapes);
    const bool hasRootTerrain = version >= 6 && (count & HasRootTerrain);
    if (version >= 6) count &= ~(HasShapes | HasRootTerrain);
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
    } else {
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
    if (hasShapes) {
        BinaryReader entries(in.readBlock(2 + 3 * Volume));
        const auto count = entries.read<uint16_t>();
        if (!count || count > Volume || entries.remaining() != 3u * count)
            throw SerializationError("bad chunk shape section length");
        int previous = -1;
        for (uint16_t i = 0; i < count; ++i) {
            const auto cell = entries.read<uint16_t>();
            const auto shape =
                TileShape::fromCanonicalCode(entries.read<uint8_t>());
            if (cell >= Volume || cell <= previous || !shape ||
                !shape->code() || !tiles[cell].isSolid() ||
                tiles[cell].isLiquid())
                throw SerializationError("bad chunk shape entry");
            chunk.setCell(cell, tiles[cell], *shape);
            previous = cell;
        }
    }
    chunk.legacyRootTerrain = version < 3;
    chunk.legacyRootShape = version == 3;
    if (hasRootTerrain || (version >= 3 && version < 6 && !in.atEnd())) {
        const auto flags = in.read<uint8_t>();
        if (flags > (version == 3 ? 1 : 3))
            throw SerializationError("bad chunk root flags");
        chunk.legacyRootTerrain = flags & 1;
        chunk.legacyRootShape |= (flags & 2) != 0;
        auto data = std::make_shared<RootTerrain>();
        const auto roots = in.read<uint16_t>();
        if (roots > Volume || (chunk.legacyRootTerrain && roots))
            throw SerializationError("bad root count");
        for (uint16_t i = 0; i < roots; ++i) {
            const auto cell = in.read<uint16_t>();
            RootVoxel root{in.read<uint64_t>(), in.read<uint8_t>(),
                           in.read<uint8_t>(), in.read<uint8_t>()};
            if (version >= 6) {
                const auto shape =
                    TileShape::fromCanonicalCode(in.read<uint8_t>());
                if (!shape) throw SerializationError("bad root shape");
                root.shape = *shape;
            }
            if (cell >= Volume || !tiles[cell].isSolid() || !root.owner ||
                !root.block || (!root.cover && root.coverState) ||
                !data->roots.emplace(cell, root).second)
                throw SerializationError("bad root provenance");
        }
        const auto edits = in.read<uint16_t>();
        if (size_t(roots) + edits > Volume ||
            (chunk.legacyRootTerrain && edits))
            throw SerializationError("bad root edit count");
        for (uint16_t i = 0; i < edits; ++i) {
            const auto cell = in.read<uint16_t>();
            if (cell >= Volume || data->roots.contains(cell) ||
                !data->edited.insert(cell).second)
                throw SerializationError("bad root edit cell");
        }
        if (roots || edits) chunk.rootTerrain = std::move(data);
    }
    if (!in.atEnd()) throw SerializationError("trailing chunk data");
    output = std::move(chunk);
}

int highestSolidY(const Chunk& chunk) {
    for (int y = Chunk::SIZE_Y - 1; y >= 0; --y)
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x)
                if (chunk.tiles[z][y][x].block_id != 0) return y;
    return -1;
}

}  // namespace chunk_codec
