#include "world.hpp"

void World::update()
{
}

int64_t World::makeKey(int x, int y)
{
    x = (x % width + width) % width;
    return (static_cast<int64_t>(y) << 32) | x;
}
