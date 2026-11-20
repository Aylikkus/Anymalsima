#include "sky_column.hpp"

#include <algorithm>
#include <climits>

#include "consts.hpp"
#include "save/chunk_codec.hpp"

namespace sky {

uint8_t Column::direct(int y) const {
    int light = Opaque;
    for (const auto& span : spans) {
        if (span.bottom <= y) {
            // Tiles of the run above y (none when y is above the run).
            if (span.top > y) light -= (span.top - y) * span.opacity;
            break;
        }
        light -= (span.top - span.bottom + 1) * span.opacity;
        if (light <= 0) return 0;
    }
    if (opaque != None && y < opaque) return 0;
    return uint8_t(std::max(0, light));
}

uint8_t Column::opacityAt(int y) const {
    if (opaque != None && y <= opaque) return Opaque;
    for (const auto& span : spans)
        if (y <= span.top && y >= span.bottom) return span.opacity;
    return 0;
}

Tile::Tile() { opaque.fill(None); }

Tile::Tile(const std::array<Column, Size>& columns) {
    for (int i = 0; i < Size; ++i) {
        const auto& c = columns[size_t(i)];
        opaque[size_t(i)] = c.opaque;
        spanBegin[size_t(i)] = uint16_t(spans.size());
        spanCount[size_t(i)] = uint8_t(c.spans.size());
        flags[size_t(i)] = c.overflow ? OverflowFlag : 0;
        spans.insert(spans.end(), c.spans.begin(), c.spans.end());
    }
}

Column Tile::column(int i) const {
    Column c;
    c.opaque = opaque[size_t(i)];
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
constexpr uint8_t WaterOpacity = 8;  // tileOpacity: liquids

struct EditTiles {
    // Translucent or opaque tiles of the edit's structure, per column:
    // (y, opacity), highest first.
    std::array<std::vector<std::pair<int, uint8_t>>, Tile::Size> tiles;
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
                        int(it->first) * Chunk::SIZE_Y + y, opacity[block]);
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
        uint8_t groundOpacity = Opaque;
        int waterTop = in.water[s].topTile();
        if (in.edit) {
            ground = in.edit->groundY[s];
            groundOpacity =
                ground >= 0
                    ? std::max<uint8_t>(1, opacity[in.edit->groundBlock[s]])
                    : 0;
            waterTop = in.edit->waterAt(i, in.water[s]).topTile();
        }
        const auto& built = edits.tiles[s];
        const int structure = in.structureTop[s];
        int from = std::max({ground, waterTop, structure});
        if (!built.empty()) from = std::max(from, built.front().first);
        size_t next = 0;
        // The edit's ground tile, when translucent, rests on unknown
        // tiles: the one below counts as opaque (darker than or equal to
        // the truth).
        const int floor =
            ground >= 0 && groundOpacity < Opaque ? ground - 1 : INT_MIN;
        columns[s] = scanColumn(from, 0, [&](int y) -> int {
            int o = 0;
            if (y == structure) o = Opaque;
            while (next < built.size() && built[next].first > y) ++next;
            if (next < built.size() && built[next].first == y)
                o = std::max<int>(o, built[next].second);
            if (y == ground) o = std::max<int>(o, groundOpacity);
            if (y <= floor || (y < ground && ground >= 0)) o = Opaque;
            if (y > ground && y <= waterTop) o = std::max<int>(o, WaterOpacity);
            return o;
        });
    }
    return Tile(columns);
}

}  // namespace sky
