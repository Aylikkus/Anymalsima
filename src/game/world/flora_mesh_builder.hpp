#pragma once

#include <cstdint>
#include <vector>

#include "core/assets_api.hpp"
#include "items/flora.hpp"
#include "items/foliage_material.hpp"
#include "render/vertex.hpp"

// A plant mesh in plant-local space: base at the origin, +Y up, metres
// (= tiles). Shared by every instance of its (species, variant, stage);
// instances add rotation, scale, colour jitter and wind in the shader.
struct FloraMesh {
    std::vector<FoliageVertex> vertices;
    std::vector<uint16_t> indices;
    float height = 0;  // top of the mesh
    float radius = 0;  // horizontal extent from the base
    bool empty() const { return indices.empty(); }
};

// Builds plant meshes from Flora recipes (docs/handoff/flora.md, sections 3
// and 5). CPU only and deterministic in (species, variant, stage), so it
// runs anywhere, including workers. Part types:
//   stem          tapered, untextured strips (cylindrical normals)
//   blade         crossed tuft cards leaning outward, bent
//   leaf_card     single cards along stems (phyllotaxis) or at the base
//   leaf_clump    cards on an ellipsoid shell, spherical normals, so a bush
//                 shades as one soft volume
//   petal_ring    petals around stem tips
//   flower_head   a small up-facing disc at stem tips
//   fruit_cluster small cards on clump shells or stem tips
//   frond         arching, drooping cards around the base
// Colour: palette and gradient of the part's foliage material, per-card
// value variation (color_variation); the withered colour rides along.
class FloraMeshBuilder {
   public:
    static constexpr int Variants = 8;

    // assets must outlive the builder (Flora and FoliageMaterial storage).
    explicit FloraMeshBuilder(const IAssets& assets) : assets(assets) {}

    FloraMesh build(uint8_t species, int variant, FloraStage stage) const;

   private:
    const IAssets& assets;
};
