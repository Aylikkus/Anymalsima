#pragma once

#include "core/assets_api.hpp"
#include "flora_mesh_builder.hpp"
#include "items/foliage_material.hpp"
#include "items/tree.hpp"
#include "tree_skeleton.hpp"

// Crown meshes of trees (docs/handoff/trees.md, "Rendering"): everything of
// a tree that is not wood blocks. Built in tree-local space (anchor point at
// the origin, the skeleton's frame) as a FloraMesh, so the foliage program
// draws it instanced like plants:
//   - bark tubes: two crossed, tapered strips per branch segment thinner
//     than TreeVoxelRadius (untextured, cylindrical normals, bark palette);
//   - needle and leaf clumps: a few cards around each foliage site with
//     spherical normals and the material's centre-to-edge gradient;
//   - fronds: arching, drooping strips (palm leaves), root to tip.
// The vertex `height` attribute carries the wind weight (0 at the trunk, 1
// at the outer crown); the foliage program reads it in tree mode.
namespace tree_foliage {

// Levels of detail: Near has every clump and twig; Far keeps half the
// clumps (larger, fewer cards) and only main branches. Distant is for trees
// beyond the detailed chunks, whose wood blocks are not drawn: a sixth of
// the clumps (large, two cards), and the block wood itself as six-sided
// bark prisms (slightly thinner than the blocks, so near voxels win).
// DistantCrown is Distant without the prisms, for trees whose block wood is
// drawn as voxels at distance (landmarks, StructureLod).
enum class Detail : uint8_t { Near, Far, Distant, DistantCrown };

// Growth is drawn in steps of 1/Buckets of the tree's maturity.
inline constexpr int Buckets = 32;
inline int bucket(float age, float matureAge) {
    if (matureAge <= 0 || age >= matureAge) return Buckets;
    return std::clamp(int(age / matureAge * Buckets), 0, Buckets - 1);
}
// The age a bucket's mesh is built for.
inline float bucketAge(int bucket, float matureAge) {
    return bucket >= Buckets ? matureAge
                             : (float(bucket) + 0.5f) / Buckets * matureAge;
}

// bare: a dead tree, twigs only (its leaves have fallen).
FloraMesh build(const TreeSkeleton& skeleton, const Tree& species,
                const DataStorage<FoliageMaterial>& materials, float age,
                Detail detail, bool bare);

// A far billboard of a species, normalized: x and z span [-1, 1] (times the
// tree's crown radius), y spans [0, 1] (times its height); instances scale
// it per tree. Three crossed crown cards with the species' billboard
// silhouette (foliage.billboard_<conifer|broadleaf|palm|pine> by form)
// coloured from its leaf palette, and two crossed bark quads for the trunk;
// proportions from `shape` (a full-grown skeleton of the species). Empty
// when the billboard material is missing.
FloraMesh buildBillboard(const TreeSkeleton& shape, const Tree& species,
                         const DataStorage<FoliageMaterial>& materials);

}  // namespace tree_foliage
