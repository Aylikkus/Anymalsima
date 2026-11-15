#include "world_fixture.hpp"

namespace {
void step(test::WorldFixture& f) {
    f.chunks.update();
    f.sunlight.update();
    f.wait();
}
Vec3 focus(unsigned x, int y = 404) { return {float(x * 16 + 8), float(y), 8}; }
ChunkCoord lit(unsigned x, unsigned y = 1) {
    return test::SunlightFixture::coord({x, y, 0});
}
// A running gate makes queued-worker behavior deterministic without sleeps.
class Gate {
   public:
    explicit Gate(ThreadPool& pool) {
        auto started = entered.get_future();
        auto release = exit.get_future().share();
        worker = pool.submit_task(
            [this, release] {
                entered.set_value();
                release.wait();
            },
            BS::pr::highest);
        if (started.wait_for(std::chrono::seconds(3)) !=
            std::future_status::ready) {
            exit.set_value();
            worker.wait();
            throw std::runtime_error("worker gate did not start");
        }
    }
    ~Gate() {
        open();
        worker.wait();
    }
    void open() {
        if (!opened) {
            exit.set_value();
            opened = true;
        }
    }

   private:
    std::promise<void> entered, exit;
    std::future<void> worker;
    bool opened = false;
};
}  // namespace

TEST(priority_progressive_near_first) {
    test::SunlightFixture f;
    f.load({40, 1, 0});
    f.load({20, 1, 0});
    f.load({0, 1, 0});
    f.sunlight.setFocus(focus(0));
    step(f);  // direct near job
    step(f);  // publish direct, submit near indirect
    CHECK(f.sunlight.hasDirect(lit(0)));
    CHECK(!f.sunlight.isComplete(lit(0)));
    CHECK(!f.sunlight.hasDirect(lit(40)));
    f.expectSun(8, 20, 8, 15);
    step(f);
    CHECK(f.sunlight.isComplete(lit(0)));
    CHECK(!f.sunlight.isIdle());
    f.light();
}

TEST(priority_camera_teleport) {
    test::SunlightFixture f;
    for (unsigned x : {0u, 20u, 40u}) f.load({x, 1, 0});
    f.sunlight.setFocus(focus(0));
    step(f);
    f.sunlight.setFocus(focus(40));
    step(f);
    step(f);
    CHECK(f.sunlight.hasDirect(lit(40)));
    CHECK(!f.sunlight.hasDirect(lit(20)));
    step(f);
    CHECK(f.sunlight.isComplete(lit(40)));
    CHECK(!f.sunlight.isComplete(lit(0)));
    f.light();
}

TEST(priority_vertical_camera_move) {
    test::SunlightFixture f;
    f.load({0, 1, 0});
    f.load({0, 2, 0});
    f.sunlight.setFocus(focus(0));
    step(f);
    f.sunlight.setFocus(focus(0, 424));
    step(f);
    step(f);
    CHECK(f.sunlight.isComplete(lit(0, 2)));
    CHECK(!f.sunlight.isComplete(lit(0, 1)));
    f.light();
}

TEST(priority_arrivals_preserve_light) {
    test::SunlightFixture f;
    f.load({0, 1, 0});
    f.light();
    CHECK(f.sunlight.hasDirect(lit(0)));
    f.load({0, 0, 0});
    CHECK(f.sunlight.hasDirect(lit(0)));
    f.expectSun(8, 20, 8, 15);
    for (int i = 0; i < 4; ++i) {
        step(f);
        f.expectSun(8, 20, 8, 15);
    }
    f.light();
}

TEST(priority_continuous_arrivals) {
    test::SunlightFixture f;
    f.load({0, 1, 0});
    f.sunlight.setFocus(focus(0));
    for (unsigned i = 0; i < 12; ++i) {
        f.load({100 + i * 3, 1, 0});
        step(f);
        if (i >= 2) CHECK(f.sunlight.isComplete(lit(0)));
    }
    CHECK(!f.sunlight.isIdle());
    f.light();
}

TEST(priority_background_progress) {
    test::SunlightFixture f;
    f.load({1000, 1, 0});  // oldest request, always far from the focus
    for (unsigned i = 0; i < 40; ++i) f.load({i * 3, 1, 0});
    f.sunlight.setFocus(focus(0));
    for (int i = 0; i < 34 && !f.sunlight.isComplete(lit(1000)); ++i) step(f);
    CHECK(f.sunlight.isComplete(lit(1000)));
    CHECK(!f.sunlight.isIdle());
    f.light();
}

TEST(priority_generation_bounded_teleport) {
    test::WorldFixture f;
    for (unsigned x = 0; x < 16; ++x) {
        f.registerSlab({x, 0, 0});
        f.chunks.request({x, 0, 0});
    }
    Gate gate(f.pool);
    f.chunks.setFocus(focus(0, 8));
    f.chunks.update();
    EQUAL(f.chunks.generationInFlight(), 4u, "generation submission cap");
    EQUAL(f.chunks.pendingGeneration(), 12u, "requests stay outside pool");
    f.registerSlab({40, 0, 0});
    f.chunks.setFocus(focus(40, 8));
    f.chunks.request({40, 0, 0});
    gate.open();
    f.wait();
    f.chunks.update();  // one old batch finishes, new focus wins next dispatch
    f.wait();
    f.chunks.update();
    CHECK(f.chunks.isReady({40, 0, 0}));
    CHECK(!f.chunks.isReady({4, 0, 0}));
    CHECK(f.chunks.generationInFlight() <= 4);
}

