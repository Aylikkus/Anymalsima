#pragma once

#include <climits>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>

#include "tile_pos.hpp"

// Workers keep immutable snapshots while the world thread moves water. A
// copied ledger shares untouched X rows. Within a row (Z, Y) ordering keeps
// each vertical column contiguous for surface and rectangular LOD queries.
// Values are absolute volume units: 255 units is one cubic metre. A cell
// whose partial solid leaves less free space therefore tops out below 255.
class LocalWaterCells {
    using Key = std::pair<int, int>;
    using Row = std::map<Key, uint8_t>;
    std::map<int, std::shared_ptr<Row>> rows;
    size_t count = 0;

   public:
    bool empty() const { return count == 0; }
    size_t size() const { return count; }
    uint8_t fill(TilePos p) const {
        const auto row = rows.find(p.x);
        if (row == rows.end()) return 0;
        const auto cell = row->second->find({p.z, p.y});
        return cell == row->second->end() ? 0 : cell->second;
    }
    bool contains(TilePos p) const { return fill(p) != 0; }
    uint8_t at(TilePos p) const {
        const auto amount = fill(p);
        if (!amount) throw std::out_of_range("Missing local water cell");
        return amount;
    }
    void set(TilePos p, uint8_t amount) {
        auto row = rows.find(p.x);
        if (row == rows.end()) {
            if (!amount) return;
            row = rows.emplace(p.x, std::make_shared<Row>()).first;
        } else if (!row->second.unique()) {
            row->second = std::make_shared<Row>(*row->second);
        }
        const Key key{p.z, p.y};
        if (amount) {
            count += row->second->insert_or_assign(key, amount).second;
        } else {
            count -= row->second->erase(key);
            if (row->second->empty()) rows.erase(row);
        }
    }
    int top(int x, int z) const {
        const auto row = rows.find(x);
        if (row == rows.end()) return -1;
        auto end = row->second->upper_bound({z, INT_MAX});
        if (end == row->second->begin()) return -1;
        const auto& [column, y] = std::prev(end)->first;
        return column == z ? y + 1 : -1;
    }
    bool intersects(int x0, int z0, int x1, int z1) const {
        for (auto row = rows.lower_bound(x0);
             row != rows.end() && row->first < x1; ++row) {
            const auto cell = row->second->lower_bound({z0, INT_MIN});
            if (cell != row->second->end() && cell->first.first < z1)
                return true;
        }
        return false;
    }
    template <typename F>
    void visitRow(int x, int z0, int z1, F visit) const {
        const auto row = rows.find(x);
        if (row == rows.end()) return;
        for (auto it = row->second->lower_bound({z0, INT_MIN});
             it != row->second->end() && it->first.first <= z1; ++it)
            visit(TilePos{x, it->first.second, it->first.first}, it->second);
    }
    template <typename F>
    void forEach(F visit) const {
        for (const auto& [x, row] : rows)
            for (const auto& [pos, amount] : *row)
                visit(TilePos{x, pos.second, pos.first}, amount);
    }
};
