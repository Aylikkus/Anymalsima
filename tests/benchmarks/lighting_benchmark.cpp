#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "test_assets.hpp"
#include "world/block_light_propagator.hpp"
#include "world/sunlight_propagator.hpp"

int main(int argc, char** argv) {
    try {
        const bool teleport = argc > 1 && std::string(argv[1]) == "teleport";
        const bool torches = argc > 1 && std::string(argv[1]) == "torches";
        test::Assets assets;
        World world{};
        Region region = test::regionAt(415);
        LocationGenerator generator;
        ThreadPool pool(8);
        ChunkManager chunks(generator, pool, nullptr, world, &assets);
        SunLightPropagator sun(chunks, assets.getStorage<Block>(), pool);
        for (unsigned z = 0; z < 17; ++z)
            for (unsigned y = 17; y < 34; ++y)
                for (unsigned x = 0; x < 17; ++x) {
                    const ChunkCoord c{x, y, z};
                    chunks.registerChunk(c, {}, &region);
                    chunks.request(c);
                }
#ifdef LIGHTING_BASELINE
        pool.wait();
        for (unsigned z = 0; z < 17; ++z)
            for (unsigned y = 17; y < 34; ++y)
                for (unsigned x = 0; x < 17; ++x) chunks.request({x, y, z});
#else
        while (chunks.hasGenerationWork()) {
            chunks.update();
            if (!pool.wait_for(std::chrono::seconds(10)))
                throw std::runtime_error("generation timed out");
        }
#endif
        chunks.forEachReady([](auto, Chunk& chunk) { test::fill(chunk); });
        ChunkCoord target{8, 25, 8};
        if (torches) {
            // A lamp every 8 tiles across 5^3 chunks around the target
            // (1000 lamps), then the near-field flood fill for all of it.
            int lamps = 0;
            chunks.forEachReady([&](const ChunkCoord& c, Chunk& chunk) {
                if (std::abs(int(c.x) - int(target.x)) > 2 ||
                    std::abs(int(c.y) - int(target.y)) > 2 ||
                    std::abs(int(c.z) - int(target.z)) > 2)
                    return;
                for (int z = 4; z < 16; z += 8)
                    for (int y = 4; y < 16; y += 8)
                        for (int x = 4; x < 16; x += 8) {
                            chunk.tiles[z][y][x].block_id = 4;
                            ++lamps;
                        }
            });
            BlockLightPropagator light(chunks, assets.getStorage<Block>(),
                                       pool);
            light.setFocus({float(target.x * 16 + 8), float(target.y * 16 + 8),
                            float(target.z * 16 + 8)});
            const auto begin = std::chrono::steady_clock::now();
            double worstUpdate = 0;
            int frames = 0;
            do {
                const auto t = std::chrono::steady_clock::now();
                light.update();
                worstUpdate = std::max(
                    worstUpdate, std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t)
                                     .count());
                if (!pool.wait_for(std::chrono::seconds(10)))
                    throw std::runtime_error("block light timed out");
                if (++frames > 100000)
                    throw std::runtime_error("block light did not settle");
            } while (!light.isIdle());
            std::cout << "scenario=torches lamps=" << lamps
                      << " frames=" << frames << " full_ms="
                      << std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - begin)
                             .count()
                      << " worst_update_ms=" << worstUpdate << '\n';
            return 0;
        }
        auto focus = [&] {
#ifndef LIGHTING_BASELINE
            sun.setFocus({float(target.x * 16 + 8), float(target.y * 16 + 8),
                          float(target.z * 16 + 8)});
#endif
        };
        focus();
        const auto start = std::chrono::steady_clock::now();
        auto targetStart = start;
        int targetFrame = 0, firstDirect = -1, firstComplete = -1, frame = 0;
        double directMs = -1, completeMs = -1;
        do {
            if (teleport && frame == 8) {
                target = {16, 25, 16};
                targetFrame = frame;
                targetStart = std::chrono::steady_clock::now();
                firstDirect = firstComplete = -1;
                focus();
            }
            sun.update();
            if (!pool.wait_for(std::chrono::seconds(10)))
                throw std::runtime_error("lighting worker timed out");
            const auto elapsed =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - targetStart)
                    .count();
#ifdef LIGHTING_BASELINE
            const bool direct = chunks.getChunk(target)
                                    ->tiles[8][8][8]
                                    .lightLevel.sun_exposure == 15;
            const bool complete = direct;
#else
            const bool direct = sun.hasDirect(target);
            const bool complete = sun.isComplete(target);
#endif
            if (firstDirect < 0 && direct) {
                firstDirect = frame - targetFrame;
                directMs = elapsed;
            }
            if (firstComplete < 0 && complete) {
                firstComplete = frame - targetFrame;
                completeMs = elapsed;
            }
            if (++frame > 10000)
                throw std::runtime_error("scheduler did not settle");
        } while (!sun.isIdle());
        std::cout << "scenario=" << (teleport ? "teleport" : "startup")
                  << " chunks=4913 workers=8 direct_frames=" << firstDirect
                  << " direct_ms=" << directMs
                  << " complete_frames=" << firstComplete
                  << " complete_ms=" << completeMs << " full_frames=" << frame
                  << " full_ms="
                  << std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count()
                  << '\n';
        // Frames are deterministic dispatch opportunities with a completion
        // barrier, not a simulated GPU or an enforced real-time frame cadence.
        return firstDirect < 0 || firstComplete < 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
