#include <cstdint>

#include "items/block.hpp"
#include "test.hpp"

TEST(block_default_opacity) {
    const auto block =
        Json{{"name", "stone"}, {"material", "stone"}}.get<Block>();
    EQUAL(int(block.properties.opacity), 15, "default opaque block");
}

TEST(block_explicit_opacity) {
    const auto block = Json{
        {"name", "glass"},
        {"material", "glass"},
        {"properties",
         {{"opacity", 3}}}}.get<Block>();
    EQUAL(int(block.properties.opacity), 3, "configured glass opacity");
}
