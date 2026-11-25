#include "sky_column.hpp"

#include <algorithm>
#include <climits>

#include "consts.hpp"
#include "save/chunk_codec.hpp"

namespace sky {

uint8_t Column::direct(int y) const {
    int light = Opaque;
    for (const auto& span : spans) {
        const int loss = voxel_light::verticalLoss(span.cell());
        if (span.bottom <= y) {
            // Tiles of the run above y (none when y is above the run).
            if (span.top > y) light -= (span.top - y) * loss;
            break;
        }
        light -= (span.top - span.bottom + 1) * loss;
        if (light <= 0) return 0;
    }
    if (opaque != None && y < opaque) return 0;
    return uint8_t(std::max(0, light));
}

voxel_light::Cell Column::cellAt(int y) const {
    if (opaque != None && y <= opaque)
        return {Opaque, y == opaque ? opaqueShape : TileShape{}};
    for (const auto& span : spans)
        if (y <= span.top && y >= span.bottom) return span.cell();
    return 0;
}

Tile::Tile() { opaque.fill(None); }

Tile::Tile(const std::array<Column, Size>& columns) {
    for (int i = 0; i < Size; ++i) {
        const auto& c = columns[size_t(i)];
        opaque[size_t(i)] = c.opaque;
        if (c.opaqueShape.code()) {
            if (opaqueShapes.empty()) opaqueShapes.resize(Size);
            opaqueShapes[i] = c.opaqueShape;
        }
        spanBegin[size_t(i)] = uint16_t(spans.size());
        spanCount[size_t(i)] = uint8_t(c.spans.size());
        flags[size_t(i)] = c.overflow ? OverflowFlag : 0;
        spans.insert(spans.end(), c.spans.begin(), c.spans.end());
    }
}

Column Tile::column(int i) const {
    Column c;
    c.opaque = opaque[size_t(i)];
    if (!opaqueShapes.empty()) c.opaqueShape = opaqueShapes[i];
    c.spans.assign(spans.begin() + spanBegin[size_t(i)],
                   spans.begin() + spanBegin[size_t(i)] + spanCount[size_t(i)]);
    c.overflow = overflow(i);
    return c;
}

int Tile::top(int i) const {
    return spanCount[size_t(i)] ? spans[spanBegin[size_t(i)]].top
                                : opaque[size_t(i)];
}

uint8_t Tile::direct(int i, int y) const {
    if (y > top(i)) return Opaque;
    if (!spanCount[size_t(i)]) return y == opaque[size_t(i)] ? Opaque : 0;
    return column(i).direct(y);
}

int Tile::highest() const {
    int best = None;
    for (int i = 0; i < Size; ++i) best = std::max(best, top(i));
    return best;
}

void Tile::set(int i, const Column& column) {
    std::array<Column, Size> all;
    for (int j = 0; j < Size; ++j)
        all[size_t(j)] = j == i ? column : this->column(j);
    *this = Tile(all);
}

namespace {

struct EditTiles {
    // Translucent or opaque tiles of the edit's structure, per column:
    // (y, material opacity and profile), highest first.
    std::array<std::vector<std::pair<int, voxel_light::Cell>>, Tile::Size>
        tiles;
};

EditTiles decodeEdit(const ColumnEdit& edit, const OpacityTable& opacity) {
    EditTiles out;
    Chunk part;
    for (auto it = edit.structure.rbegin(); it != edit.structure.rend(); ++it) {
        chunk_codec::decode(*it->second, part);
        for (int z = 0; z < Chunk::SIZE_Z; ++z)
            for (int x = 0; x < Chunk::SIZE_X; ++x)
                for (int y = Chunk::SIZE_Y - 1; y >= 0; --y) {
                    const uint8_t block = part.tiles[z][y][x].block_id;
                    if (!block || !opacity[block]) continue;
                    out.tiles[size_t(Tile::index(x, z))].emplace_back(
                        int(it->first) * Chunk::SIZE_Y + y,
                        voxel_light::Cell{opacity[block],
                                          part.shapeAt(Chunk::cell(x, y, z))});
                }
    }
    return out;
}
}  // namespace

Tile build(const TileInputs& in) {
    const auto& opacity = *in.opacity;
    EditTiles edits;
    if (in.edit) edits = decodeEdit(*in.edit, opacity);
    std::array<Column, Tile::Size> columns;
    for (int i = 0; i < Tile::Size; ++i) {
        const size_t s = size_t(i);
        // Ground: the generated surface, or the edit's ground override.
        int ground = in.surface[s];
        voxel_light::Cell groundCell = Opaque;
        auto water = in.water[s];
        if (in.edit) {
            ground = in.edit->groundY[s];
            groundCell = {opacity[in.edit->groundBlock[s]],
                          in.edit->groundShape[s]};
            water = in.edit->waterAt(i, water);
        }
        const int waterTop = water.topTile();
        const auto& built = edits.tiles[s];
        const int structure = in.structureTop[s];
        int from = std::max({ground, waterTop, structure});
        if (!built.empty()) from = std::max(from, built.front().first);
        size_t next = 0;
        columns[s] = scanColumn(from, 0, [&](int y) -> voxel_light::Cell {
            if (y == structure || (y < ground && ground >= 0)) return Opaque;
            if (y == ground) return groundCell;
            while (next < built.size() && built[next].first > y) ++next;
            if (next < built.size() && built[next].first == y)
                return built[next].second;
            if (y > ground && y <= waterTop)
                return waterOpacity(y == waterTop ? water.topFill : 255);
            return {};
        });
    }
    return Tile(columns);
}

}  // namespace sky
