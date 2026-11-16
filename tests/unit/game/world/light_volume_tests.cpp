#include <set>

#include "world/light_volume.hpp"
#include "world_fixture.hpp"

namespace {
using LightVolume::Cells;
using LightVolume::Volume;

// Distinct, deterministic light per chunk slot and cell.
Cells patterned(int seed) {
    Cells cells{};
    for (int z = 0; z < LightVolume::CellsZ; ++z)
        for (int y = 0; y < LightVolume::CellsY; ++y)
            for (int x = 0; x < LightVolume::CellsX; ++x) {
                const int i = LightVolume::cellIndex(x, y, z);
                cells[i] = uint8_t(seed * 37 + x * 3 + y * 5 + z * 7);
                cells[i + 1] = uint8_t(seed * 11 + x + y * 13 + z * 17);
            }
    return cells;
}
std::pair<int, int> texel(const Volume& v, int x, int y, int z) {
    const int i = LightVolume::texelIndex(x, y, z);
    return {v[i], v[i + 1]};
}
std::pair<int, int> cell(const Cells& c, int x, int y, int z) {
    const int i = LightVolume::cellIndex(x, y, z);
    return {c[i], c[i + 1]};
}
void expectSame(std::pair<int, int> actual, std::pair<int, int> expected,
                const std::string& context) {
    EQUAL(actual.first, expected.first, context + " sky");
    EQUAL(actual.second, expected.second, context + " block");
}
}  // namespace

TEST(light_volume_encoding) {
    test::Assets assets;
    const auto& blocks = assets.getStorage<Block>();
    Chunk chunk;
    test::fill(chunk);
    auto& air = chunk.tiles[1][2][3];
    air.lightLevel.sun_exposure = 5;
    air.lightLevel.artificial_light = 3;
    auto& opaque = chunk.tiles[4][4][4];  // block1: opacity 15
    opaque.block_id = 1;
    opaque.lightLevel.sun_exposure = 15;
    auto& partial = chunk.tiles[5][5][5];  // block2: opacity 3
    partial.block_id = 2;
    partial.lightLevel.sun_exposure = 4;
    auto& water = chunk.tiles[6][6][6];
    water.liquid = LiquidType::Water;
    water.lightLevel.sun_exposure = 7;
    EQUAL(int(tileOpacity(opaque, blocks)), 15, "opaque block");
    EQUAL(int(tileOpacity(water, blocks)), 8, "water");

    Cells cells{};
    LightVolume::encode(chunk, blocks, nullptr, cells);
    expectSame(cell(cells, 3, 2, 1), {85, 51}, "air scales 0-15 to 0-255");
    // Opaque cells hold no light: trilinear filtering darkens corners.
    expectSame(cell(cells, 4, 4, 4), {0, 0}, "opaque cell");
    expectSame(cell(cells, 5, 5, 5), {68, 0}, "partially opaque cell");
    expectSame(cell(cells, 6, 6, 6), {119, 0}, "water keeps its light");
}

TEST(light_volume_neighbor_borders) {
    std::array<Cells, 27> data;
    std::array<const Cells*, 27> chunks{};
    for (int i = 0; i < 27; ++i) {
        data[i] = patterned(i);
        chunks[i] = &data[i];
    }
    Volume volume{};
    LightVolume::build(chunks, volume);
    const auto& center = data[LightVolume::neighbor(0, 0, 0)];
    // Interior cell c lives at texel c + 1.
    expectSame(texel(volume, 1, 1, 1), cell(center, 0, 0, 0), "interior min");
    expectSame(texel(volume, 16, 9, 4), cell(center, 15, 8, 3), "interior");
    // Faces, edges and corners copy the matching neighbor cell.
    expectSame(texel(volume, 0, 5, 6),
               cell(data[LightVolume::neighbor(-1, 0, 0)], 15, 4, 5),
               "-x face");
    expectSame(texel(volume, 7, 17, 3),
               cell(data[LightVolume::neighbor(0, 1, 0)], 6, 0, 2), "+y face");
    expectSame(texel(volume, 0, 0, 9),
               cell(data[LightVolume::neighbor(-1, -1, 0)], 15, 15, 8),
               "-x-y edge");
    expectSame(texel(volume, 17, 17, 17),
               cell(data[LightVolume::neighbor(1, 1, 1)], 0, 0, 0),
               "+x+y+z corner");
    expectSame(texel(volume, 17, 0, 0),
               cell(data[LightVolume::neighbor(1, -1, -1)], 0, 15, 15),
               "+x-y-z corner");
}

TEST(light_volume_missing_neighbor) {
    const Cells center = patterned(4);
    std::array<const Cells*, 27> chunks{};
    chunks[LightVolume::neighbor(0, 0, 0)] = &center;
    Volume volume{};
    LightVolume::build(chunks, volume);
    // Missing chunks repeat the nearest edge cell instead of inventing
    // darkness or sky.
    expectSame(texel(volume, 0, 0, 0), cell(center, 0, 0, 0), "corner clamp");
    expectSame(texel(volume, 17, 5, 3), cell(center, 15, 4, 2), "face clamp");
    expectSame(texel(volume, 8, 17, 0), cell(center, 7, 15, 0), "edge clamp");
}

TEST(light_volume_shared_face) {
    // Chunks A and B touch across A's +x face. The filter samples within
    // texel centres [0.5, 17.5], so both volumes must agree on the two cell
    // planes either side of the face for the light to be seamless.
    const Cells a = patterned(1), b = patterned(2);
    std::array<const Cells*, 27> aChunks{}, bChunks{};
    aChunks[LightVolume::neighbor(0, 0, 0)] = &a;
    aChunks[LightVolume::neighbor(1, 0, 0)] = &b;
    bChunks[LightVolume::neighbor(0, 0, 0)] = &b;
    bChunks[LightVolume::neighbor(-1, 0, 0)] = &a;
    Volume av{}, bv{};
    LightVolume::build(aChunks, av);
    LightVolume::build(bChunks, bv);
    for (int z = 1; z <= 16; ++z)
        for (int y = 1; y <= 16; ++y) {
            expectSame(texel(av, 17, y, z), texel(bv, 1, y, z), "B's first");
            expectSame(texel(av, 16, y, z), texel(bv, 0, y, z), "A's last");
        }
}

TEST(light_volume_signals) {
    test::SunlightFixture f;
    std::set<std::tuple<unsigned, unsigned, unsigned>> lit;
    f.chunks.lightChanged.connect(
        [&](const ChunkCoord& c) { lit.insert({c.x, c.y, c.z}); });
    f.load({0, 0, 0});
    f.load({0, 1, 0});
    f.light();
    const auto low = test::SunlightFixture::coord({0, 0, 0});
    const auto high = test::SunlightFixture::coord({0, 1, 0});
    // Published sunlight announces light changes for both slabs.
    CHECK(lit.contains({low.x, low.y, low.z}));
    CHECK(lit.contains({high.x, high.y, high.z}));

    lit.clear();
    f.chunks.markLightChanged({9, 9, 9});  // not registered
    CHECK(lit.empty());
    f.chunks.markModified(low);  // edits change opacity
    CHECK(lit.contains({low.x, low.y, low.z}));
}
