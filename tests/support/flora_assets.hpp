#pragma once

#include <fstream>
#include <string>

#include "core/assets_api.hpp"
#include "items/block.hpp"
#include "items/flora.hpp"
#include "items/foliage_material.hpp"
#include "items/ground_cover.hpp"
#include "items/material.hpp"
#include "items/rock.hpp"
#include "items/soil.hpp"
#include "items/tree.hpp"
#include "test.hpp"

namespace test {
// Real data loaders for the soil and flora types, without GPU resources.
// Overlay materials ("materials.cover_*") map to texture layer CoverLayer.
class FloraAssets final : public IAssets {
   public:
    static constexpr uint16_t CoverLayer = 5;
    FloraAssets() {
        registerDataType<Material>();
        registerDataType<Rock>();
        registerDataType<Block>();
        registerDataType<Soil>();
        registerDataType<GroundCover>();
        registerDataType<FoliageMaterial>();
        registerDataType<Flora>();
        registerDataType<Tree>();
    }
    template <typename T>
    void load(const Json& j) {
        loaders.at(T::TypeName)(*this, j);
    }
    // Loads assets/data/<type>/<file> from the source tree.
    template <typename T>
    void loadContent(const std::string& relative) {
        std::ifstream input(std::string(SOIL_CONTENT_DIR) + "/" + relative);
        CHECK(input.good());
        load<T>(Json::parse(input));
    }
    Vec2u getTextureSize(const std::string&) override { return {16, 16}; }
    uint16_t getMaterialAlbedoLayer(const std::string& key) const override {
        return key.starts_with("materials.cover_") ? CoverLayer : 1;
    }
    uint16_t getMaterialNormalLayer(const std::string& key) const override {
        return getMaterialAlbedoLayer(key);
    }
    bgfx::TextureHandle getMaterialAlbedoArray() const override {
        return BGFX_INVALID_HANDLE;
    }
    bgfx::TextureHandle getMaterialNormalArray() const override {
        return BGFX_INVALID_HANDLE;
    }
};

template <typename F>
void expectThrows(F fn, const std::string& context) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception&) {
        threw = true;
    }
    EQUAL(threw, true, context);
}

// Every shipped definition the world generator and flora use.
inline void loadAllContent(FloraAssets& a) {
    for (const auto* file :
         {"materials/rocks.json", "materials/soils.json",
          "materials/vegetation.json", "materials/wood.json"})
        a.loadContent<Material>(file);
    for (const auto* file :
         {"rocks/igneous_extrusive.json", "rocks/igneous_intrusive.json",
          "rocks/metamorphic.json", "rocks/sedimentary.json"})
        a.loadContent<Rock>(file);
    for (const auto* file :
         {"blocks/rocks.json", "blocks/soils.json", "blocks/trees.json"})
        a.loadContent<Block>(file);
    a.loadContent<Soil>("soils/soils.json");
    a.loadContent<GroundCover>("ground_covers/ground_covers.json");
    a.loadContent<FoliageMaterial>("foliage_materials/foliage_materials.json");
    a.loadContent<Flora>("flora/flora.json");
    a.loadContent<Tree>("trees/trees.json");
}

// Loam with a topsoil variant and one meadow cover on Plains.
inline void loadMeadow(FloraAssets& a) {
    a.load<Material>(Json::array({{{"name", "materials.loam"},
                                   {"texture_albedo", "loam"},
                                   {"texture_normal", "loam_normal"}},
                                  {{"name", "materials.cover_meadow"},
                                   {"texture_albedo", "meadow"},
                                   {"texture_normal", "meadow_normal"}}}));
    a.load<Block>(Json::array({{{"name", "blocks.loam"},
                                {"material", "materials.loam"},
                                {"render_category", "Terrain Soil"}},
                               {{"name", "blocks.loam_topsoil"},
                                {"material", "materials.loam"},
                                {"render_category", "Terrain Soil"}}}));
    a.load<Soil>(
        Json::array({Json{{"name", "soils.loam"},
                          {"block_name", "blocks.loam"},
                          {"topsoil_block_name", "blocks.loam_topsoil"},
                          {"depth_m", {3, 3}},
                          {"properties",
                           {{"porosity", .4},
                            {"permeability", .5},
                            {"water_retention", .6},
                            {"fertility", .7},
                            {"organic_content", .02}}},
                          {"placement", Json::array({Json::object()})}}}));
    a.load<GroundCover>(Json::array(
        {Json{{"name", "ground_covers.meadow"},
              {"overlay_material", "materials.cover_meadow"},
              {"faces", {{"side_fringe_m", {0.2, 0.4}}}},
              {"placement", Json::array({{{"allowed_land", {"Plains"}}}})},
              {"density_noise",
               {{"scale_m", 12}, {"threshold", 0.2}, {"contrast", 2.0}}}}}));
}
}  // namespace test
