#pragma once

#include "core/assets_api.hpp"
#include "items/block.hpp"
#include "items/material.hpp"
#include "items/soil.hpp"

// Run after every data file has loaded, independently of manifest order.
inline void validateSoilReferences(const IAssets& assets) {
    if (!assets.hasStorage<Soil>()) return;
    const auto& blocks = assets.getStorage<Block>();
    const auto& materials = assets.getStorage<Material>();
    for (const auto& soil : assets.getStorage<Soil>().items) {
        const auto check = [&](const std::string& name) {
            try {
                const auto& block = blocks.get(name);
                if (block.renderCategory != BlockRenderCategory::TERRAIN_SOIL)
                    throw std::runtime_error("expected Terrain Soil block");
                materials.get(block.material);
            } catch (const std::exception& error) {
                throw std::runtime_error(soil.name + ": " + name + ": " +
                                         error.what());
            }
        };
        check(soil.block_name);
        if (soil.topsoil_block_name) check(*soil.topsoil_block_name);
    }
}
