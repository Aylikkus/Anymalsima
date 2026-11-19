#pragma once

#include <optional>
#include <utility>

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

// Growth is drawn in steps of 1/Buckets of the current growth phase: from
// `start` (0, or the latest regrowth's start: TreeSkeleton::
// growthPhaseStart) to the tree's maturity, so fresh regrowth on an old
// tree gets fine steps too.
inline constexpr int Buckets = 32;
inline int bucket(float age, float matureAge, float start = 0) {
    if (matureAge <= start || age >= matureAge) return Buckets;
    return std::clamp(int((age - start) / (matureAge - start) * Buckets), 0,
                      Buckets - 1);
}
// The age a bucket's mesh is built for: the bucket's start, so a mesh is
// never ahead of the wood blocks (grown for the exact day). Ahead, a
// branch's base would be left to blocks that do not exist yet and its thin
// rest drawn floating above them. A sapling's first bucket uses a quarter
// step (its start would be empty).
inline float bucketAge(int bucket, float matureAge, float start = 0) {
    if (bucket >= Buckets) return matureAge;
    const float step = (matureAge - start) / Buckets;
    return start + step * (bucket == 0 && start <= 0 ? 0.25f : float(bucket));
}

// The part of a segment drawn as bark strips at `age` (t from, t to): the
// visible part thinner than a block. None when blocks or nothing show it.
std::optional<std::pair<float, float>> tubeSpan(const TreeSkeleton& k,
                                                const TreeSegment& s,
                                                float age);

// bare: a dead tree, twigs only (its leaves have fallen). One mesh: the
// first part only for crowns beyond 16-bit indices (massive trees); use
// buildParts for those.
FloraMesh build(const TreeSkeleton& skeleton, const Tree& species,
                const DataStorage<FoliageMaterial>& materials, float age,
                Detail detail, bool bare);
// The crown as meshes of at most 65535 vertices each (one for most trees).
std::vector<FloraMesh> buildParts(const TreeSkeleton& skeleton,
                                  const Tree& species,
                                  const DataStorage<FoliageMaterial>& materials,
                                  float age, Detail detail, bool bare);

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
