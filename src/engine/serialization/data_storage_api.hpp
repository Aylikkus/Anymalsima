#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <cassert>

struct IDataStorage {
    virtual ~IDataStorage() = default;
};

template<typename T>
class DataStorage : public IDataStorage {
public:
    std::vector<T> items;
    std::unordered_map<std::string, size_t> indexByName;

    const T& get(const std::string& name) const {
        return items[indexByName.at(name)];
    }

    const T& get(int id) const {
        assert(id > 0 && id <= items.size() && "Invalid ID was passed for data");
        return items[id - 1];
    }

    template<typename Func>
    void forEach(Func&& f) const {
        for (const auto& item : items)
            f(item);
    }

    template<typename Func>
    void filter(Func&& predicate, Func&& action) const {
        for (const auto& item : items)
            if (predicate(item))
                action(item);
    }
};