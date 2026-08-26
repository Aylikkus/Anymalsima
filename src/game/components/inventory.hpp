#pragma once
#include <vector>
#include "ecs/entity.hpp"

struct Inventory {
    std::vector<Entity> items;
};