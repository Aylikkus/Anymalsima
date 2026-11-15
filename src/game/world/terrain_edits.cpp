#include "terrain_edits.hpp"

#include <algorithm>

#include "save/chunk_codec.hpp"

void ColumnEdit::finish() {
    std::sort(structure.begin(), structure.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    structureTop = -1;
    for (const auto& [slabY, blob] : structure) {
        Chunk chunk;
        chunk_codec::decode(*blob, chunk);
        const int top = chunk_codec::highestSolidY(chunk);
        if (top >= 0)
            structureTop =
                std::max(structureTop, int(slabY) * Chunk::SIZE_Y + top);
    }
}

const ChunkBlob* ColumnEdit::slab(uint16_t slabY) const {
    const auto it = std::lower_bound(
        structure.begin(), structure.end(), slabY,
        [](const auto& entry, uint16_t y) { return entry.first < y; });
    return it != structure.end() && it->first == slabY ? &it->second : nullptr;
}

std::shared_ptr<const ColumnEdit> TerrainEdits::find(uint64_t key) const {
    const auto it = map->find(key);
    return it == map->end() ? nullptr : it->second;
}

void TerrainEdits::set(uint64_t key, std::shared_ptr<const ColumnEdit> edit) {
    auto next = std::make_shared<EditMap>(*map);
    (*next)[key] = std::move(edit);
    map = std::move(next);
    ++rev;
    dirty = true;
    changed.emit(key);
}

void TerrainEdits::erase(uint64_t key) {
    if (!map->contains(key)) return;
    auto next = std::make_shared<EditMap>(*map);
    next->erase(key);
    map = std::move(next);
    ++rev;
    dirty = true;
    changed.emit(key);
}
