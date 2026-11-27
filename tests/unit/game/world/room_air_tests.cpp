#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

#include "items/thermal_item.hpp"
#include "test.hpp"
#include "world/local_temperature.hpp"
#include "world/room_air.hpp"
#include "world/save/save_catalog.hpp"
#include "world/thermal_source_tracker.hpp"
#include "world/thermal_sources.hpp"
#include "world/world.hpp"
#include "world/world_editor.hpp"
#include "world/world_weather.hpp"
#include "world_fixture.hpp"

namespace {

bool near(double actual, double expected, double tolerance = 1e-4) {
    return std::abs(actual - expected) <= tolerance;
}

struct RoomFixture : test::SunlightFixture {
    ThermalSourceRegistry registry;
    WorldEditor editor{chunks, &sunlight, assets.getStorage<Block>()};
    ThermalSourceTracker tracker{chunks, assets.getStorage<Block>(), registry};
    room::RoomAir air{world, chunks, registry, thermal::voxelBarrier(chunks)};

    RoomFixture() {
        tracker.watch(editor);
        air.watch(editor);
    }
    ~RoomFixture() { pool.wait(); }

    static TilePos at(int x, int y, int z) { return {x, y + BASE_Y, z}; }

    void coldWorld() {
        world.width = 1;
        world.height = 1;
        world.regions.resize(1);
        world.regions[0].coord = {0, 0};
        world.regions[0].temperature_c = -40;
        // A valley floor well below the room makes the annual ground warmer
        // than the winter air, so opening the door has a clear direction.
        world.regions[0].land_height_m = BASE_Y + 2000;
        region.temperature_c = -40;
        region.land_height_m = BASE_Y + 2000;
    }

    void place(TilePos pos, uint8_t block) {
        EQUAL(int(editor.setBlock(pos, block)), int(EditStatus::Applied),
              "room fixture block edit");
    }

    // Interior x,z 4..6, y 1..3, walls at 3/7, roof at y 4. The door is the
    // two lower cells of the near wall, left open by `doorOpen`.
    void buildRoom(bool doorOpen, bool withRoof = true) {
        for (int z = 3; z <= 7; ++z)
            for (int x = 3; x <= 7; ++x) place(at(x, 0, z), 1);
        for (int y = 1; y <= 3; ++y)
            for (int z = 3; z <= 7; ++z)
                for (int x = 3; x <= 7; ++x) {
                    const bool boundary = x == 3 || x == 7 || z == 3 || z == 7;
                    if (!boundary) continue;
                    if (doorOpen && z == 3 && x == 5 && y <= 2) continue;
                    place(at(x, y, z), 1);
                }
        if (withRoof)
            for (int z = 3; z <= 7; ++z)
                for (int x = 3; x <= 7; ++x) place(at(x, 4, z), 1);
    }

    void openDoor() {
        place(at(5, 1, 3), 0);
        place(at(5, 2, 3), 0);
    }
    void closeDoor() {
        place(at(5, 1, 3), 1);
        place(at(5, 2, 3), 1);
    }
    std::array<double, 3> centre() const {
        return {5.5, double(BASE_Y) + 2.5, 5.5};
    }
};

}  // namespace

