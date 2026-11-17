#pragma once

#include "core/assets_api.hpp"
#include "items/flora.hpp"
#include "items/foliage_material.hpp"
#include "items/ground_cover.hpp"
#include "items/material.hpp"
#include "items/soil.hpp"

// Cross-references of ground covers, foliage materials and flora. Run after
// every data file has loaded, independently of manifest order. Forage items
// are not checked: the items type does not exist yet (docs/handoff/flora.md).
// Foliage texture keys are checked by validateFoliageTextures.
inline void validateFloraReferences(const IAssets& assets) {
    const auto soilExists = [&](const std::string& name) {
        return assets.hasStorage<Soil>() &&
               assets.getStorage<Soil>().indexByName.contains(name);
    };
    const auto storageHas = [&]<typename T>(const std::string& name) {
        return assets.hasStorage<T>() &&
               assets.getStorage<T>().indexByName.contains(name);
    };
    if (assets.hasStorage<GroundCover>())
        for (const auto& cover : assets.getStorage<GroundCover>().items) {
            const auto fail = [&](const std::string& what) {
                throw std::runtime_error(cover.name + ": " + what);
            };
            if (!storageHas.template operator()<Material>(
                    cover.overlay_material))
                fail("missing overlay material " + cover.overlay_material);
            for (const auto& soil : cover.soils.names)
                if (!soilExists(soil)) fail("unknown soil " + soil);
        }
    if (!assets.hasStorage<Flora>()) return;
    for (const auto& flora : assets.getStorage<Flora>().items) {
        const auto fail = [&](const std::string& what) {
            throw std::runtime_error(flora.name + ": " + what);
        };
        for (const auto& soil : flora.placement.soils.names)
            if (!soilExists(soil)) fail("unknown soil " + soil);
        for (const auto* list : {&flora.placement.ground_cover_required,
                                 &flora.placement.ground_cover_forbidden})
            for (const auto& cover : *list)
                if (!storageHas.template operator()<GroundCover>(cover))
                    fail("unknown ground cover " + cover);
        for (const auto& companion : flora.placement.companions)
            if (!storageHas.template operator()<Flora>(companion.species))
                fail("unknown companion " + companion.species);
        for (const auto& part : flora.parts)
            if (!storageHas.template operator()<FoliageMaterial>(part.material))
                fail("unknown foliage material " + part.material);
    }
}

// Foliage material textures ("foliage.<card>" under assets/materials/
// foliage/) must exist. hasTexture: whether the manifest loaded that key.
template <typename HasTexture>
void validateFoliageTextures(const IAssets& assets, HasTexture&& hasTexture) {
    if (!assets.hasStorage<FoliageMaterial>()) return;
    for (const auto& material : assets.getStorage<FoliageMaterial>().items) {
        if (!hasTexture(material.albedo))
            throw std::runtime_error(material.name + ": missing texture " +
                                     material.albedo);
        if (!material.normal.empty() && !hasTexture(material.normal))
            throw std::runtime_error(material.name + ": missing texture " +
                                     material.normal);
    }
}