TEST(priority_sky_dependency_height) {
    test::WorldFixture f;
    f.region = test::regionAt(700);
    // A slab far above the focus is urgent because the nearby lower chunk
    // needs it. Another ordinary request is closer to the slab itself.
    f.registerSlab({0, 43, 0});
    f.chunks.requestLighting({0, 43, 0}, {0, 0, 0});
    for (unsigned x = 5; x < 12; ++x) {
        f.registerSlab({x, 0, 0});
        f.chunks.request({x, 0, 0});
    }
    f.chunks.setFocus(focus(0, 8));
    f.chunks.update();
    f.wait();
    f.chunks.update();
    CHECK(f.chunks.isReady({0, 43, 0}));
    CHECK(!f.chunks.isReady({5, 0, 0}) || !f.chunks.isReady({8, 0, 0}));
}

TEST(priority_generation_replacement) {
    test::WorldFixture f;
    f.registerSlab({0, 0, 0});
    f.chunks.request({0, 0, 0});
    Gate gate(f.pool);
    f.chunks.update();
    f.chunks.unregister({0, 0, 0});  // must not wait on a submitted job
    f.region.land_height_m = 0;
    f.registerSlab({0, 0, 0});
    f.chunks.request({0, 0, 0});
    gate.open();
    while (f.chunks.hasGenerationWork()) {
        f.chunks.update();
        f.wait();
    }
    CHECK(f.chunks.isReady({0, 0, 0}));
    CHECK(f.chunks.tileAt(8, 1, 8)->isLiquid());
}

TEST(priority_dependency_camera_move) {
    test::WorldFixture f;
    f.region = test::regionAt(700);
    f.registerSlab({0, 43, 0});
    f.chunks.requestLighting({0, 43, 0}, {0, 0, 0});
    for (unsigned x = 1; x <= 6; ++x) {
        f.registerSlab({x, 43, 0});
        f.chunks.request({x, 43, 0});
    }
    // The focus has moved from the beneficiary to the sky slab itself.
    f.chunks.setFocus(focus(0, 696));
    f.chunks.update();
    f.wait();
    f.chunks.update();
    CHECK(f.chunks.isReady({0, 43, 0}));
    CHECK(!f.chunks.isReady({4, 43, 0}));
}

TEST(priority_worker_pool_urgent) {
    ThreadPool pool(1);
    Gate gate(pool);
    std::vector<int> order;
    auto normal = pool.submit_task([&] { order.push_back(1); });
    auto urgent = pool.submit_task([&] { order.push_back(2); }, BS::pr::high);
    gate.open();
    urgent.get();
    normal.get();
    EQUAL(order.at(0), 2, "urgent work bypasses queued normal work");
    EQUAL(order.at(1), 1, "ordinary work still completes");
}

TEST(priority_mesh_bounded_focus) {
    test::SunlightFixture f;
    for (unsigned x : {2u, 20u, 40u}) {
        for (ChunkCoord c : {ChunkCoord{x, 1, 1},
                             {x - 1, 1, 1},
                             {x + 1, 1, 1},
                             {x, 0, 1},
                             {x, 2, 1},
                             {x, 1, 0},
                             {x, 1, 2}})
            f.load(c);
    }
    // Drain existing CPU jobs before requesting three new remeshes.
    for (int i = 0; i < 3; ++i) {
        f.chunks.update();
        f.wait();
    }
    for (unsigned x : {2u, 20u, 40u})
        f.chunks.markDirty(f.coord({x, 1, 1}), true);
    f.chunks.setFocus({648, 408, 24});
    f.chunks.update();
    EQUAL(f.chunks.meshInFlight(), 2u, "mesh submission cap");
    CHECK(f.chunks.meshInFlight(f.coord({40, 1, 1})));
    CHECK(!f.chunks.meshInFlight(f.coord({2, 1, 1})));
    f.wait();
}

TEST(priority_lighting_submission_cap) {
    test::SunlightFixture f(2);
    for (unsigned x = 0; x < 8; ++x) f.load({x * 3, 1, 0});
    f.sunlight.update();
    CHECK(f.sunlight.inFlightCount() <= 2);
    f.light();
}

TEST(priority_queue_stable_age) {
    CameraWorkQueue queue;
    queue.insert({1, 0, 0});
    queue.insert({0, 0, 0});
    queue.insert({1, 0, 0});  // deduplication preserves request age
    const auto tied = queue.ordered({16, 8, 8}, false);
    CHECK((tied.front() == ChunkCoord{1, 0, 0}));
    EQUAL(queue.size(), 2u, "duplicate work is coalesced");
    CHECK((queue.ordered({0, 8, 8}, false).front() == ChunkCoord{0, 0, 0}));
    CHECK((queue.ordered({0, 8, 8}, true).front() == ChunkCoord{1, 0, 0}));
}