TEST(thermal_target_controller_schema_and_hysteresis) {
    const auto defaults = Json::object().get<ThermalEmission>();
    EQUAL(int(defaults.mode), int(ThermalMode::Fixed), "fixed by default");
    CHECK(defaults.thermostat() == false);

    const auto parsed =
        Json{{"mode", "target"}, {"delta_c", 0},       {"radius", 6},
             {"falloff", 1},     {"target_c", 21},     {"deadband_c", .5},
             {"gain", 4},        {"min_delta_c", -30}, {"max_delta_c", 60}}
            .get<ThermalEmission>();
    CHECK(parsed.thermostat());
    CHECK(near(parsed.commandedDelta(10), 44.0));
    CHECK(near(parsed.commandedDelta(-10), 60.0));
    CHECK(near(parsed.commandedDelta(40), -30.0));

    ThermalControllerState state;
    EQUAL(parsed.updateCommand(-10, state), 60.f, "cold room saturates high");
    CHECK(state.saturated);
    // Inside the deadband the command switches off (hysteresis).
    EQUAL(parsed.updateCommand(21.2, state), 0.f, "deadband off");
    EQUAL(parsed.updateCommand(15, state), 24.f, "proportional command");
    CHECK(!state.saturated);
    EQUAL(parsed.updateCommand(40, state), -30.f, "hot room saturates low");
    CHECK(state.saturated);

    ThermalEmission fixed;
    fixed.deltaC = 20;
    ThermalControllerState fixedState;
    EQUAL(fixed.updateCommand(-10, fixedState), 20.f, "fixed stays open-loop");
    CHECK(!fixedState.saturated);

    bool threw = false;
    try {
        (void)Json{{"mode", "target"}, {"gain", 0}}.get<ThermalEmission>();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)Json{{"mode", "sideways"}}.get<ThermalEmission>();
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(room_air_detects_sealed_and_leaky_rooms) {
    RoomFixture f;
    f.coldWorld();
    f.load({0, 0, 0});
    f.buildRoom(false);
    f.air.update(f.centre(), 1.0);
    const auto sealed = f.air.debugAt(f.centre());
    CHECK(sealed.valid);
    CHECK(sealed.volume >= 27);
    CHECK(sealed.wallFaces > 0);
    EQUAL(sealed.leakFaces, 0, "a sealed room has no leak faces");
    EQUAL(sealed.approximateFaces, 0, "no unloaded boundary in the fixture");

    f.openDoor();
    f.air.update(f.centre(), 1.05);
    const auto open = f.air.debugAt(f.centre());
    CHECK(open.valid);
    CHECK(open.volume > sealed.volume);
    CHECK(open.leakFaces > 0);
    CHECK(open.lastDay > sealed.lastDay);

    // A roof hole exposes the room: the ceiling scan no longer covers all
    // cells and the affected column becomes an outdoor leak.
    f.closeDoor();
    f.place(f.at(5, 4, 5), 0);
    f.air.update(f.centre(), 1.1);
    // The column under the missing roof piece is now exposed and belongs to
    // the outdoors; the remaining covered ring is still a room.
    const std::array<double, 3> corner{4.5, double(RoomFixture::BASE_Y) + 2.5,
                                       4.5};
    const auto hole = f.air.debugAt(corner);
    CHECK(hole.valid);
    CHECK(hole.volume < open.volume);
    CHECK(!f.air.debugAt(f.centre()).valid);
}

TEST(room_air_walls_store_heat_and_door_leakage_cools_air) {
    RoomFixture sealed;
    sealed.coldWorld();
    sealed.load({0, 0, 0});
    sealed.buildRoom(false);
    sealed.place(sealed.at(4, 1, 5), 8);  // fixed heater on the interior wall
    sealed.air.update(sealed.centre(), 1.0);
    for (int i = 1; i <= 12; ++i)
        sealed.air.update(sealed.centre(), 1.0 + i * .05);
    const auto warm = sealed.air.debugAt(sealed.centre());
    CHECK(warm.valid);
    CHECK(warm.sources > 0);
    CHECK(warm.airC > warm.groundC + 1.f);

    // Turn the fixed heater off. The wall remains above the ground baseline
    // and releases heat into the air as both cool.
    sealed.place(sealed.at(4, 1, 5), 1);
    sealed.air.update(sealed.centre(), 1.3);
    sealed.air.update(sealed.centre(), 2.0);
    const auto cooling = sealed.air.debugAt(sealed.centre());
    CHECK(cooling.airC < warm.airC);
    CHECK(cooling.wallC > cooling.groundC + .1f);
    CHECK(cooling.airC > -80.f);

    // The same warm room with an open door loses its air heat much faster.
    RoomFixture leaky;
    leaky.coldWorld();
    leaky.load({0, 0, 0});
    leaky.buildRoom(false);
    leaky.place(leaky.at(4, 1, 5), 8);
    leaky.air.update(leaky.centre(), 1.0);
    for (int i = 1; i <= 12; ++i)
        leaky.air.update(leaky.centre(), 1.0 + i * .05);
    leaky.place(leaky.at(4, 1, 5), 1);
    leaky.openDoor();
    leaky.air.update(leaky.centre(), 1.3);
    leaky.air.update(leaky.centre(), 2.0);
    const auto opened = leaky.air.debugAt(leaky.centre());
    CHECK(opened.leakFaces > 0);
    CHECK(opened.airC < cooling.airC);

    // The query sees the room air and does not add the source twice.
    thermal::Query query(sealed.world, sealed.registry,
                         thermal::voxelBarrier(sealed.chunks));
    query.setRoomProvider([&](std::array<double, 3> position, double days,
                              thermal::RoomSample& out) {
        return sealed.air.sample(position, days, out);
    });
    const auto sample = query.at(sealed.centre(), 2.0);
    CHECK(sample.indoors);
    EQUAL(sample.detailedC, 0.f, "room air owns the source contribution");
    CHECK(near(sample.temperatureC, sample.roomC));
    CHECK(near(sample.roomC, cooling.airC, .05));
}

TEST(room_air_thermostat_saturates_with_an_open_door) {
    RoomFixture f;
    f.coldWorld();
    f.load({0, 0, 0});
    f.buildRoom(false);
    f.place(f.at(4, 1, 5), 10);  // target 21 °C thermostat
    f.air.update(f.centre(), 1.0);
    for (int i = 1; i <= 80; ++i) f.air.update(f.centre(), 1.0 + i * .1);
    const auto settled = f.air.debugAt(f.centre());
    CHECK(settled.valid);
    CHECK(settled.sources > 0);
    CHECK(std::abs(settled.airC - 21.f) < 3.f);

    f.openDoor();
    float peakOpenCommand = 0;
    bool openSaturated = false;
    for (int i = 1; i <= 30; ++i) {
        f.air.update(f.centre(), 9.0 + i * .1);
        const auto d = f.air.debugAt(f.centre());
        peakOpenCommand = std::max(peakOpenCommand, d.commandC);
        openSaturated |= d.saturated;
    }
    const auto open = f.air.debugAt(f.centre());
    CHECK(open.leakFaces > 0);
    CHECK(open.airC < 21.f);
    CHECK(peakOpenCommand >= 59.f);
    CHECK(openSaturated);

    f.closeDoor();
    for (int i = 1; i <= 80; ++i) f.air.update(f.centre(), 12.0 + i * .1);
    const auto closed = f.air.debugAt(f.centre());
    CHECK(std::abs(closed.airC - 21.f) < 3.f);
    CHECK(closed.leakFaces == 0);
}

TEST(room_air_store_roundtrip) {
    RoomFixture f;
    f.coldWorld();
    f.load({0, 0, 0});
    f.buildRoom(false);
    f.place(f.at(4, 1, 5), 8);
    f.air.update(f.centre(), 1.0);
    for (int i = 1; i <= 10; ++i) f.air.update(f.centre(), 1.0 + i * .05);
    const auto records = f.air.saveRecords();
    CHECK(!records.empty());
    const auto before = f.air.debugAt(f.centre());

    std::vector<thermal_sources_codec::RoomRecord> stored;
    for (const auto& record : records)
        stored.push_back({record.x, record.y, record.z, record.airC,
                          record.wallC, record.lastDay});
    const auto root =
        std::filesystem::path(TEST_SCRATCH_DIR) / "room_air_store";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const SaveSlot slot{root, {}};
    save_catalog::writeThermalSources(slot, {}, stored);
    const auto file = save_catalog::readThermalSources(slot);
    EQUAL(file.rooms.size(), stored.size(), "room stores round-trip");
    CHECK(file.rooms[0] == stored[0]);

    room::RoomAir restored(f.world, f.chunks, f.registry,
                           thermal::voxelBarrier(f.chunks));
    std::vector<room::Record> loaded;
    for (const auto& record : file.rooms)
        loaded.push_back({record.x, record.y, record.z, record.airC,
                          record.wallC, record.lastDay});
    restored.loadRecords(loaded);
    restored.update(f.centre(), before.lastDay);
    const auto after = restored.debugAt(f.centre());
    CHECK(after.valid);
    CHECK(near(after.airC, before.airC, .1));
    CHECK(near(after.wallC, before.wallC, .1));

    std::filesystem::remove_all(root);
}

TEST(room_air_records_reject_bad_room_sections) {
    using thermal_sources_codec::RoomRecord;
    std::vector<RoomRecord> rooms{{1, 2, 3, 20, 10, 4.5}};
    const auto encoded = thermal_sources_codec::encode({}, rooms);
    const auto decoded = thermal_sources_codec::decode(encoded);
    EQUAL(decoded.rooms.size(), size_t(1), "one decoded room");
    CHECK(decoded.rooms[0] == rooms[0]);

    bool threw = false;
    try {
        (void)thermal_sources_codec::decode(
            ByteView(encoded).first(encoded.size() - 2));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}
