#include <cmath>

#include "items/block.hpp"
#include "test.hpp"
#include "world/block_light_propagator.hpp"

namespace {
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

bool throws(const Json& j) {
    try {
        (void)j.get<Block>();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

Json block(Json light) {
    return {{"name", "blocks.lamp"},
            {"material", "materials.lamp"},
            {"light", std::move(light)}};
}
}  // namespace

TEST(light_emission_defaults) {
    const Json plain = {{"name", "blocks.stone"}, {"material", "m"}};
    CHECK(!plain.get<Block>().light);

    const auto lamp = block(Json::object()).get<Block>();
    CHECK(lamp.light.has_value());
    EQUAL(lamp.light->radius, 15.f, "default radius");
    EQUAL(lamp.light->falloff, 1.f, "default falloff");
    EQUAL(lamp.light->intensity, 1.f, "default intensity");
    CHECK(lamp.light->isWhite());
    CHECK(!lamp.light->cone);
    CHECK(!lamp.light->isFar());

    // "enabled": false means the block is not a light source at all.
    CHECK(!block({{"enabled", false}, {"radius", 30}}).get<Block>().light);

    const auto far = block({{"radius", 8000},
                            {"falloff", 2},
                            {"color", {1, 0.5, 0}},
                            {"cone",
                             {{"direction", {0, 0, 2}},
                              {"inner_angle", 10},
                              {"outer_angle", 25}}}})
                         .get<Block>();
    CHECK(far.light->isFar());
    CHECK(!far.light->isWhite());
    // Directions are normalized on load.
    CHECK(near(far.light->cone->direction[2], 1.f));
    EQUAL(far.light->cone->outerAngle, 25.f, "outer angle");
}

TEST(light_emission_rejects_invalid) {
    CHECK(throws(block({{"radius", 0}})));
    CHECK(throws(block({{"radius", 8193}})));
    CHECK(throws(block({{"falloff", 0}})));
    CHECK(throws(block({{"intensity", 1.5}})));
    CHECK(throws(block({{"color", {1, 2, 0}}})));
    CHECK(throws(block({{"color", {0, 0, 0}}})));
    CHECK(throws(block({{"cone", {{"direction", {0, 0, 0}}}}})));
    CHECK(
        throws(block({{"cone", {{"inner_angle", 40}, {"outer_angle", 30}}}})));
    CHECK(throws(block("not an object")));
    CHECK(!throws(block({{"radius", 8192}})));
}

TEST(light_emission_curve) {
    LightEmission e;
    e.radius = 10;
    e.falloff = 2;
    e.intensity = 0.5f;
    CHECK(near(e.attenuation(0), 0.5f));
    CHECK(near(e.attenuation(5), 0.5f * 0.25f));
    EQUAL(e.attenuation(10), 0.f, "zero at the radius");
    EQUAL(e.attenuation(20), 0.f, "zero beyond");

    e.cone = LightCone{{0, -1, 0}, 20, 40};
    const std::array<float, 3> down{0, -1, 0};
    CHECK(near(e.coneFactor({0, -1, 0}, down), 1.f));
    EQUAL(e.coneFactor({1, 0, 0}, down), 0.f, "outside the cone");
    const float a = 30.f * 3.14159265f / 180.f;
    const float mid = e.coneFactor({std::sin(a), -std::cos(a), 0}, down);
    CHECK(mid > 0.05f && mid < 0.95f);
}

TEST(light_chroma_encoding) {
    const auto white = BlockLightPropagator::encodeChroma({1, 1, 1});
    EQUAL(int(white[0]), 128, "white co");
    EQUAL(int(white[1]), 128, "white cg");
    // Scale does not matter, only the hue.
    CHECK(BlockLightPropagator::encodeChroma({0.2f, 0.2f, 0.2f}) == white);
    const auto red = BlockLightPropagator::encodeChroma({1, 0, 0});
    CHECK(red[0] > 200 && red[1] < 50);
    const auto blue = BlockLightPropagator::encodeChroma({0, 0, 1});
    CHECK(blue[0] < 50 && blue[1] < 50);
}
