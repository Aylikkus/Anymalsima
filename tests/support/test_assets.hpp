#pragma once

#include "render/ui_context.hpp"
#include "world/chunk.hpp"
#include "world/region.hpp"

namespace test {
class Assets final : public IAssets {
   public:
    Assets() {
        auto blocks = std::make_unique<DataStorage<Block>>();
        for (int id = 1; id <= 3; ++id) {
            Block block;
            block.id = id;
            block.name = "block" + std::to_string(id);
            block.material = block.name;
            block.properties.opacity = id == 1 ? 15 : (id == 2 ? 3 : 0);
            blocks->indexByName[block.name] = blocks->items.size();
            blocks->items.push_back(block);
        }
        // Light sources: 4 white lamp, 5 red lamp, 6 far beacon, 7 spotlight
        // pointing down. All opaque, like a lamp block.
        auto lamp = [&](int id, const std::string& name, LightEmission light) {
            Block block;
            block.id = id;
            block.name = name;
            block.material = name;
            block.properties.opacity = 15;
            block.light = light;
            blocks->indexByName[block.name] = blocks->items.size();
            blocks->items.push_back(block);
        };
        lamp(4, "lamp", LightEmission{});
        LightEmission red;
        red.color = {1, 0, 0};
        lamp(5, "red_lamp", red);
        LightEmission beacon;
        beacon.radius = 64;
        lamp(6, "beacon", beacon);
        LightEmission spot;
        spot.cone = LightCone{{0, -1, 0}, 20, 30};
        lamp(7, "spot", spot);
        dataStorages[Block::TypeName] = std::move(blocks);
        auto rocks = std::make_unique<DataStorage<Rock>>();
        Rock rock{};
        rock.id = 1;
        rock.name = "test_rock";
        rock.block_name = "block1";
        rock.category = RockType::IGNEOUS_INTRUSIVE;
        rock.placement.allow_underwater = true;
        rock.body_size_m = {128, 128};
        rocks->indexByName[rock.name] = 0;
        rocks->items.push_back(rock);
        dataStorages[Rock::TypeName] = std::move(rocks);
    }
    template <typename T>
    void setData(std::vector<T> values) {
        auto storage = std::make_unique<DataStorage<T>>();
        for (const auto& value : values)
            storage->indexByName[value.name] = storage->indexByName.size();
        storage->items = std::move(values);
        dataStorages[T::TypeName] = std::move(storage);
    }
    Vec2u getTextureSize(const std::string&) override { return {16, 16}; }
    uint16_t getMaterialAlbedoLayer(const std::string& key) const override {
        return key == "block1" ? 7 : 9;
    }
    uint16_t getMaterialNormalLayer(const std::string&) const override {
        return 0;
    }
    bgfx::TextureHandle getMaterialAlbedoArray() const override {
        return BGFX_INVALID_HANDLE;
    }
    bgfx::TextureHandle getMaterialNormalArray() const override {
        return BGFX_INVALID_HANDLE;
    }
};

inline Region regionAt(int surface) {
    Region region{};
    region.land_height_m = surface;
    region.height = 0.75f;
    region.crust = CrustType::CONTINENTAL;
    return region;
}

inline void fill(Chunk& chunk, uint8_t block = 0, uint8_t sun = 0) {
    for (auto& plane : chunk.tiles)
        for (auto& row : plane)
            for (auto& tile : row) {
                tile = Tile{};
                tile.block_id = block;
                tile.lightLevel.sun_exposure = sun;
            }
}

}  // namespace test
