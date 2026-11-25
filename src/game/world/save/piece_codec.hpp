#pragma once

#include "serialization/binary_stream.hpp"
#include "world/chunk.hpp"

namespace piece_codec {

inline constexpr size_t RecordBytes = 32;

inline void write(BinaryWriter& out, const CellPiece& piece) {
    const auto& g = piece.geometry;
    out.write(g.id);
    out.write(g.blockId);
    out.write(g.shape.code());
    out.write(g.offset.x);
    out.write(g.offset.y);
    out.write(g.offset.z);
    out.write(uint8_t(g.fill));
    out.write(g.ceiling);
    out.write(piece.origin.kind);
    out.write(piece.origin.owner);
}

inline std::vector<CellPiece> read(BinaryReader& in, size_t count) {
    if (count > Chunk::MaxPiecesPerCell || count > in.remaining() / RecordBytes)
        throw SerializationError("bad solid piece count");
    std::vector<CellPiece> pieces;
    pieces.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto id = in.read<tile_geometry::PieceId>();
        const auto block = in.read<uint8_t>();
        const auto shape = TileShape::fromCanonicalCode(in.read<uint8_t>());
        const Vec3 offset{in.read<float>(), in.read<float>(), in.read<float>()};
        const auto fill = in.read<uint8_t>();
        const auto ceiling = in.read<float>();
        const auto origin = in.read<PieceOrigin>();
        const auto owner = in.read<uint64_t>();
        if (!shape || fill > uint8_t(tile_geometry::PieceFill::AboveProfile) ||
            origin > PieceOrigin::Environment ||
            (!pieces.empty() && id <= pieces.back().geometry.id))
            throw SerializationError("bad solid piece record");
        pieces.push_back({{id, block, *shape, offset,
                           tile_geometry::PieceFill(fill), ceiling},
                          {origin, owner}});
    }
    return pieces;
}

}  // namespace piece_codec
