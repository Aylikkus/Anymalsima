#pragma once

#include <functional>
#include <algorithm>

template<typename... Args>
class Signal
{
public:
    using Slot = std::function<void(Args...)>;

    struct Connection {
        size_t id = 0;
    };

    Connection connect(Slot slot)
    {
        size_t id = nextId++;
        slots.emplace_back(id, std::move(slot));
        return { id };
    }

    void disconnect(Connection c)
    {
        slots.erase(
            std::remove_if(slots.begin(), slots.end(),
                [&](const auto& s) { return s.first == c.id; }),
            slots.end()
        );
    }

    void emit(Args... args)
    {
        // copy to allow modification during iteration
        auto copy = slots;

        for (auto& [id, slot] : copy)
        {
            if (slot)
                slot(args...);
        }
    }

    bool empty() const { return slots.empty(); }

private:
    std::vector<std::pair<size_t, Slot>> slots{};
    size_t nextId = 1;
};